# rep-64 POC — Kubernetes harness

End-to-end test harness for the rep-64 RocksDB-backed GCS POC, running on
KubeRay against either a local k3d cluster or a remote Kubernetes cluster.
Covers the K8s-shaped items from `rep-64-poc/COLLABORATORS.md`: pod-delete
recovery (#5), actor-survival E2E (#6), substrate sweep (#4 partial), and
fast-NVMe throughput (#8, stubbed).

## Quickstart (local k3d)

```bash
# Prerequisites: docker, k3d v5+, kubectl, jq, envsubst, helm, python3.
# A pre-built rep-64 image at cr.ray.io/rayproject/ray:nightly-rep64-fix
# (run rep-64-poc/build-image.sh from repo root if you don't have one yet).

cd rep-64-poc/harness/k8s
export HARNESS_ENV_FILE=env/k3d.env

scripts/run-all.sh                  # full sweep + aggregate
cat results/k3d/summary.md          # human-readable findings

scripts/run-all.sh --teardown       # full sweep + tear down k3d cluster
```

That's it. Individual scripts (`scripts/00-setup-k3d.sh`,
`scripts/30-pod-delete.sh`, etc.) can be run directly for iterative
debugging.

## Tier model

The harness is designed to run identical scripts against two tiers:

| Tier   | Env file       | Cluster bring-up   | Status                                   |
|--------|----------------|--------------------|------------------------------------------|
| k3d    | `env/k3d.env`  | `00-setup-k3d.sh`  | implemented; runs end-to-end             |
| remote | `env/remote.env` (planned) | external | remote bring-up + perms-check are Phase E follow-on (not yet implemented) |

`KUBE_CONTEXT` in the env file selects which kubectl context the run
attaches to.  Remote tier (when added) will never create or destroy
clusters — bring-up and tear-down stay the operator's responsibility.

### Why k3d, not kind

The local tier was originally kind, but kind v0.25 fails on Mariner
cgroup-v1 hosts: kubelet can't create the nested systemd slices it
expects (`cgroup [kubelet kubepods] has some missing paths`).  k3d
(k3s in Docker) has a much smaller cgroup footprint and works on the
same hosts.  All scripts were written tier-agnostic — switching to
remote (EKS, GKE, or any managed K8s) is a one-line edit to
`env/remote.env`.

## Scripts catalog

| Script                              | Purpose                                          |
|-------------------------------------|--------------------------------------------------|
| `00-setup-k3d.sh`                   | Create k3d cluster + KubeRay operator (k3d only) |
| `10-deploy-cluster.sh`              | Apply RayCluster CR + PVC                        |
| `20-actor-survival.sh`              | COLLAB #6 — detached actors survive disconnect   |
| `30-pod-delete.sh`                  | COLLAB #5 — head pod kill + recovery; runs both rocksdb and inmem baselines |
| `40-substrate-sweep.sh`             | COLLAB #4 — fsync honesty per StorageClass       |
| `50-fast-storage.sh` (stub)         | COLLAB #8 — pipelined throughput (deferred)      |
| `99-teardown.sh`                    | Delete RayCluster, PVC, k3d cluster              |
| `run-all.sh`                        | Orchestrator — runs everything, then aggregates  |

Tests collect a JSON envelope per run into `$RESULTS_DIR/<test>-<ts>.json`
(see `lib.sh::write_result`); `python/aggregate.py` reads the latest
JSON per test and emits `summary.md`.

## Interpreting results

`summary.md` has two sections:

- **Tests** — one row per test with status, duration, and a key metric.
- **Findings** — explicit list of metric thresholds that were missed
  *or* tests that didn't complete.  Empty Findings = clean run.

Spec thresholds (from `python/aggregate.py:THRESHOLDS`):

| Metric                | Spec target | What it gates                          |
|-----------------------|-------------|----------------------------------------|
| `pod_restart_s`       | ≤ 30 s      | head pod recovers within 30 s          |
| `state_preserved_pct` | ≥ 95 %      | ≥ 95 % of actor state preserved        |
| `new_tasks_ok`        | == true     | cluster accepts new tasks after restart |

A finding looks like:

```
| 30-pod-delete | rocksdb | state_preserved_pct | >= 95 (actor state ≥95% preserved) | 0 | 30-pod-delete-20260508T042023Z.json |
```

The `source` column always points back to the raw JSON for full context.

## Image story

The deployed image is `cr.ray.io/rayproject/ray:nightly-rep64-fix` —
a locally-built variant of the upstream Ray nightly image that includes
two POC-required C++ patches not yet upstream:

- `gcs_server.cc`: split RocksDB into `/data/gcs/tables/` and
  `/data/gcs/kv/` subdirs (avoids RocksDB's same-process double-open
  ENOLCK).
- `rocksdb_store_client.cc`: mkdir loop for intermediate dirs.

Build via `rep-64-poc/build-image.sh ray` (run from repo root; ~3 min
warm cache, ~50 min cold).  k3d picks it up via `k3d image import`;
remote tiers need it pushed to a registry the cluster can reach.  For
external collaborators, the image is also published at
`ghcr.io/jhasm/ray-rocksdb-poc:rep-64-poc` (public).

## Known issues

### Path footgun

Run scripts from the repo root, not from `rep-64-poc/harness/k8s/`.
`RESULTS_DIR` in `env/k3d.env` is a relative path; running from inside
the harness directory nests the output one level too deep.

### Open finding 1: rocksdb 0–50 % recovery (non-deterministic)

The headline 30-pod-delete test currently records 0 %–50 % actor state
preservation across runs (observed: 0 %, 40 %, 50 % in successive runs).
The non-determinism suggests an async-write race: actor manager writes
to GCS aren't durably committed to RocksDB before the head pod is
killed.  The 2-second flush sleep at `python/pod_delete_workload.py:65-66`
is insufficient.  Confirmed POC bug; investigation deferred per the
harness-build phase mandate.  The inmem baseline records 0 % (expected —
memory-only GCS doesn't survive pod kill).

### Open finding 2: rocksdb head-pod cold-start exceeds liveness window

On a cold cluster (fresh PVC, first head-pod restart), rocksdb GCS
startup takes ~138 s — far longer than the kubelet liveness probe
allows.  The container is killed mid-startup, triggering a CrashLoop;
recovery eventually happens via repeated container restarts within the
300 s harness timeout.  On a warm cluster the same path takes ~10 s, so
this only manifests on the first restart of a freshly deployed cluster
— which is exactly the production-relevant scenario.  Liveness probe
config likely needs `initialDelaySeconds` raised, or KubeRay's default
probe schedule needs override at the RayCluster level.

## Canonical baseline

`results/canonical/` contains the committed reference baseline (`*.committed.json` per test, plus a `summary.md`).  This is the snapshot the project considers "current known state" — both findings above are visible
in it.  Re-run `run-all.sh` and copy the latest `results/k3d/*.json` over the canonical files to refresh the baseline once a fix lands.

### Substrate sweep

`SUBSTRATE_SWEEP_CLASSES` is empty in `env/k3d.env` because k3d ships
only `local-path`.  Set it on remote tier (e.g.
`SUBSTRATE_SWEEP_CLASSES=local-nvme,local-ssd,nfs-fast-provision-v3`)
to exercise `40-substrate-sweep.sh`.

## Customizing

Every cluster-specific value lives in the env file.  Common knobs:

| Var                       | Purpose                                       |
|---------------------------|-----------------------------------------------|
| `KUBE_CONTEXT`            | which kubectl context to attach to            |
| `NAMESPACE`               | k8s namespace for RayCluster + Jobs           |
| `IMAGE`                   | rep-64 image reference                        |
| `STORAGE_CLASS`           | default PVC class for RayCluster              |
| `SUBSTRATE_SWEEP_CLASSES` | csv list of classes for `40-substrate-sweep`  |
| `FAST_NVME_CLASS`         | NVMe class for `50-fast-storage` (stub)       |
| `BASELINE_INMEM`          | 1 = run inmem baseline alongside rocksdb in C3 |
| `ACTOR_COUNT`             | actors created in the survival/pod-delete tests |
| `POD_RESTART_TIMEOUT_S`   | how long to wait for head pod re-Ready        |
| `RESULTS_DIR`             | where JSONs and summary land                  |

`HARNESS_ENV_FILE` can be set explicitly to override the default lookup;
otherwise scripts expect `env/k3d.env`.

## Reproducing on a different cluster (planned)

The remote tier is designed but not yet implemented.  Sketch of how it
will work once the Phase E scripts land:

```bash
# Will need: env/remote.env (templated from a future remote.env.example),
# scripts/00-check-remote.sh (read-only perms + CRDs check), and run-all
# with --tier=remote.

cp env/remote.env.example env/remote.env
# Edit env/remote.env: KUBE_CONTEXT, NAMESPACE, IMAGE (registry the
# cluster can pull from), STORAGE_CLASS, SUBSTRATE_SWEEP_CLASSES.

scripts/00-check-remote.sh                 # (planned) read-only verify
scripts/run-all.sh --tier=remote --skip-setup
```

Until then, contributors who want to validate against an external
cluster can hand-author an env file off `env/k3d.env`, set
`HARNESS_ENV_FILE=env/your.env`, and run individual scripts directly.
