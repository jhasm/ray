#!/usr/bin/env python3
"""Reads every *.json in a results directory, emits summary.md."""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys


def _key_metric(test: str, metrics: dict) -> str:
    if test == "20-actor-survival":
        c = metrics.get("actors_created", 0)
        r = metrics.get("actors_recovered", 0)
        return f"{r}/{c} actors recovered"
    if test.startswith("30-pod-delete"):
        pr = metrics.get("pod_restart_s", "?")
        sp = metrics.get("state_preserved_pct", "?")
        return f"pod_restart {pr}s, state {sp}%"
    if test == "40-substrate-sweep":
        n = len(metrics.get("by_storage_class", {}))
        return f"{n} StorageClass(es) probed"
    if test == "50-fast-storage":
        ip = metrics.get("inflection_pool_size", "?")
        return f"inflection pool size: {ip}"
    return "(no key metric)"


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("results_dir")
    args = p.parse_args()

    files = sorted(glob.glob(os.path.join(args.results_dir, "*.json")))
    if not files:
        print(f"No JSON results in {args.results_dir}", file=sys.stderr)
        return 1

    # Latest run per test_name (test files are timestamped).
    latest: dict[str, dict] = {}
    for f in files:
        with open(f) as fh:
            r = json.load(fh)
        latest[r["test_name"]] = r

    # Header from any one envelope (cluster facts are identical across tests in a run).
    sample = next(iter(latest.values()))
    out = []
    out.append(f"# Test summary — {sample['tier']} tier")
    out.append("")
    out.append("## Cluster")
    out.append(f"- tier:    {sample['tier']}")
    out.append(f"- context: {sample['cluster']['kube_context']}")
    out.append(f"- ns:      {sample['cluster']['namespace']}")
    out.append(f"- image:   {sample['image']}")
    out.append(f"- commit:  {sample['git_commit']}")
    out.append(f"- kuberay: {sample['cluster']['kuberay_operator_version']}  "
               f"|  k8s: {sample['cluster']['k8s_server_version']}  "
               f"|  {sample['cluster']['node_count']} nodes")
    out.append("")
    out.append("## Tests")
    out.append("| test                | status | duration | key metric |")
    out.append("|---------------------|--------|----------|------------|")
    for name in sorted(latest):
        r = latest[name]
        km = _key_metric(name, r.get("metrics", {}))
        if r["status"] == "skipped":
            km = r.get("skip_reason") or "skipped"
        out.append(
            f"| {name:<19} | {r['status']:<6} | {int(r['duration_s']):>4}s "
            f"| {km} |"
        )

    summary_path = os.path.join(args.results_dir, "summary.md")
    with open(summary_path, "w") as fh:
        fh.write("\n".join(out) + "\n")
    print(f"wrote {summary_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
