# KubeRay test harness — design

**Status:** draft, awaiting review
**Date:** 2026-05-08
**Author:** jhasm (Santosh Jha)
**Branch:** `jhasm/rep-64-poc-1`
**Related:** [`COLLABORATORS.md`](../../COLLABORATORS.md), [`PLAN.md`](../../PLAN.md), REP-64

## Why this exists

The REP-64 POC currently has no end-to-end test against a real KubeRay deployment. `harness/kind/ray-head-rocksdb.yaml` is a single-pod manifest, not a `RayCluster` CR; `harness/integration/test_rocksdb_recovery.py` is a 163-line skeleton that has never been run against a live cluster. The headline test in `COLLABORATORS.md` — item #5, "kubectl delete pod ray-head and measure recovery" — is the single most important production-defensibility signal for the POC and is currently missing.

This document specifies a reproducible test harness that:

1. Runs against a local `kind` cluster on a developer machine for fast iteration.
2. Runs against any external Kubernetes cluster (the author uses `prod-ltx1-k8s-1`, namespace `kk-flyte-prod`) for real-storage and real-KubeRay-operator signal.
3. Uses identical scripts on both tiers, parametrized by env-file profiles.
4. Emits machine-readable JSON results plus a human-readable `summary.md`, so kind and remote runs are mechanically comparable.
5. Has zero LinkedIn-internal coupling in script logic; everything cluster-specific lives in a gitignored env file.

## Goals

- Cover the K8s-shaped items from `COLLABORATORS.md`: #5 pod-delete recovery (headline), #6 actor-survival E2E, #4 substrate sweep, #1 different-FS (bonus), #8 fast-NVMe pipelined throughput (bonus).
- Be runnable by an external collaborator on EKS / GKE / kind without touching script source.
- Provide an inmem-baseline companion run for #5 to make the rocksdb-vs-default comparison explicit.
- Pin KubeRay operator to **v1.6.0** in the local `kind` cluster to match what the author's remote cluster runs, so a bug in kind reproduces upstream.

## Non-goals

- arm64 (#2), macOS build (#3), ASAN/TSAN (#7), architectural review (#9). These remain VM- or judgment-bound and are out of scope for this harness.
- Production hardening of the POC's offload path (separate follow-on; called out in `COLLABORATORS.md` "What's not on this list").
- Multi-AZ K8s recovery (KubeRay-operator territory).

## Tier strategy

Three layers, only the second and third are new:

| Tier | What it tests | Feedback loop | Image distribution |
|---|---|---|---|
| 0 (existing) | Bazel unit + parity + concurrency tests | ~30s | n/a (in-process) |
| 1 (new) | Local `kind` cluster, full KubeRay surface | ~5–10 min | `kind load docker-image` |
| 2 (new) | External cluster (e.g. `prod-ltx1-k8s-1`) | ~20–40 min | public ghcr.io image, optional in-cluster registry mirror |

The user-facing image for tier 2 is the public package the author has already pushed: **`ghcr.io/jhasm/ray-rocksdb-poc:rep-64-poc`** (also tagged `:69a81393` for the exact commit). External collaborators set this in `remote.env` and KubeRay pulls it directly.

## Directory layout

```
rep-64-poc/harness/k8s/
├── README.md                # one-page how-to
├── DESIGN.md                # this document
├── lib.sh                   # render_manifest, wait_ray_ready, collect_results
├── env/
│   ├── kind.env             # local-kind profile (committed)
│   ├── remote.env.example   # template for any external cluster (committed)
│   └── remote.env           # user-specific (gitignored)
├── manifests/
│   ├── raycluster.yaml.tmpl       # the RayCluster CR
│   ├── job-substrate-probe.yaml.tmpl
│   ├── job-microbench.yaml.tmpl
│   └── job-pipeline-bench.yaml.tmpl
├── scripts/
│   ├── 00-setup-kind.sh           # creates kind cluster + installs KubeRay + kind-loads image
│   ├── 00-check-remote.sh         # verifies KUBE_CONTEXT exists, perms, operator present
│   ├── 05-deploy-registry.sh      # opt-in: in-namespace registry:2 fallback for restricted clusters
│   ├── 10-deploy-cluster.sh       # apply RayCluster, wait for head + workers Ready
│   ├── 20-actor-survival.sh       # COLLABORATORS #6
│   ├── 30-pod-delete.sh           # COLLABORATORS #5 — runs rocksdb + inmem-baseline
│   ├── 40-substrate-sweep.sh      # COLLABORATORS #1 + #4
│   ├── 50-fast-storage.sh         # COLLABORATORS #8 — only when FAST_NVME_CLASS set
│   ├── 99-teardown.sh
│   ├── run-all.sh                 # orchestrator: --tier=kind|remote
│   └── aggregate.py               # JSON → summary.md
└── results/
    ├── .gitignore                 # ignore everything except *.committed.json + canonical/
    ├── kind/                      # gitignored
    ├── remote/                    # gitignored
    └── canonical/
        ├── kind-baseline.committed.json
        └── remote-baseline.committed.json   # populated once remote tier is green
```

### Cluster-identity contract

Every script as its first action does:

```bash
source "${HARNESS_ENV_FILE:?set HARNESS_ENV_FILE=env/kind.env or env/remote.env}"
kubectl config use-context "${KUBE_CONTEXT:?KUBE_CONTEXT must be set in env file}"
```

No script names a cluster. Switching clusters is one line in one env file. Adding a third cluster (EKS, GKE) is one new env file.

## Env-profile contents

### `env/kind.env` (committed, runs as-is on a developer machine)

```bash
# Cluster identity
KUBE_CONTEXT=kind-rep64                    # 00-setup-kind.sh sets this
NAMESPACE=rep64-poc
KIND_CLUSTER_NAME=rep64                    # only used by 00-setup-kind.sh

# Image
IMAGE=cr.ray.io/rayproject/ray:nightly     # what build-image.sh produced
IMAGE_PULL_POLICY=Never                    # image is kind-loaded, no pull

# Workload sizing (kind = single host, keep small)
HEAD_CPU=2
HEAD_MEMORY=4Gi
WORKER_REPLICAS=1
WORKER_CPU=1
WORKER_MEMORY=2Gi

# Storage
STORAGE_CLASS=standard                     # kind ships local-path-provisioner as `standard`
PVC_SIZE=5Gi
SUBSTRATE_SWEEP_CLASSES=                   # default empty; opt-in via --with-substrate-diversity
FAST_NVME_CLASS=                           # not present on kind

# Test workload
ACTOR_COUNT=10
MICROBENCH_KEY_COUNT=10000
PIPELINE_BENCH_IO_POOL_SIZES="1 2 4 8"
BASELINE_INMEM=1                           # run inmem-baseline companion in 30-pod-delete.sh

# Output
RESULTS_DIR=rep-64-poc/harness/k8s/results/kind
```

### `env/remote.env.example` (committed, copy to `remote.env` and edit)

```bash
# Cluster identity — fill in for your cluster
KUBE_CONTEXT=                              # e.g. prod-ltx1-k8s-1, arn:aws:eks:..., gke_proj_zone_name
NAMESPACE=                                 # a namespace where you can create RayCluster + PVC

# Image — recommended public default (override if your cluster can't reach ghcr.io)
IMAGE=ghcr.io/jhasm/ray-rocksdb-poc:rep-64-poc
IMAGE_PULL_POLICY=IfNotPresent
IMAGE_PULL_SECRET=                         # optional, for private registries

# Workload sizing (real cluster — bump up)
HEAD_CPU=4
HEAD_MEMORY=8Gi
WORKER_REPLICAS=2
WORKER_CPU=2
WORKER_MEMORY=4Gi

# Storage
STORAGE_CLASS=                             # primary SC for non-sweep tests (e.g. local-nvme)
PVC_SIZE=20Gi
SUBSTRATE_SWEEP_CLASSES=                   # csv list, e.g. local-nvme,local-ssd,nfs-fast-provision-v3
FAST_NVME_CLASS=                           # optional, for 50-fast-storage.sh (e.g. local-nvme)

# Test workload
ACTOR_COUNT=20
MICROBENCH_KEY_COUNT=100000
PIPELINE_BENCH_IO_POOL_SIZES="1 2 4 8 16"
BASELINE_INMEM=0                           # skip baseline on real clusters by default

# Output
RESULTS_DIR=rep-64-poc/harness/k8s/results/remote
```

### Why these knobs

- **`SUBSTRATE_SWEEP_CLASSES` is a list, not a single value.** `40-substrate-sweep.sh` iterates over it; empty list ⇒ script no-ops with a clean message. kind users get a stub run, multi-StorageClass cluster users get a real sweep.
- **`FAST_NVME_CLASS` is separate from `STORAGE_CLASS`.** `50-fast-storage.sh` only runs if this is set, so it cleanly skips on clusters without local NVMe.
- **No registry credentials in env files.** `IMAGE_PULL_SECRET` is just a secret-name reference; the secret itself is created out-of-band.
- **`RESULTS_DIR` is per-tier**, so kind and remote results don't overwrite each other.
- **`BASELINE_INMEM` defaults to 1 in kind, 0 in remote.** Comparison runs are valuable on the dev machine; on a real cluster they double the runtime and are usually unwanted.

## Test catalog

Every test:

- Sources `$HARNESS_ENV_FILE`, switches to `$KUBE_CONTEXT`, and uses `lib.sh` helpers.
- Emits a JSON to `$RESULTS_DIR/<script>-<timestamp>.json` matching the envelope schema in §"Result schema".
- Exits 0 on pass, 1 on fail, 0 with `status: skipped` on intentional skip.
- Prints a one-line summary to stdout.

### `10-deploy-cluster.sh` (foundational, not a test)

Renders `manifests/raycluster.yaml.tmpl` with env vars, applies it, waits for the RayCluster CR's `Ready` condition plus head and worker pods all `Running` + `Ready` within `DEPLOY_TIMEOUT_S` (default 600s). The RayCluster sets `RAY_gcs_storage=rocksdb` and `RAY_gcs_storage_path=/data/gcs` on the head; PVC mounted at `/data`. Idempotent — `kubectl apply` only, no delete.

**Used by:** every test below. **Success:** RayCluster `Ready=True` and all pods Ready in time.

### `20-actor-survival.sh` — `COLLABORATORS #6`

Submits a `RayJob` that runs the existing `harness/integration/test_rocksdb_recovery.py` (already 163 lines, salvageable). Workload: create `$ACTOR_COUNT` named actors holding state, submit tasks updating state, snapshot. Then tears down the Ray client and reconnects, verifies actors still resolvable by name and state intact. **Doesn't kill anything** — narrower than #5; tests serialization round-trip through `RocksDbStoreClient`.

**Success:** all actors resolvable post-reconnect, state matches pre-snapshot, named-actor lookup works, detached actors survive client disconnect.
**Metrics:** `{actors_created, actors_recovered, actors_lost, named_lookup_ok, detached_survived, state_match_pct}`.

### `30-pod-delete.sh` — `COLLABORATORS #5` (the headline)

1. Establish workload (same as #20).
2. Snapshot state at T0.
3. `kubectl delete pod <head>` at T1.
4. Poll head pod's `.status.containerStatuses[0].ready` until true → T2.
5. Reconnect Python client, verify state matches T0 snapshot → T3.
6. Re-run a few new tasks to confirm cluster is serving.

When `BASELINE_INMEM=1`, runs the entire flow a second time with `RAY_gcs_storage=` (default in-memory backend). Result file: `30-pod-delete.inmem.json`. The contrast — rocksdb run preserves state, inmem run does not — is the point.

**Success:** T2 − T1 < `POD_RESTART_TIMEOUT_S` (default 120s, REP-64 target ≤ 30s on warm image), `actors_recovered ≥ 95%` of `actors_created`, new tasks execute.
**Metrics:** `{pod_restart_s, gcs_reload_estimate_s, state_preserved_pct, new_tasks_ok, backend}`.

This is the single most important measurement in the suite. Risk-register claim 9 in `RISKS.md` is provisional pending exactly this number.

### `40-substrate-sweep.sh` — `COLLABORATORS #4` (and `#1` if substrate diversity is on)

For each `SC` in `SUBSTRATE_SWEEP_CLASSES`:

1. Create PVC bound to `SC` (`PVC_SIZE`).
2. Run a Job pod that mounts the PVC, runs `probe_fsync.py` (existing) → emits `verdict: honest|lying`. If honest, runs `storage_microbench` (existing Bazel target, `--db-dir /pvc/microbench`).
3. Collect both JSONs back to `$RESULTS_DIR/sweep/<SC>/`.
4. Delete PVC + Job.

Empty `SUBSTRATE_SWEEP_CLASSES` ⇒ script logs "no sweep configured, skipping" and exits 0 with `status: skipped`.

**Success per SC:** probe verdict `honest` (or explicitly `lying` flagged for documentation), microbench p50 sync-write within ~30% of probe fsync p50.
**Metrics:** `{by_storage_class: {<sc>: {fsync_verdict, fsync_p50_us, fsync_p99_us, microbench_sync_write_p50_us, microbench_read_p50_us}}}`.

### `50-fast-storage.sh` — `COLLABORATORS #8`

Skips with `status: skipped` if `FAST_NVME_CLASS` is unset. Otherwise: single Job, PVC backed by `FAST_NVME_CLASS`, mounted at `/data`. Runs `storage_microbench` twice — sequential (per-call latency) and pipelined (aggregate throughput) — across `PIPELINE_BENCH_IO_POOL_SIZES`.

**Success:** sequential `inline ≈ offload` (within 5%); pipelined offload throughput strictly higher than inline; offload throughput monotonic in pool size until inflection.
**Metrics:** `{sequential: {inline_p50_us, offload_p50_us}, pipelined: {by_pool_size: {N: {inline_ops_s, offload_ops_s}}}, inflection_pool_size}`.

The "inflection point on real NVMe" is the open question called out in the PR body's Open Question 6.

### `run-all.sh` (orchestrator)

```
run-all.sh --tier=kind|remote [--skip 40,50] [--keep-cluster]
```

Order: setup → 10-deploy → 20 → 30 → 40 → 50 → 99-teardown (unless `--keep-cluster`). Each test's failure is logged but doesn't abort the suite. Final exit code is the OR of all sub-exit-codes. Writes `$RESULTS_DIR/summary.md` aggregating all JSONs into one human-readable table.

## Setup scripts

### `00-setup-kind.sh` (local tier)

1. **Tool installs (idempotent):** `kind` v0.25 → `~/.local/bin/kind` (single-binary download from GitHub release). No helm — KubeRay operator installed via raw manifests.
2. **Cluster create** with `/tmp/rep64-kind-config.yaml`:
   ```yaml
   kind: Cluster
   apiVersion: kind.x-k8s.io/v1alpha4
   nodes:
     - role: control-plane
     - role: worker
     - role: worker
   ```
   Multi-node so head + workers can land on different nodes — `30-pod-delete.sh` becomes a real test of pod-delete-then-reattach, not same-node restart.
3. **Cgroup-v1 sanity check.** Mariner is cgroup-v1 + systemd; kind v0.25 handles this fine but the script explicitly probes `docker info | grep CgroupVersion` and aborts with a clear message if cgroup-v2 conflicts surface.
4. **Set kubectl context** to `kind-$KIND_CLUSTER_NAME`.
5. **Create namespace** `$NAMESPACE` (idempotent).
6. **Install KubeRay operator pinned to v1.6.0** to match production:
   ```bash
   kubectl apply --server-side -f https://github.com/ray-project/kuberay/releases/download/v1.6.0/kuberay-operator.yaml
   ```
   Wait for operator deployment Ready.
7. **Image load:** `kind load docker-image $IMAGE --name $KIND_CLUSTER_NAME` (~30–60s for the 2.17 GB image, one time per cluster). After this, `IMAGE_PULL_POLICY=Never` works.
8. **Default StorageClass check:** `kubectl get storageclass standard`.
9. **Optional substrate diversity** (gated by `--with-substrate-diversity`, OFF by default). When ON: the script reorders itself so the loopback FS prep happens **before** step 2's cluster create, the kind config includes `extraMounts` exposing each loopback under the node, and the image-load (step 7) runs after cluster create as usual. Workflow: create 5 GB loopback files, `mkfs.ext4`/`mkfs.xfs`/`mkfs.btrfs`, mount under `/tmp/rep64-fs/{ext4,xfs,btrfs}`, write the kind config with the matching `extraMounts`, then proceed through cluster create / image load / SC registration. The `loop-ext4`/`loop-xfs`/`loop-btrfs` StorageClasses are registered after cluster create, backed by static PVs pointing at the in-node mount paths. Requires `sudo` + `xfsprogs` + `btrfs-progs` on the host. Default OFF because the loopback prep is a one-way step (un-prepping requires umount + rm).

### `00-check-remote.sh` (remote tier — verifies, never mutates)

Verifies, in order:

- `kubectl config use-context $KUBE_CONTEXT` succeeds.
- For each verb/resource in the test surface (`create rayclusters.ray.io`, `create persistentvolumeclaims`, `create configmaps`, `create secrets`, `create pods`, `delete pods`), `kubectl auth can-i ... -n $NAMESPACE` returns yes.
- CRDs `rayclusters.ray.io` and `rayjobs.ray.io` are installed.
- A `kuberay-operator` deployment exists somewhere in the cluster; prints its image tag.
- `STORAGE_CLASS` exists; every entry in `SUBSTRATE_SWEEP_CLASSES` exists.
- Configured `IMAGE` is printed (soft check — operator-side pull verifies it on first deploy).

Outputs a one-page green/red checklist. Safe to run repeatedly.

### `99-teardown.sh`

- kind: `kind delete cluster --name $KIND_CLUSTER_NAME` (and unmount loopback FS if substrate-diversity was used).
- remote: `kubectl delete raycluster --all -n $NAMESPACE` and `kubectl delete pvc --all -n $NAMESPACE`, with a confirmation prompt (irreversible). Never deletes the namespace itself — collaborator-owned, may have other things.

## Result schema

### Envelope (every test)

```json
{
  "schema_version": 1,
  "test_name": "30-pod-delete",
  "tier": "kind",
  "started_at": "2026-05-08T18:30:00Z",
  "duration_s": 87.4,
  "status": "pass",
  "skip_reason": null,
  "cluster": {
    "kube_context": "kind-rep64",
    "namespace": "rep64-poc",
    "kuberay_operator_version": "v1.6.0",
    "node_count": 3,
    "k8s_server_version": "v1.30.4"
  },
  "image": "cr.ray.io/rayproject/ray:nightly",
  "image_digest": "sha256:abc123…",
  "git_commit": "69a8139361…",
  "config": {
    "storage_class": "standard",
    "actor_count": 10,
    "head_cpu": "2",
    "head_memory": "4Gi"
  },
  "metrics": { /* test-specific */ },
  "errors": [ /* structured errors if status=fail */ ]
}
```

### Per-test metrics

```yaml
20-actor-survival:
  metrics:
    actors_created: int
    actors_recovered: int
    actors_lost: int
    named_lookup_ok: bool
    detached_survived: bool
    state_match_pct: float

30-pod-delete:
  metrics:
    pod_restart_s: float                   # T2 - T1
    state_preserved_pct: float
    new_tasks_executed: int
    backend: "rocksdb" | "inmem"           # baseline run uses "inmem"

40-substrate-sweep:
  metrics:
    by_storage_class:
      <sc-name>:
        fsync_verdict: "honest" | "lying"
        fsync_p50_us: float
        fsync_p99_us: float
        microbench_sync_write_p50_us: float
        microbench_read_p50_us: float

50-fast-storage:
  metrics:
    sequential:
      inline_p50_us: float
      offload_p50_us: float
    pipelined:
      by_pool_size:
        "<n>":
          inline_ops_s: float
          offload_ops_s: float
    inflection_pool_size: int
```

### `summary.md` format

`scripts/aggregate.py` (~50 lines) reads every JSON in `$RESULTS_DIR/`, emits:

```
## Cluster
- tier:    kind
- context: kind-rep64
- ns:      rep64-poc
- image:   cr.ray.io/rayproject/ray:nightly @ sha256:abc123…
- commit:  69a8139361…
- kuberay: v1.6.0  |  k8s: v1.30.4  |  3 nodes

## Tests
| test                | status | duration | key metric                            |
|---------------------|--------|----------|---------------------------------------|
| 20-actor-survival   | pass   | 23s      | 10/10 actors recovered                |
| 30-pod-delete       | pass   | 87s      | pod_restart 12.4s, state 100%         |
| 30-pod-delete.inmem | fail   | 91s      | pod_restart 14.1s, state 0% ⚠ baseline|
| 40-substrate-sweep  | skip   | 0s       | no SUBSTRATE_SWEEP_CLASSES set        |
| 50-fast-storage     | skip   | 0s       | FAST_NVME_CLASS not set               |
```

### Files-on-disk policy

```
results/
├── .gitignore                      # everything except *.committed.json + canonical/
├── kind/                           # gitignored
├── remote/                         # gitignored
└── canonical/
    ├── kind-baseline.committed.json
    └── remote-baseline.committed.json
```

Scratch runs stay local. Curated reference results — a known-good kind run, eventually a known-good remote run — land under `canonical/` with a `.committed.json` suffix that the `.gitignore` whitelists. Cross-tier diff is `diff <(jq '.metrics' kind/30-pod-delete-*.json) <(jq '.metrics' remote/30-pod-delete-*.json)`.

## Image distribution

The recommended default for external collaborators is the **public** image already on ghcr.io: `ghcr.io/jhasm/ray-rocksdb-poc:rep-64-poc` (also tagged `:69a81393` for the exact commit). `remote.env.example` has this as the default value of `IMAGE`. Most collaborator clusters can pull from ghcr.io directly; for those that cannot, fallbacks below.

### Fallback A — `05-deploy-registry.sh` (in-namespace registry)

For clusters where the operator can't reach ghcr.io. Deploys `registry:2` as a Pod in `$NAMESPACE`, fronted by a `ClusterIP` Service `rep64-registry.<NAMESPACE>.svc.cluster.local:5000`. Workflow:

1. `05-deploy-registry.sh` applies the manifest, waits for the Pod, opens a `kubectl port-forward` on `localhost:5000`.
2. Host: `docker pull ghcr.io/jhasm/ray-rocksdb-poc:rep-64-poc && docker tag ... localhost:5000/ray:rep-64-poc && docker push localhost:5000/ray:rep-64-poc`.
3. User updates `IMAGE` in `remote.env` to the in-cluster Service DNS.
4. KubeRay pulls from the in-cluster registry — no external network required.

Constraints: the cluster's container runtime must trust the in-namespace registry (`insecure-registries` may need to allow it; varies by cluster). Cross-namespace Service DNS resolution is the K8s default. The registry Pod's PVC needs ~5 GB.

### Fallback B — Kaniko build (deferred)

If neither ghcr.io nor an in-cluster registry works for a collaborator's cluster, document a Kaniko-based in-cluster build from the branch source. Slow (~30–60 min cold) and additive complexity. Don't script unless we hit that wall.

### LinkedIn-internal registry (author's own follow-on)

The author's `prod-ltx1-k8s-1` can pull from ghcr.io directly via `IMAGE_PULL_POLICY=IfNotPresent` (verified in remote-tier setup). If a future LinkedIn-internal mirror is preferred for performance, the workflow is well-understood (look at neighbor pods in `kk-flyte-prod` for the convention; LinkedIn `linkedin-cli-tools` plugin covers internal-registry push). This is not a blocker for the harness.

## Implementation sequencing

1. **Phase A — kind tier (no external dependencies):** scaffold `harness/k8s/`, write `lib.sh`, `00-setup-kind.sh`, `10-deploy-cluster.sh`, `manifests/raycluster.yaml.tmpl`. Get a single RayCluster up on kind with the rocksdb backend, verify head + worker Ready. End state: `kubectl exec` into head, see RocksDB DB at `/data/gcs`.
2. **Phase B — first test green on kind:** `20-actor-survival.sh` end-to-end. Reuse existing `harness/integration/test_rocksdb_recovery.py`. End state: green JSON with 10/10 actors recovered.
3. **Phase C — pod-delete on kind:** `30-pod-delete.sh` with the inmem-baseline. End state: rocksdb run preserves state, inmem run loses state, both runs visible in `summary.md`.
4. **Phase D — sweep + fast-storage on kind:** `40-substrate-sweep.sh` and `50-fast-storage.sh` with stub `kind.env` configs. End state: both scripts run; sweep is a no-op, fast-storage is a no-op, but the orchestrator + `summary.md` handle skips cleanly.
5. **Phase E — remote tier validation:** point a `remote.env` at `prod-ltx1-k8s-1` / `kk-flyte-prod` (or any external cluster), run `00-check-remote.sh` and `run-all.sh --tier=remote`. End state: a `remote-baseline.committed.json` ready to land alongside the kind one.
6. **Phase F — substrate sweep on real storage:** populate `SUBSTRATE_SWEEP_CLASSES` and `FAST_NVME_CLASS` in `remote.env`, get real numbers across `local-nvme` / `local-ssd` / `nfs-fast-provision-v3`. End state: per-StorageClass numbers in `summary.md`; substrate evidence updated in `EVIDENCE.md` from "ext4 single-substrate verified" to a multi-substrate matrix.

Phases A–D are purely local. Phase E is the only dependency on cluster access.

## Open items

- **Substrate-diversity flag for kind**: documented as opt-in (`--with-substrate-diversity`). Whether to make it the default is deferred until we have a clean repro of `mkfs.xfs` / `mkfs.btrfs` working unattended on Mariner. If not, opt-in stays.
- **Comparison-with-Redis** for the pod-delete number (`COLLABORATORS.md` #5 bonus): out of scope for this harness; would require a separate Redis-backed RayCluster + a deliberate failure mode. Tracked as a follow-on, not a goal.

## What this harness will NOT prove

- Multi-AZ recovery (single-cluster only).
- Behavior under sustained chaos (no chaos-test framework here; a `chaos_rocksdb_store_client_test.cc` mirroring `chaos_redis_store_client_test.cc` is a follow-on).
- Performance on hardware different from what runs the harness — these are *correctness* tests on kind and *one-cluster real-storage* numbers on the remote tier. Real-fleet performance would need a multi-cluster sweep, which is out of scope.

## Decision log

- **kind, not k3d / minikube.** Existing harness directory is named `kind/`; kind v0.25 is what KubeRay's own docs reference; no compelling reason to add a second local-cluster tool. (k3d is faster to start, ~10s vs ~30s — not enough win.)
- **Raw KubeRay manifest, not Helm.** Avoids a tool dependency; pinning to v1.6.0 is one URL.
- **`envsubst`, not Helm or Kustomize, for templates.** Bash-only; no learning curve for collaborators.
- **No `kubectl wait` magic strings inside test scripts** — everything goes through `lib.sh::wait_ray_ready` so timeouts are tunable in one place.
- **Tier 2's image default is the public ghcr.io package**, not an in-cluster registry. The latter is documented as a fallback. Optimizes for the common case where collaborators have egress to ghcr.io.
