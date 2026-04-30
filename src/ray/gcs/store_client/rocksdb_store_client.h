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

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"
#include "ray/common/asio/instrumented_io_context.h"
#include "ray/gcs/store_client/store_client.h"
#include "rocksdb/db.h"

namespace ray {
namespace gcs {

/// \class RocksDbStoreClient
/// Embedded-storage StoreClient backed by RocksDB on a local persistent
/// volume. Implements the GCS fault-tolerance contract proposed in
/// REP-64 (`enhancements/reps/2026-02-23-gcs-embedded-storage.md`).
///
/// **Phase 3 (walking skeleton):** `AsyncPut`, `AsyncGet`, and
/// `GetNextJobID` implemented; remaining methods stubbed.
///
/// **Phase 6 (API completeness):** `AsyncGetAll`, `AsyncMultiGet`,
/// `AsyncDelete`, `AsyncBatchDelete`, `AsyncGetKeys` (prefix scan),
/// `AsyncExists` filled in. The full `StoreClientTestBase` suite is
/// exercised via the POC harness target
/// `//rep-64-poc/harness/store_client_parity:rocksdb_parity_test`.
///
/// **Threading.** Like `InMemoryStoreClient`, RocksDB calls are made
/// inline on the caller thread and the user-supplied callback is
/// `post()`-ed onto `main_io_service_` so callback ordering matches the
/// rest of GCS. RocksDB's local ops are fast enough that blocking the
/// caller is acceptable for this phase; if Phase 7 measurements show
/// event-loop starvation we will revisit by dispatching to an I/O
/// worker thread.
///
/// **Durability.** Every mutating call uses `WriteOptions::sync = true`
/// so the WAL is fsynced before the callback fires. This is the
/// invariant Ray's GCS RPC layer relies on: a caller that received an
/// ack can assume the write survived a crash. Whether `fsync` actually
/// flushes to media is a property of the underlying volume and is the
/// subject of Phase 4.
class RocksDbStoreClient : public StoreClient {
 public:
  /// Open or create a RocksDB at \p db_path and validate the cluster-ID
  /// marker.
  ///
  /// \param main_io_service The GCS event loop. All callbacks are
  ///   posted here.
  /// \param db_path Filesystem path on a persistent volume. Must
  ///   already exist (the operator-managed PVC mounts it).
  /// \param expected_cluster_id If non-empty: enforce that any existing
  ///   marker matches; if there's no marker yet, write this value. If
  ///   empty: accept whatever is there (or write a fresh UUID on first
  ///   open). The non-empty case is the production path; empty is
  ///   meant for unit tests where the cluster ID is not material.
  RocksDbStoreClient(instrumented_io_context &main_io_service,
                     const std::string &db_path,
                     const std::string &expected_cluster_id);

  ~RocksDbStoreClient() override;

  RocksDbStoreClient(const RocksDbStoreClient &) = delete;
  RocksDbStoreClient &operator=(const RocksDbStoreClient &) = delete;

  // ---- Implemented in Phase 3. -----------------------------------------

  Status AsyncPut(const std::string &table_name,
                  const std::string &key,
                  const std::string &data,
                  bool overwrite,
                  std::function<void(bool)> callback) override;

  Status AsyncGet(const std::string &table_name,
                  const std::string &key,
                  const OptionalItemCallback<std::string> &callback) override;

  int GetNextJobID() override;

  // ---- Implemented in Phase 6. -----------------------------------------

  Status AsyncGetAll(const std::string &table_name,
                     const MapCallback<std::string, std::string> &callback) override;

  Status AsyncMultiGet(const std::string &table_name,
                       const std::vector<std::string> &keys,
                       const MapCallback<std::string, std::string> &callback) override;

  Status AsyncDelete(const std::string &table_name,
                     const std::string &key,
                     std::function<void(bool)> callback) override;

  Status AsyncBatchDelete(const std::string &table_name,
                          const std::vector<std::string> &keys,
                          std::function<void(int64_t)> callback) override;

  Status AsyncGetKeys(const std::string &table_name,
                      const std::string &prefix,
                      std::function<void(std::vector<std::string>)> callback) override;

  Status AsyncExists(const std::string &table_name,
                     const std::string &key,
                     std::function<void(bool)> callback) override;

 private:
  /// Look up the column family for \p table_name, creating it lazily on
  /// first use. The returned handle is owned by `db_`.
  rocksdb::ColumnFamilyHandle *GetOrCreateColumnFamily(
      const std::string &table_name) ABSL_LOCKS_EXCLUDED(cf_mutex_);

  /// Write the cluster-ID marker on first open or validate it on
  /// subsequent opens. RAY_CHECK-fails on mismatch.
  void ValidateOrWriteClusterIdMarker(const std::string &expected_cluster_id);

  /// Reserved key in the default column family that holds the cluster
  /// ID marker (REP §"Stale data protection").
  static constexpr char kClusterIdKey[] = "__ray_rep64_cluster_id__";
  /// Reserved key in the default column family that holds the
  /// monotonically increasing job counter, mirroring Redis's `INCR
  /// JobCounter`.
  static constexpr char kJobCounterKey[] = "__ray_rep64_job_counter__";

  instrumented_io_context &main_io_service_;

  // RocksDB itself owns all column-family handles.
  std::unique_ptr<rocksdb::DB> db_;

  // table_name → CF handle. `default` is always present.
  absl::Mutex cf_mutex_;
  absl::flat_hash_map<std::string, rocksdb::ColumnFamilyHandle *> cf_handles_
      ABSL_GUARDED_BY(cf_mutex_);

  // Mutex-RMW path for `GetNextJobID`. The current value is also
  // persisted after every increment, so the counter survives restarts.
  // Phase 5 evaluates whether to keep this approach or switch to a
  // RocksDB Merge operator.
  absl::Mutex job_id_mutex_;
  int job_id_ ABSL_GUARDED_BY(job_id_mutex_) = 0;
};

}  // namespace gcs
}  // namespace ray
