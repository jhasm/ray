# [REP-64 POC] Embedded RocksDB Storage Backend for Ray GCS — proof of concept

This PR is a **proof of concept** for [REP-64](https://github.com/ray-project/enhancements/blob/master/reps/2026-02-23-gcs-embedded-storage.md) (Embedded GCS Storage Backend), not a feature merge. It is here to prove the design works, surface its risks, and produce reproducible evidence for every concrete claim in the REP — before anyone sinks weeks into production hardening.

The dossier this PR ships under [`rep-64-poc/`](./rep-64-poc/) is the load-bearing artifact. Per-claim verdicts and one-command repros are in [`rep-64-poc/EVIDENCE.md`](./rep-64-poc/EVIDENCE.md).

## TL;DR

The REP's design **works on the substrates we can test rigorously here**, with three findings that warrant maintainer attention before merge:

| | Finding | Action requested |
|---|---|---|
| 🔴 | **REP's "0.01–0.1 ms RocksDB write" claim is misleading.** That's a memtable-only / `sync=false` number. The durable-write path the REP requires is fsync-bounded: **3.81 ms p50** on probe-verified ext4. (Reads remain 0.01–0.1 ms class as claimed.) | REP perf section needs a sync vs non-sync split. |
| 🟡 | **Cluster-ID-mismatch fail-fast cannot be implemented as the REP draws it.** Ray's GCS init order has `InitKVManager()` before `GetOrGenerateClusterId()` *and* the persisted ID *is* the cluster ID. PVC-mismatch detection requires an external authoritative source (K8s downward API). Deferred to Phase 8 with an honest write-up. | Confirm the deferral is acceptable; alt-design welcome. |
| 🟢 | **Two Ray-core regressions surfaced and are fixed inline.** Both are independently upstream-worthy: `@boost` URL (jfrog → archives.boost.io); `BUILD.rocksdb` lib path. Either could be split into a separate small PR if the maintainers prefer. | Indicate preference: bundled vs split commits. |

Phases 1–8 each produced a written report with claim addressed / method / result / skepticism / pivot decision. Verdicts:

| # | REP claim | Verdict |
|---|---|---|
| 1 | RocksDB integrates into Ray Bazel build cleanly | ✅ Verified |
| 2 | Binary size bounded (PLAN pivot trigger: 50 MB) | ✅ Verified — 24 MB stripped |
| 3 | StoreClient API fits RocksDB | ✅ Verified — `StoreClientTestBase` PASSES unmodified |
| 4 | State persists across close+reopen at storage layer | ✅ Verified — sub-100 ms cold open at 10k entries |
| 5 | Ack ⇒ durable | ✅ Verified on ext4 (cloud PVs pending collaborator) |
| 6 | Atomic GetNextJobID under N-thread contention | ✅ Verified — caught + fixed a real TOCTOU race in this phase |
| 7 | RocksDB write 0.01–0.1 ms | ❌ **Contradicted for sync writes (3.81 ms p50)** |
| 8 | RocksDB read 0.01–0.1 ms | ✅ Confirmed (0.97 µs p50) |
| 9 | Recovery time ≥ Redis-based FT | 🟡 Provisional yes (sub-100 ms storage-layer; head-to-head pending) |
| 10 | Stale-data protection on PVC swap | 🟡 Storage-layer marker tested; fail-fast deferred to Phase 8 |

Full evidence dossier: **[`rep-64-poc/EVIDENCE.md`](./rep-64-poc/EVIDENCE.md)**.

## What's in this PR

### Production-bound code (would land in a real merge)

- `bazel/BUILD.rocksdb` (new) — drives RocksDB's CMake build via `rules_foreign_cc`, all optional features OFF, `CMAKE_INSTALL_LIBDIR=lib` for portability.
- `bazel/ray_deps_setup.bzl` — adds `@com_github_facebook_rocksdb`; switches `@boost` URL to `archives.boost.io` (Ray-core regression fix, independent of REP-64).
- `src/ray/gcs/store_client/rocksdb_store_client.{h,cc}` (new) — full `StoreClient` implementation. All 9 methods. Mutex-RMW `GetNextJobID`. Sync writes via `WriteOptions::sync = true`. Cluster-ID marker plumbing (skipped when cluster_id is empty — see below).
- `src/ray/gcs/store_client/test/rocksdb_store_client_test.cc` (new) — Phase 3 walking-skeleton unit tests (PutGet, RecoverAcrossReopen, JobIdMonotonicAndPersists, ClusterIdMarkerWritesOnFirstOpen).
- `src/ray/gcs/gcs_server/gcs_server.{h,cc}` — adds `StorageType::ROCKSDB_PERSIST` + `kRocksDbStorage = "rocksdb"`; new `InitKVManager` branch. Passes empty cluster_id to `RocksDbStoreClient` because Ray's init order makes a real cluster_id unavailable at this point (fully written up in `reports/phase-3-skeleton.md`).
- `src/ray/common/ray_config_def.h` — adds `RAY_CONFIG(std::string, gcs_storage_path, "")` for the `RAY_GCS_STORAGE_PATH` env var.
- `BUILD.bazel` — three new entries: the smoke test, the Phase 3 unit test, the `gcs_rocksdb_store_client` library wired into `gcs_server_lib`.

### POC harness (lives entirely in `rep-64-poc/`, not on the production path)

Each phase ships a self-contained harness:

- **Phase 1** — `harness/durability/probe_fsync.py` (fsync-honesty probe), `harness/docker-compose/` (Redis baseline).
- **Phase 4** — `harness/durability/kill9/{writer.cc, verifier.cc, run_kill9.py}`. Includes a deliberately-broken `--ghost-writes` mode that proves the harness has fault sensitivity.
- **Phase 5** — `harness/concurrency/concurrency_test.cc`. 16-thread × 1000-call stress on `GetNextJobID` + parallel-write linearizability + post-restart counter monotonicity. Caught + drove a real TOCTOU bug fix in `GetOrCreateColumnFamily`.
- **Phase 6** — `harness/store_client_parity/rocksdb_parity_test.cc`. Subclasses Ray's existing `StoreClientTestBase` (the same one the in-memory and Redis backends use). PASSES unmodified.
- **Phase 7** — `harness/microbench/storage_microbench.cc`. C++ side-by-side latency benchmark, p50/p95/p99/throughput. Default working dir is `$HOME/.cache/...` not `/tmp` — see methodology note about tmpfs masking fsync.
- **Phase 8** — `harness/recovery/recovery_bench.cc` (storage-layer cold-open + scan); `harness/kind/ray-head-rocksdb.yaml` (reference K8s manifest with annotated failure modes); `harness/cloud/cloud_fsync_check.sh` (collaborator-runnable cloud-volume wrapper).

### Dossier

- `rep-64-poc/README.md` — orientation for reviewers.
- `rep-64-poc/EVIDENCE.md` — claim-to-evidence map. **Read first.**
- `rep-64-poc/RISKS.md` — live risk register with end-of-POC status per row.
- `rep-64-poc/reports/phase-N-*.md` — eight phase reports with claim / method / result / skepticism / repro / pivot.
- `rep-64-poc/reproducers/README.md` — one-command repros for everything.
- `rep-64-poc/PLAN.md` — the up-front plan, kept for the audit trail.

## Test plan

- [x] `bazel build //:rocksdb_smoke_test` — green, 8.3 MB binary
- [x] `bazel test //:rocksdb_smoke_test` — 64 ms PASS
- [x] `bazel test //:rocksdb_store_client_test` — 4/4 PASS in 273 ms
- [x] `bazel test //rep-64-poc/harness/concurrency:concurrency_test --runs_per_test=10` — 40/40 PASS, no flake
- [x] `bazel test //rep-64-poc/harness/store_client_parity:rocksdb_parity_test` — `StoreClientTestBase` PASS in 1.8 s
- [x] `bazel build //:gcs_server` with the new code path — 32 MB unstripped / 24 MB stripped, `--help` exits 0
- [x] Phase 4 kill-9 harness, ext4 + sync=1 — 0 acked-but-missing across 1k and 5k writes
- [x] Phase 4 ghost-writes negative control — exactly 50/50 acked-but-missing detected
- [x] Phase 7 microbench on probe-verified honest ext4
- [x] Phase 8 storage-layer recovery: 34 ms / 39 ms / 81 ms at 100 / 1k / 10k entries

Pending (reproducers shipped; runs not done in this POC):

- [ ] `--config=asan-clang` and `--config=tsan` runs of all the test targets above (LLVM toolchain not on the dev host)
- [ ] End-to-end actor-survival test (`harness/integration/test_rocksdb_recovery.py`) — needs a Ray container image built from this branch
- [ ] K8s pod-delete recovery timing — needs the same image
- [ ] Cloud-volume substrate sweep (EBS / GCE PD) via collaborator running `cloud_fsync_check.sh`
- [ ] NFS loopback substrate
- [ ] Multi-threaded-writer microbench variant (group-commit benefit)
- [ ] Docker Compose Redis-vs-RocksDB head-to-head harness
- [ ] `chaos_rocksdb_store_client_test.cc` mirroring `chaos_redis_store_client_test.cc`

The POC stops short of running these because each requires either a multi-hour wheel-build pipeline, cloud infra access, or a host with `sudo` for `drop_caches`. The reproducers are shipped so the maintainers (or the next iteration on a properly-tooled host) can pick up where we left off.

## Open questions for maintainers

1. **REP perf-table revision.** The 0.01–0.1 ms RocksDB-write number conflates memtable-only and sync-on-write paths. Phase 7 has the data; want me to send a small REP-revision PR alongside this?
2. **Boost URL fix scope.** Commit `07c65c84` (`@boost` URL: jfrog → archives.boost.io) is independent of REP-64 and would unblock every Ray master build today. Bundle here, or split into a separate small PR?
3. **Cluster-ID fail-fast deferral.** The current code passes empty cluster_id to `RocksDbStoreClient` and skips the marker check (Phase 3 report). Is the K8s-downward-API plumbing path acceptable as the way forward, or is a different design preferred?
4. **PVC StorageClass guidance.** The K8s manifest assumes RWO; multi-AZ recovery (pod re-scheduled to a different AZ) needs the StorageClass to allow re-attachment. Do you want guidance committed alongside the manifest, or treated as KubeRay-operator territory?
5. **Test-target naming / placement.** I put POC-specific tests under `//rep-64-poc/harness/...` to keep `BUILD.bazel` minimal. The Phase 3 walking-skeleton test sits alongside the InMemory / Redis tests in `//:rocksdb_store_client_test`. Is the split right, or would you prefer everything under `src/ray/gcs/store_client/test/`?

## Migration / risk

This branch does **not** change behavior for existing Redis-backed Ray clusters. `RAY_GCS_STORAGE` defaults to its current value; the new `"rocksdb"` value is opt-in. `RAY_GCS_STORAGE_PATH` defaults to empty and only matters when `RAY_GCS_STORAGE=rocksdb`.

The boost URL fix (commit `07c65c84`) changes the upstream URL for the `@boost` archive. The pinned SHA256 is unchanged; the new URL serves the *same* archive content (verified manually with `curl + sha256sum`). Builds that previously failed will now succeed; no other change.

## How to review

Time-boxed:

- **10 minutes** — `rep-64-poc/EVIDENCE.md`, then skim `rep-64-poc/RISKS.md`.
- **30 minutes** — read each phase report's "Result" and "Skepticism" sections. Skip method bodies.
- **1 hour** — re-run the quick sanity sweep (4 bazel tests, ~10 minutes after first cold build), spot-check one or two repros from `rep-64-poc/reproducers/README.md`.
- **Half a day** — re-run on your own substrate, especially Phase 7 microbench (different fsync class) and Phase 8 recovery_bench. Numbers should track ours within an order of magnitude on commodity-PV-class storage.

---

🤖 The dossier was authored with Claude Code (`Co-Authored-By:` lines on every commit). All numerical claims are reproducible from the bench harnesses; please run them rather than taking the numbers on faith.
