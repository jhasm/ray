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

#include "ray/gcs/store_client/rocksdb_store_client.h"

#include <algorithm>
#include <utility>

#include "ray/util/logging.h"
#include "rocksdb/options.h"

namespace ray {
namespace gcs {

namespace {

constexpr char kDefaultCFName[] = "default";

rocksdb::Options BuildDbOptions() {
  rocksdb::Options options;
  options.create_if_missing = true;
  options.create_missing_column_families = true;
  // Per REP: hardcoded conservative settings sized for the GCS metadata
  // workload (10–100 MB). Phase 6 / Phase 7 will revisit some of these
  // (compression at L2+, write buffer sizing). Phase 3 keeps them at
  // RocksDB defaults except where the REP makes them non-negotiable.
  options.IncreaseParallelism(2);
  options.OptimizeLevelStyleCompaction();
  return options;
}

rocksdb::WriteOptions SyncWriteOptions() {
  rocksdb::WriteOptions wo;
  // REP §"Durability and Consistency Semantics": fsync per write is the
  // contract. Phase 4 verifies this on real substrates.
  wo.sync = true;
  return wo;
}

}  // namespace

RocksDbStoreClient::RocksDbStoreClient(instrumented_io_context &main_io_service,
                                       const std::string &db_path,
                                       const std::string &expected_cluster_id)
    : main_io_service_(main_io_service) {
  RAY_CHECK(!db_path.empty()) << "RAY_GCS_STORAGE_PATH must be set when "
                                 "RAY_GCS_STORAGE=rocksdb.";

  // List existing CFs so we can open them all. On a fresh DB this
  // returns just `default`.
  rocksdb::Options list_options;
  std::vector<std::string> cf_names;
  auto list_status =
      rocksdb::DB::ListColumnFamilies(list_options, db_path, &cf_names);
  if (!list_status.ok()) {
    // Fresh DB — only the default CF will exist after first Open.
    cf_names = {kDefaultCFName};
  }
  if (std::find(cf_names.begin(), cf_names.end(), kDefaultCFName) ==
      cf_names.end()) {
    cf_names.push_back(kDefaultCFName);
  }

  std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
  descriptors.reserve(cf_names.size());
  for (const auto &name : cf_names) {
    descriptors.emplace_back(name, rocksdb::ColumnFamilyOptions());
  }

  std::vector<rocksdb::ColumnFamilyHandle *> handles;
  rocksdb::DB *raw_db = nullptr;
  auto open_status =
      rocksdb::DB::Open(BuildDbOptions(), db_path, descriptors, &handles, &raw_db);
  RAY_CHECK(open_status.ok())
      << "Failed to open RocksDB at " << db_path << ": " << open_status.ToString();
  db_.reset(raw_db);

  RAY_CHECK_EQ(handles.size(), descriptors.size());
  {
    absl::MutexLock lock(&cf_mutex_);
    for (size_t i = 0; i < handles.size(); ++i) {
      cf_handles_[descriptors[i].name] = handles[i];
    }
  }

  ValidateOrWriteClusterIdMarker(expected_cluster_id);

  // Recover the persisted job counter. Default 0 if the DB is fresh.
  std::string counter_value;
  auto get_status = db_->Get(rocksdb::ReadOptions(),
                             db_->DefaultColumnFamily(),
                             kJobCounterKey,
                             &counter_value);
  if (get_status.ok()) {
    try {
      absl::MutexLock lock(&job_id_mutex_);
      job_id_ = std::stoi(counter_value);
    } catch (...) {
      RAY_LOG(FATAL) << "RocksDB job counter is corrupt at " << db_path
                     << ": " << counter_value;
    }
  } else {
    RAY_CHECK(get_status.IsNotFound())
        << "Unexpected RocksDB Get error for job counter: "
        << get_status.ToString();
  }
}

RocksDbStoreClient::~RocksDbStoreClient() {
  // Release CF handles before DB closes.
  absl::MutexLock lock(&cf_mutex_);
  for (auto &[_, handle] : cf_handles_) {
    db_->DestroyColumnFamilyHandle(handle);
  }
  cf_handles_.clear();
}

void RocksDbStoreClient::ValidateOrWriteClusterIdMarker(
    const std::string &expected_cluster_id) {
  std::string existing;
  auto get_status = db_->Get(rocksdb::ReadOptions(),
                             db_->DefaultColumnFamily(),
                             kClusterIdKey,
                             &existing);

  if (get_status.IsNotFound()) {
    // First open. Write the marker if we have one; otherwise leave the
    // DB unmarked (acceptable in unit-test mode).
    if (!expected_cluster_id.empty()) {
      auto put_status = db_->Put(SyncWriteOptions(),
                                 db_->DefaultColumnFamily(),
                                 kClusterIdKey,
                                 expected_cluster_id);
      RAY_CHECK(put_status.ok())
          << "Failed to write cluster ID marker: " << put_status.ToString();
    }
    return;
  }

  RAY_CHECK(get_status.ok())
      << "Unexpected RocksDB Get error for cluster marker: "
      << get_status.ToString();

  // REP §"Stale data protection": fail-fast if the marker disagrees,
  // rather than silently loading another cluster's state.
  if (!expected_cluster_id.empty() && existing != expected_cluster_id) {
    RAY_LOG(FATAL) << "RocksDB at this path belongs to cluster '" << existing
                   << "' but this GCS expected cluster '" << expected_cluster_id
                   << "'. Refusing to load stale state.";
  }
}

rocksdb::ColumnFamilyHandle *RocksDbStoreClient::GetOrCreateColumnFamily(
    const std::string &table_name) {
  // Hold cf_mutex_ across the CreateColumnFamily call so two concurrent
  // first-touches of the same table don't both try to create it: RocksDB
  // serialises CreateColumnFamily internally but rejects a second
  // create on the same name as Status::InvalidArgument, which we'd then
  // RAY_CHECK on. The lock is only contended on the first touch per
  // table per process; the (table_name -> handle) cache fast-path in
  // the early return means steady-state lookups are uncontended-cheap.
  absl::MutexLock lock(&cf_mutex_);
  auto it = cf_handles_.find(table_name);
  if (it != cf_handles_.end()) {
    return it->second;
  }
  rocksdb::ColumnFamilyHandle *new_handle = nullptr;
  auto status = db_->CreateColumnFamily(rocksdb::ColumnFamilyOptions(),
                                        table_name,
                                        &new_handle);
  RAY_CHECK(status.ok()) << "Failed to create column family '" << table_name
                         << "': " << status.ToString();
  cf_handles_[table_name] = new_handle;
  return new_handle;
}

Status RocksDbStoreClient::AsyncPut(const std::string &table_name,
                                    const std::string &key,
                                    const std::string &data,
                                    bool overwrite,
                                    std::function<void(bool)> callback) {
  auto *cf = GetOrCreateColumnFamily(table_name);

  bool inserted = true;
  if (!overwrite) {
    // Honour the !overwrite contract: only write if the key is absent.
    // This is racy against concurrent writers; GCS's single-writer model
    // makes that fine in practice.
    std::string existing;
    auto get_status = db_->Get(rocksdb::ReadOptions(), cf, key, &existing);
    if (get_status.ok()) {
      inserted = false;
      if (callback) {
        main_io_service_.post([callback]() { callback(false); },
                              "GcsRocksDb.PutSkip");
      }
      return Status::OK();
    }
    RAY_CHECK(get_status.IsNotFound())
        << "RocksDB Get failed: " << get_status.ToString();
  } else {
    // For overwrite: distinguish insert vs update so the callback's
    // bool is accurate.
    std::string existing;
    auto get_status = db_->Get(rocksdb::ReadOptions(), cf, key, &existing);
    if (get_status.ok()) {
      inserted = false;
    } else {
      RAY_CHECK(get_status.IsNotFound())
          << "RocksDB Get failed: " << get_status.ToString();
    }
  }

  auto put_status = db_->Put(SyncWriteOptions(), cf, key, data);
  RAY_CHECK(put_status.ok()) << "RocksDB Put failed for table=" << table_name
                             << " key=" << key << ": "
                             << put_status.ToString();

  if (callback) {
    main_io_service_.post([callback, inserted]() { callback(inserted); },
                          "GcsRocksDb.Put");
  }
  return Status::OK();
}

Status RocksDbStoreClient::AsyncGet(
    const std::string &table_name,
    const std::string &key,
    const OptionalItemCallback<std::string> &callback) {
  RAY_CHECK(callback != nullptr);
  auto *cf = GetOrCreateColumnFamily(table_name);

  std::string raw_value;
  auto status = db_->Get(rocksdb::ReadOptions(), cf, key, &raw_value);

  boost::optional<std::string> data;
  if (status.ok()) {
    data = std::move(raw_value);
  } else if (!status.IsNotFound()) {
    RAY_LOG(FATAL) << "RocksDB Get failed for table=" << table_name
                   << " key=" << key << ": " << status.ToString();
  }
  main_io_service_.post(
      [callback, data = std::move(data)]() { callback(Status::OK(), data); },
      "GcsRocksDb.Get");
  return Status::OK();
}

int RocksDbStoreClient::GetNextJobID() {
  absl::MutexLock lock(&job_id_mutex_);
  job_id_ += 1;
  // Persist so the counter survives restart. fsync semantics match the
  // rest of the StoreClient writes.
  auto status = db_->Put(SyncWriteOptions(),
                         db_->DefaultColumnFamily(),
                         kJobCounterKey,
                         std::to_string(job_id_));
  RAY_CHECK(status.ok()) << "RocksDB Put for job counter failed: "
                         << status.ToString();
  return job_id_;
}

// ---- Phase 6: full StoreClient API surface. -------------------------------
// All methods mirror the Phase 3 dispatch pattern: do RocksDB work
// synchronously (AsyncPut already blocks on the WAL fsync because
// SyncWriteOptions sets sync=true), then post the callback onto
// main_io_service_ so callers see consistent async semantics.

Status RocksDbStoreClient::AsyncGetAll(
    const std::string &table_name,
    const MapCallback<std::string, std::string> &callback) {
  RAY_CHECK(callback != nullptr);
  auto *cf = GetOrCreateColumnFamily(table_name);

  rocksdb::ReadOptions read_opts;
  read_opts.total_order_seek = true;  // ignore prefix-extractor for full scan
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_opts, cf));

  absl::flat_hash_map<std::string, std::string> result;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    result.emplace(it->key().ToString(), it->value().ToString());
  }
  RAY_CHECK(it->status().ok())
      << "RocksDB iterator failed during GetAll: " << it->status().ToString();

  main_io_service_.post(
      [callback, r = std::move(result)]() mutable { callback(std::move(r)); },
      "GcsRocksDb.GetAll");
  return Status::OK();
}

Status RocksDbStoreClient::AsyncMultiGet(
    const std::string &table_name,
    const std::vector<std::string> &keys,
    const MapCallback<std::string, std::string> &callback) {
  RAY_CHECK(callback != nullptr);
  absl::flat_hash_map<std::string, std::string> result;
  if (keys.empty()) {
    main_io_service_.post(
        [callback, r = std::move(result)]() mutable { callback(std::move(r)); },
        "GcsRocksDb.MultiGet");
    return Status::OK();
  }

  auto *cf = GetOrCreateColumnFamily(table_name);
  std::vector<rocksdb::ColumnFamilyHandle *> cfs(keys.size(), cf);
  std::vector<rocksdb::Slice> key_slices;
  key_slices.reserve(keys.size());
  for (const auto &k : keys) key_slices.emplace_back(k);

  std::vector<std::string> values;
  std::vector<rocksdb::Status> statuses =
      db_->MultiGet(rocksdb::ReadOptions(), cfs, key_slices, &values);
  RAY_CHECK_EQ(statuses.size(), keys.size());

  for (size_t i = 0; i < keys.size(); ++i) {
    if (statuses[i].ok()) {
      result.emplace(keys[i], std::move(values[i]));
    } else if (!statuses[i].IsNotFound()) {
      RAY_LOG(FATAL) << "RocksDB MultiGet failed for key=" << keys[i] << ": "
                     << statuses[i].ToString();
    }
    // NotFound: simply omit from the map, mirroring Redis MGET semantics.
  }

  main_io_service_.post(
      [callback, r = std::move(result)]() mutable { callback(std::move(r)); },
      "GcsRocksDb.MultiGet");
  return Status::OK();
}

Status RocksDbStoreClient::AsyncDelete(const std::string &table_name,
                                       const std::string &key,
                                       std::function<void(bool)> callback) {
  auto *cf = GetOrCreateColumnFamily(table_name);

  // Determine prior existence so the callback's bool is "did this delete
  // remove an existing entry" — same contract as InMemoryStoreClient.
  std::string existing;
  auto get_status = db_->Get(rocksdb::ReadOptions(), cf, key, &existing);
  bool existed = get_status.ok();
  if (!existed && !get_status.IsNotFound()) {
    RAY_LOG(FATAL) << "RocksDB Get during Delete failed: "
                   << get_status.ToString();
  }

  auto del_status = db_->Delete(SyncWriteOptions(), cf, key);
  RAY_CHECK(del_status.ok()) << "RocksDB Delete failed for table=" << table_name
                             << " key=" << key << ": "
                             << del_status.ToString();

  if (callback) {
    main_io_service_.post([callback, existed]() { callback(existed); },
                          "GcsRocksDb.Delete");
  }
  return Status::OK();
}

Status RocksDbStoreClient::AsyncBatchDelete(
    const std::string &table_name,
    const std::vector<std::string> &keys,
    std::function<void(int64_t)> callback) {
  auto *cf = GetOrCreateColumnFamily(table_name);

  // Count how many of the requested keys actually exist before delete,
  // so the callback's int64_t is "how many real deletions happened",
  // mirroring Redis DEL semantics. For the POC's expected workload sizes
  // (≤ a few thousand keys per call) the per-key Get is cheap; a more
  // efficient implementation could use MultiGet here.
  rocksdb::WriteBatch batch;
  int64_t deleted_count = 0;
  for (const auto &k : keys) {
    std::string v;
    auto gs = db_->Get(rocksdb::ReadOptions(), cf, k, &v);
    if (gs.ok()) {
      ++deleted_count;
    } else if (!gs.IsNotFound()) {
      RAY_LOG(FATAL) << "RocksDB Get during BatchDelete failed for key=" << k
                     << ": " << gs.ToString();
    }
    auto bs = batch.Delete(cf, k);
    RAY_CHECK(bs.ok()) << "WriteBatch Delete failed: " << bs.ToString();
  }
  auto write_status = db_->Write(SyncWriteOptions(), &batch);
  RAY_CHECK(write_status.ok())
      << "RocksDB BatchDelete write failed: " << write_status.ToString();

  if (callback) {
    main_io_service_.post([callback, deleted_count]() { callback(deleted_count); },
                          "GcsRocksDb.BatchDelete");
  }
  return Status::OK();
}

Status RocksDbStoreClient::AsyncGetKeys(
    const std::string &table_name,
    const std::string &prefix,
    std::function<void(std::vector<std::string>)> callback) {
  RAY_CHECK(callback != nullptr);
  auto *cf = GetOrCreateColumnFamily(table_name);

  // Prefix scan: Seek to the prefix and walk forward while keys still
  // share the prefix. RocksDB's iterator is byte-ordered, so once a key
  // does not start with `prefix` no later key can either.
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions(), cf));
  std::vector<std::string> result;
  for (it->Seek(prefix); it->Valid(); it->Next()) {
    if (!it->key().starts_with(rocksdb::Slice(prefix))) break;
    result.emplace_back(it->key().ToString());
  }
  RAY_CHECK(it->status().ok())
      << "RocksDB iterator failed during GetKeys: " << it->status().ToString();

  main_io_service_.post(
      [callback, r = std::move(result)]() mutable { callback(std::move(r)); },
      "GcsRocksDb.GetKeys");
  return Status::OK();
}

Status RocksDbStoreClient::AsyncExists(const std::string &table_name,
                                       const std::string &key,
                                       std::function<void(bool)> callback) {
  RAY_CHECK(callback != nullptr);
  auto *cf = GetOrCreateColumnFamily(table_name);

  std::string v;
  auto status = db_->Get(rocksdb::ReadOptions(), cf, key, &v);
  bool exists = status.ok();
  if (!exists && !status.IsNotFound()) {
    RAY_LOG(FATAL) << "RocksDB Get for AsyncExists failed: " << status.ToString();
  }

  main_io_service_.post([callback, exists]() { callback(exists); },
                        "GcsRocksDb.Exists");
  return Status::OK();
}

}  // namespace gcs
}  // namespace ray
