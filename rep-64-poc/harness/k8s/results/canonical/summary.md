# Test summary — k3d tier

## Cluster
- tier:    k3d
- context: k3d-rep64
- ns:      rep64-poc
- image:   cr.ray.io/rayproject/ray:nightly-rep64-fix
- commit:  cbea070f2f27035110c7d338c4b7e582d9467377
- kuberay: unknown  |  k8s: v1.30.4+k3s1  |  2 nodes

## Tests
| test                | status | duration | key metric |
|---------------------|--------|----------|------------|
| 20-actor-survival   | pass   |    7s | 10/10 actors recovered |
| 30-pod-delete       | fail   |  140s | pod_restart 138.48s, state 40% |
| 30-pod-delete.inmem | fail   |   12s | pod_restart 10.44s, state 0% |
| 40-substrate-sweep  | skipped |    0s | SUBSTRATE_SWEEP_CLASSES not configured |
| 50-fast-storage     | skipped |    0s | FAST_NVME_CLASS not configured for this tier |

## Findings
3 finding(s) across 5 test(s).  Each row below is a metric that fell outside its spec target, or a test that did not complete.

| test | backend | metric | spec target | observed | source |
|------|---------|--------|-------------|----------|--------|
| 30-pod-delete | rocksdb | pod_restart_s | <= 30 (head pod recovers within 30s) | 138.48 | 30-pod-delete.committed.json |
| 30-pod-delete | rocksdb | state_preserved_pct | >= 95 (actor state ≥95% preserved) | 40 | 30-pod-delete.committed.json |
| 30-pod-delete.inmem | memory | state_preserved_pct | >= 95 (actor state ≥95% preserved) | 0 | 30-pod-delete.inmem.committed.json |
