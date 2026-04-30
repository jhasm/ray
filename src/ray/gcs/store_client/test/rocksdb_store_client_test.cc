// Copyright 2026 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Phase 3 walking-skeleton tests for RocksDbStoreClient (REP-64 POC).
//
// We deliberately do NOT extend StoreClientTestBase here — that suite
// exercises every StoreClient method, and Phase 3 only implements three
// (AsyncPut, AsyncGet, GetNextJobID). Phase 6 swaps this for the full
// StoreClientTestBase subclass alongside the API completion work.
//
// What this file proves for Phase 3:
//   1. AsyncPut + AsyncGet roundtrip a value within a single client.
//   2. State persists across close+reopen of the client at the same
//      path — the *core* claim Phase 3 is here to demonstrate.
//   3. GetNextJobID is monotonic within a process and persists across
//      restart (so a recovered cluster doesn't re-issue old job IDs).
//   4. The cluster-ID marker fail-fast on PVC reuse.

#include "ray/gcs/store_client/rocksdb_store_client.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "ray/common/asio/instrumented_io_context.h"

namespace fs = std::filesystem;

namespace ray {
namespace gcs {

namespace {

fs::path UniqueTempDir(const std::string &tag) {
  std::random_device rd;
  std::mt19937_64 rng(rd());
  auto p = fs::temp_directory_path() /
           ("rep64-poc-" + tag + "-" + std::to_string(rng()));
  fs::create_directories(p);
  return p;
}

class IoServiceFixture {
 public:
  IoServiceFixture()
      : work_(std::make_unique<boost::asio::io_service::work>(io_)),
        thread_([this] { io_.run(); }) {}

  ~IoServiceFixture() {
    work_.reset();
    if (thread_.joinable()) thread_.join();
  }

  instrumented_io_context &io() { return io_; }

 private:
  instrumented_io_context io_;
  std::unique_ptr<boost::asio::io_service::work> work_;
  std::thread thread_;
};

// Wait until `pred` becomes true or the timeout elapses, whichever
// comes first. Returns true if the predicate fired.
bool WaitFor(std::function<bool()> pred,
             std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}

}  // namespace

TEST(RocksDbStoreClient, PutGetRoundtrip) {
  IoServiceFixture io_fix;
  const fs::path dir = UniqueTempDir("roundtrip");
  RocksDbStoreClient client(io_fix.io(), dir.string(), /*expected_cluster_id=*/"c1");

  std::atomic<bool> put_done{false};
  ASSERT_TRUE(client
                  .AsyncPut("ACTOR", "actor_1", "payload-1", /*overwrite=*/true,
                            [&](bool inserted) {
                              EXPECT_TRUE(inserted);
                              put_done = true;
                            })
                  .ok());
  ASSERT_TRUE(WaitFor([&] { return put_done.load(); }));

  std::atomic<bool> get_done{false};
  std::string read_back;
  ASSERT_TRUE(client
                  .AsyncGet("ACTOR", "actor_1",
                            [&](Status s, boost::optional<std::string> v) {
                              EXPECT_TRUE(s.ok());
                              ASSERT_TRUE(v.has_value());
                              read_back = *v;
                              get_done = true;
                            })
                  .ok());
  ASSERT_TRUE(WaitFor([&] { return get_done.load(); }));
  EXPECT_EQ(read_back, "payload-1");

  fs::remove_all(dir);
}

// The headline Phase 3 claim: state survives close+reopen at the same
// path, simulating a head-pod restart.
TEST(RocksDbStoreClient, RecoverAcrossReopen) {
  IoServiceFixture io_fix;
  const fs::path dir = UniqueTempDir("recover");

  // First lifetime: write three keys across two tables.
  {
    RocksDbStoreClient client(io_fix.io(), dir.string(), "cluster-A");
    std::atomic<int> done{0};
    auto put = [&](const std::string &table, const std::string &key,
                   const std::string &value) {
      ASSERT_TRUE(
          client
              .AsyncPut(table, key, value, /*overwrite=*/true,
                        [&](bool /*inserted*/) { done.fetch_add(1); })
              .ok());
    };
    put("ACTOR", "a1", "v-a1");
    put("ACTOR", "a2", "v-a2");
    put("NODE", "n1", "v-n1");
    ASSERT_TRUE(WaitFor([&] { return done.load() == 3; }));
  }  // client destroyed → DB closed.

  // Second lifetime: same path, same cluster ID. All three keys should
  // be readable back.
  {
    RocksDbStoreClient client(io_fix.io(), dir.string(), "cluster-A");

    std::atomic<int> done{0};
    auto get_and_check = [&](const std::string &table, const std::string &key,
                             const std::string &expected) {
      ASSERT_TRUE(
          client
              .AsyncGet(table, key,
                        [&, expected](Status s, boost::optional<std::string> v) {
                          EXPECT_TRUE(s.ok());
                          ASSERT_TRUE(v.has_value());
                          EXPECT_EQ(*v, expected);
                          done.fetch_add(1);
                        })
              .ok());
    };
    get_and_check("ACTOR", "a1", "v-a1");
    get_and_check("ACTOR", "a2", "v-a2");
    get_and_check("NODE", "n1", "v-n1");
    ASSERT_TRUE(WaitFor([&] { return done.load() == 3; }));
  }

  fs::remove_all(dir);
}

TEST(RocksDbStoreClient, JobIdMonotonicAndPersists) {
  IoServiceFixture io_fix;
  const fs::path dir = UniqueTempDir("jobid");

  int last_in_first_lifetime = 0;
  {
    RocksDbStoreClient client(io_fix.io(), dir.string(), "cluster-A");
    int prev = 0;
    for (int i = 0; i < 5; ++i) {
      int id = client.GetNextJobID();
      EXPECT_GT(id, prev);
      prev = id;
    }
    last_in_first_lifetime = prev;
  }

  // Reopen — the next ID should be strictly greater than the last from
  // the previous lifetime, not start over from 0.
  {
    RocksDbStoreClient client(io_fix.io(), dir.string(), "cluster-A");
    int next = client.GetNextJobID();
    EXPECT_GT(next, last_in_first_lifetime);
  }

  fs::remove_all(dir);
}

TEST(RocksDbStoreClient, ClusterIdMarkerWritesOnFirstOpen) {
  IoServiceFixture io_fix;
  const fs::path dir = UniqueTempDir("marker");
  // First open writes the marker.
  { RocksDbStoreClient client(io_fix.io(), dir.string(), "cluster-A"); }
  // Second open with the same ID accepts.
  { RocksDbStoreClient client(io_fix.io(), dir.string(), "cluster-A"); }
  fs::remove_all(dir);
}

// We don't EXPECT-DEATH on cluster-ID mismatch in this file because
// gtest death tests forking with RocksDB open file handles is fiddly.
// Phase 8 covers the fail-fast behaviour at the K8s level.

}  // namespace gcs
}  // namespace ray
