"""KubeRay actor-survival test (COLLABORATORS.md item #6).

Runs *inside the Ray cluster* via RayJob submission. Creates N named detached
actors, builds state, snapshots, disconnects + reconnects, verifies state.

Result: writes a JSON metrics object to stdout (last line). Exits 0 on pass,
1 on fail. The wrapping shell script (20-actor-survival.sh) parses the last
stdout line as JSON.

Env vars:
  ACTOR_COUNT (default 10)
"""

from __future__ import annotations

import json
import os
import sys
import time

import ray


def _build_state(actors):
    """Each actor gets a deterministic counter + tag."""
    increments = list(range(1, len(actors) + 1))  # 1, 2, 3, ...
    ray.get([a.incr.remote(n) for a, n in zip(actors, increments)])
    return {f"rep64-actor-{i}": n for i, n in enumerate(increments)}


def main() -> int:
    n = int(os.environ.get("ACTOR_COUNT", "10"))
    print(f"[actor_survival] creating {n} named detached actors", flush=True)

    # Inside a RayJob, ray.init() with no args attaches to the cluster.
    ray.init()

    @ray.remote
    class Counter:
        def __init__(self):
            self.value = 0

        def incr(self, k: int) -> int:
            self.value += k
            return self.value

        def value_(self) -> int:
            return self.value

    actors = [
        Counter.options(name=f"rep64-actor-{i}", lifetime="detached").remote()
        for i in range(n)
    ]
    snapshot = _build_state(actors)
    print(f"[actor_survival] snapshot: {snapshot}", flush=True)

    # Disconnect.
    ray.shutdown()
    time.sleep(2)

    # Reconnect.
    ray.init()

    recovered = {}
    lost = []
    for name in snapshot:
        try:
            a = ray.get_actor(name)
            recovered[name] = ray.get(a.value_.remote())
        except Exception as e:  # noqa: BLE001
            lost.append((name, repr(e)))

    detached_survived = len(recovered) == n
    state_match = all(recovered.get(k) == v for k, v in snapshot.items())
    state_match_pct = 100.0 * sum(
        1 for k, v in snapshot.items() if recovered.get(k) == v
    ) / max(len(snapshot), 1)

    metrics = {
        "actors_created": n,
        "actors_recovered": len(recovered),
        "actors_lost": len(lost),
        "named_lookup_ok": all(k in recovered for k in snapshot),
        "detached_survived": detached_survived,
        "state_match_pct": state_match_pct,
    }

    # Print metrics on the LAST line so the wrapper can parse it.
    print("METRICS_JSON " + json.dumps(metrics), flush=True)

    return 0 if detached_survived and state_match else 1


if __name__ == "__main__":
    sys.exit(main())
