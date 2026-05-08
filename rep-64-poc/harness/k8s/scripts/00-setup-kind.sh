#!/usr/bin/env bash
# 00-setup-kind.sh — create a local kind cluster and install KubeRay v1.6.0.
# Idempotent. Reads HARNESS_ENV_FILE for KUBE_CONTEXT, NAMESPACE, KIND_CLUSTER_NAME, IMAGE.

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
source "$HERE/../lib.sh"
load_env
require_env KIND_CLUSTER_NAME

# 1. Ensure kind is installed (single-binary download).
if ! command -v kind >/dev/null; then
  log "installing kind v0.25.0 to ~/.local/bin/kind"
  mkdir -p "$HOME/.local/bin"
  curl -fsSL -o "$HOME/.local/bin/kind" \
    "https://kind.sigs.k8s.io/dl/v0.25.0/kind-linux-amd64"
  chmod +x "$HOME/.local/bin/kind"
  export PATH="$HOME/.local/bin:$PATH"
fi

# 2. Probe Docker cgroup (Mariner is cgroup-v1 + systemd; kind handles it).
docker info --format '{{.CgroupVersion}}/{{.CgroupDriver}}' \
  | grep -E '^(1|2)/(systemd|cgroupfs)$' >/dev/null \
  || abort "unsupported cgroup config: $(docker info --format '{{.CgroupVersion}}/{{.CgroupDriver}}')"

# 3. Create the kind cluster if it doesn't exist.
if ! kind get clusters | grep -qx "$KIND_CLUSTER_NAME"; then
  log "creating kind cluster $KIND_CLUSTER_NAME (3 nodes)"
  cat > /tmp/rep64-kind-config.yaml <<EOF
kind: Cluster
apiVersion: kind.x-k8s.io/v1alpha4
nodes:
  - role: control-plane
  - role: worker
  - role: worker
EOF
  kind create cluster --name "$KIND_CLUSTER_NAME" --config /tmp/rep64-kind-config.yaml
else
  log "kind cluster $KIND_CLUSTER_NAME already exists"
fi

# 4. kubectl context set after kind create cluster (it does this for us, but be explicit).
kubectl config use-context "$KUBE_CONTEXT" >/dev/null

# 5. Namespace (idempotent).
kubectl create namespace "$NAMESPACE" --dry-run=client -o yaml | kubectl apply -f -

# 6. Install KubeRay operator pinned to v1.6.0.
if ! kubectl get deploy -n default kuberay-operator >/dev/null 2>&1; then
  log "installing KubeRay operator v1.6.0"
  kubectl apply --server-side -f \
    "https://github.com/ray-project/kuberay/releases/download/v1.6.0/kuberay-operator.yaml"
fi
kubectl rollout status deploy/kuberay-operator -n default --timeout=180s

# 7. Load the image into kind nodes.
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  abort "$IMAGE not found in local docker daemon — build it first via build-image.sh"
fi
log "loading $IMAGE into kind cluster (this takes ~30-60s for the 2.17 GB image)"
kind load docker-image "$IMAGE" --name "$KIND_CLUSTER_NAME"

# 8. StorageClass sanity.
kubectl get storageclass "$STORAGE_CLASS" >/dev/null \
  || abort "StorageClass $STORAGE_CLASS not found in cluster"

log "kind cluster ready: ctx=$KUBE_CONTEXT ns=$NAMESPACE img=$IMAGE"
