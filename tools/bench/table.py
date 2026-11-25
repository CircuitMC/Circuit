#!/usr/bin/env python3
"""Turns engine_bench output into Markdown tables.

usage: table.py <runs.txt> [scaling|digests|phases]

Each cell is the median over the repetitions of one workload and worker count.
`spread` is (max - min) / median of the repetitions' mean tick time. The
script exits with status 1 when a workload's digest differs between runs: the
worker count must never change the result.
"""
import collections
import statistics
import sys

PHASES = ["ph_commands", "ph_block_ticks", "ph_fluid_ticks", "ph_chunks", "ph_random_ticks", "ph_block_events", "ph_entities",
          "ph_block_entities", "light", "layout_rest"]
PHASE_TITLES = ["commands", "block ticks", "fluid ticks", "chunk stage", "random ticks", "block events", "entities", "block entities",
                "light (within the phases)", "between the phases"]


def read(path):
    runs, order = collections.defaultdict(list), []
    with open(path) as f:
        for line in f:
            if not line.startswith("RUN "):
                continue
            fields = dict(token.split("=", 1) for token in line.split()[1:])
            name = fields["wl"].rsplit(".", 1)[0]
            if name not in order:
                order.append(name)
            runs[(name, int(fields["workers"]))].append(fields)
    return runs, order


def median(rows, field, per_tick=False):
    return statistics.median(float(r[field]) / (float(r["ticks"]) if per_tick else 1.0) for r in rows)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    runs, order = read(sys.argv[1])
    mode = sys.argv[2] if len(sys.argv) > 2 else "scaling"
    workers = sorted({w for _, w in runs})
    status = 0
    if mode == "scaling":
        print("| Workload | Workers | mean ms | p50 | p95 | p99 | against 1 worker | spread | runs |")
        print("| --- | --- | --- | --- | --- | --- | --- | --- | --- |")
        for name in order:
            base = None
            for w in workers:
                rows = runs.get((name, w))
                if not rows:
                    continue
                mean = median(rows, "mean")
                base = base if base is not None else mean
                means = [float(r["mean"]) for r in rows]
                spread = (max(means) - min(means)) / mean * 100
                print(f"| {name} | {w} | {mean:.3f} | {median(rows, 'p50'):.3f} | {median(rows, 'p95'):.3f} | {median(rows, 'p99'):.3f} | "
                      f"{base / mean:.2f}x | {spread:.0f} % | {len(rows)} |")
    elif mode == "digests":
        print("| Workload | Timed ticks | Loaded columns | Regions | Tasks per attempt | Digest | Same in every run |")
        print("| --- | --- | --- | --- | --- | --- | --- |")
        for name in order:
            rows = [r for w in workers for r in runs.get((name, w), [])]
            digests = sorted({r["digest"] for r in rows})
            same = len(digests) == 1
            status |= 0 if same else 1
            attempts = median(rows, "attempts")
            tasks = median(rows, "tasks") / attempts if attempts else 0.0
            print(f"| {name} | {rows[0]['ticks']} | {rows[0]['loaded']} | {rows[0]['regions']} | {tasks:.1f} | "
                  f"`{' '.join(digests)}` | {'yes' if same else 'NO'} |")
    elif mode == "phases":
        # Microseconds per tick, at the smallest worker count that was run.
        print("| Workload | Workers | tick µs | " + " | ".join(PHASE_TITLES) + " | rule work | critical path | in the job runner | commit |")
        print("| --- | --- | --- | " + " | ".join("---" for _ in PHASE_TITLES) + " | --- | --- | --- | --- |")
        for name in order:
            for w in workers:
                rows = runs.get((name, w))
                if not rows or w != min(v for n, v in runs if n == name):
                    continue
                us = lambda field: f"{median(rows, field, per_tick=True) * 1000:.0f}"
                timed = all(r.get("task_times") == "1" for r in rows)
                print(f"| {name} | {w} | {median(rows, 'mean') * 1000:.0f} | " + " | ".join(us(p) for p in PHASES)
                      + f" | {us('work') if timed else '-'} | {us('critical') if timed else '-'} | {us('runner')} | {us('commit')} |")
    else:
        sys.exit(__doc__)
    sys.exit(status)


if __name__ == "__main__":
    main()
