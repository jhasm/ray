# Phase 1 — Foundations & Redis baseline

**Status:** in progress. One environment captured; second pending.
**Branch:** `jhasm/rep-64-poc-1`

## Claim addressed

n/a (foundational). Phase 1 establishes the Redis-side reference numbers we'll compare every later phase against. No REP claim is verified or refuted yet.

## Method

A single-threaded Python harness (`harness/docker-compose/bench.py`) drives a single Redis container configured with `appendonly yes` + `appendfsync always` so every write is synced to the AOF on disk before ack. This matches RocksDB's `WriteOptions::sync = true` contract — the durability guarantee both backends are claiming.

Operations measured (one entry per `StoreClient` method): `put`, `get_hit`, `get_miss`, `exists_hit`, `exists_miss`, `multi_get_10`, `prefix_scan`, `get_all`, `delete`, `batch_delete_10`. Result schema documented in `harness/docker-compose/README.md` (v1, shared across all backends so later phases produce diffable JSON).

Parameters: `n_warmup=500`, `n_measure=5000`, `value_size=256` bytes, single thread, single Redis instance on `127.0.0.1:6379` via Docker bridge networking.

## Result

Single environment so far. Raw JSON: [`harness/docker-compose/results/2026-04-29-linkedin-vm-epyc.json`](../harness/docker-compose/results/2026-04-29-linkedin-vm-epyc.json).

### Environment 1 — LinkedIn dev VM (Hyper-V on AMD EPYC 7763)

Linux 5.15.186.1, ext4 on `/dev/sda3`, 8 vCPUs, 31 GiB RAM. Redis 7.4.8 in Docker 24.0.9. Hypervisor: Microsoft (Hyper-V).

| Op | n | p50 (μs) | p99 (μs) | p999 (μs) | mean (μs) | ops/sec |
|---|---:|---:|---:|---:|---:|---:|
| `put` | 5000 | **3 869** | 12 194 | 31 338 | 4 724 | 211.6 |
| `get_hit` | 5000 | 145 | 226 | 433 | 150 | 6 588.7 |
| `get_miss` | 5000 | 142 | 214 | 427 | 147 | 6 746.8 |
| `exists_hit` | 5000 | 138 | 217 | 418 | 141 | 7 052.2 |
| `exists_miss` | 5000 | 144 | 216 | 421 | 146 | 6 797.5 |
| `multi_get_10` | 5000 | 183 | 278 | 463 | 187 | 5 317.8 |
| `prefix_scan` (full table, ~5500 keys) | 100 | 10 233 | 20 611 | — | 11 363 | 88.0 |
| `get_all` (full table) | 100 | 21 517 | 31 721 | — | 22 078 | 45.3 |
| `delete` | 2 750 | 3 855 | 13 662 | 28 343 | 4 811 | 207.7 |
| `batch_delete_10` | 100 | 4 317 | 8 895 | — | 5 156 | 193.9 |

### Environment 2 — pending

A second environment (a real laptop SSD or a cloud VM with EBS) is required by the PLAN before Phase 1 closes. Marked open and tracked in `TASKS` below.

## Skepticism

### What looks suspicious in these numbers

The PUT p50 of 3.87 ms is at the high end of the REP's claimed Redis range (0.5–2 ms). The natural explanation is "fsync per write is slow on this disk." A direct fsync probe contradicts that:

```
fsync(4 KiB) on / (ext4): n=200  p50=0.7us  p99=1.2us  mean=0.7us
```

Sub-microsecond `fsync` on a Hyper-V virtual disk means the host (or the virtualized block layer) is buffering writes — `fsync` returns before any physical media flush. **This is the failure mode `RISKS.md` R4 flags for K8s persistent volumes and we are seeing it here on a developer VM.**

Implications for the POC:
- The PUT cost on this VM is not actually fsync-bound. Most of the 3.87 ms is somewhere else: redis-py + Docker bridge networking (~150 μs round-trip × 2 for command + AOF wait), Redis AOF flush mechanics (which may use stronger sync than my probe), Python loop overhead, and possibly hypervisor scheduling jitter.
- These numbers are **not a fair representation of fsync-true durability** on a real spinning or NVMe device. They establish a "ballpark Redis-on-this-VM" reference; cross-environment comparison is essential.
- This is a strong signal that Phase 4's durability proof must be done on substrates where fsync actually means fsync — not this VM.

### What this baseline does *not* establish

- **Apples-to-apples vs Ray's `RedisStoreClient`.** This bench drives Redis at the protocol level, not through Ray's client. The Ray client adds a per-key request queue, callback dispatch, and namespace handling. Phase 7 closes that gap.
- **Network distance.** Production Redis is typically on a separate pod or off-cluster. We measure 127.0.0.1; add 0.5–2 ms RTT for production-like topology.
- **Concurrency.** Single-threaded numbers — GCS is single-threaded today, so this is the right baseline. Concurrency-stress is in Phase 5 territory.
- **Recovery time.** Phase 8.

### What would invalidate these numbers

- **Different value size.** 256-byte values are typical for actor metadata; very large entries (placement group state with many bundles) shift the read tail noticeably. Re-run with `--value-size 4096` would be a useful sensitivity check.
- **Different `n_measure`.** 5000 iterations gives stable percentiles for `put`/`get` but `prefix_scan` and `get_all` only have 100 samples each — p999 is therefore not meaningful for those (filled in as `—` above).
- **Cold cache vs warm cache.** OS page cache helps repeated reads. We did not `drop_caches` between operations; later phases will need to.

## Reproducer

```bash
cd rep-64-poc/harness/docker-compose
./run.sh                                 # writes ./baseline-redis.json
./run.sh /tmp/my-result.json             # custom output path
```

Prereqs: Docker 24+ with `docker compose` v2 (or legacy `docker-compose`). Python 3.9+. The runner creates a venv inside the harness directory and installs `redis-py` (5.2.1, pinned).

For environments without `docker compose` (e.g., this LinkedIn VM, which has only `docker run`), use the manual sequence captured in `harness/docker-compose/results/2026-04-29-linkedin-vm-epyc.json` `harness.environment.container_runtime` field — the same image, same flags, same bench script — until a follow-up adds a no-compose fallback path to `run.sh`.

## Pivot decision

**Proceed.** Phase 1 numbers are not a pivot trigger — they're the reference. The fsync surprise is logged in `RISKS.md` R4 and earmarked for Phase 4 substrates that actually flush.

## Next concrete actions before closing Phase 1

1. Capture baseline on a second environment with truthful fsync (laptop SSD or cloud VM with EBS gp3).
2. Pin `redis:7.4-alpine` to the exact digest used (`sha256:9210b8dc25f122eb00e5572dcc7147c8e11fb1a08308b088e06c9d5dd2aa49d6`) in `docker-compose.yml`. Addresses R10.
3. Optionally extend `bench.py` with a `--value-size 4096` sensitivity sweep for the read tail.
