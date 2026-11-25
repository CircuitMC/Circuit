# Benchmarks

Programs and scripts that measure Circuit on a release build. None of them is
part of the default build or of the checks.

| File | Purpose |
| --- | --- |
| `engine_bench.cpp` | Tick time of the simulation engine on a workload |
| `workloads.py` | Writes the constructed workloads as `.wl` files |
| `run.sh` | Runs `engine_bench` pinned, repeated, over worker counts, and writes tables |
| `table.py` | Turns `engine_bench` output into Markdown tables |
| `storage_bench.cpp` | Checkpoint encoding and background checkpoints ([storage evaluation](../../docs/STORAGE_EVALUATION.md)) |
| `chunk_bench.cpp` | Chunk packet encoding ([performance](../../docs/PERFORMANCE.md)) |

## Engine tick time

```bash
tools/bench/run.sh --build        # release build, the baseline set, tables
tools/bench/run.sh --reps 5 --workers "1 8" observer_clocks iso_dust_16 players_64@200
```

`--build` configures the tree for release mode; `tools/check.sh` configures it
for debug again. The results go to `build/bench/<UTC time>/`: `runs.txt` holds
one line per run, `machine.txt` the machine and the load average before and
after, and `table.md` the tables. [Performance](../../docs/PERFORMANCE.md)
records a baseline and explains how to read it.

A run with W workers is pinned with `taskset` to W CPUs starting at CPU 8
(`--first-cpu`). Every cell of a table is the median of the repetitions, and
the spread is (max - min) / median of their mean tick times. Close other work
before a run and look at the load average in `machine.txt`: a busy machine
shows up as spread.

One program run prints one line:

```
build/linux/x86_64/release/engine_bench tests/fixtures/workloads/observer_clocks.ctrace --workers 4 --ticks 600
RUN wl=observer_clocks.ctrace label=- workers=4 ticks=600 task_times=0 total_ms=... mean=... p50=... p95=... p99=... max=... digest=0x...
```

- `mean`, `p50`, `p95`, `p99` and `max` are tick times in milliseconds.
- `ph_<phase>`, `light`, `runner`, `commit`, `outside_runner` and `layout_rest`
  are totals over the timed ticks in milliseconds.
- `work` and `critical` (time in rule code, and the longest task of every
  runner call) are filled only with `--task-times`. That option makes the
  engine read the clock twice for every root it runs, which adds up to 16 % to
  a tick; leave it off when the tick time is what is measured.
- `digest` is the digest of the world after the last tick. It must not depend
  on the worker count or on the repetition; `table.py` checks that.

## Workloads

**Recorded** workloads are the five `.ctrace` files of
`tests/fixtures/workloads/` (`tools/generate_workloads.py`). They run exactly
as `circuit-server --profile-fixture` runs them: setup, the fixture's settle
time, then the timed ticks. A fixture's area is at most 128 blocks wide.

**Constructed** workloads are written by `workloads.py` into
`build/bench/workloads/` and are not committed (about 4 MB). They use a small
line format without the area limit:

| Line | Meaning |
| --- | --- |
| `# text` | Comment |
| `settle N` | Ticks to run before the timed ones (40) |
| `ticket ID CX CZ RADIUS [forced\|player]` | A chunk ticket added at tick 1. `player` adds the two tickets a server gives a player: a loading ticket of that radius and a simulation ticket of the smaller of the radius and 10 |
| `presence ID X Y Z` | A player's position, from tick 1 |
| `floor Y [BLOCK]` | Every new column gets one layer of the block at that height (stone) |
| `rule NAME VALUE` | A game rule, set at tick 2 |
| `norules` | Do not apply the fixture runner's rules |
| `at T set X Y Z BLOCK` | Place a block at setup tick T |
| `at T use X Y Z` | Use a block at setup tick T |
| `at T insert X Y Z SLOT ITEM COUNT` | Put items into a container at setup tick T |
| `every PERIOD PHASE <set\|use\|insert> ...` | The same actions at every tick t with t % PERIOD == PHASE |

Unless a file says `norules`, the fixture runner's game rules apply as they do
to recorded workloads: `randomTickSpeed 0`, no daylight cycle and no weather
cycle. The server-like workloads (`idle_16_*`, `*_grass`, `players_*`) set the
server's values with `rule` lines.

| Family | Contents |
| --- | --- |
| `iso_<kind>_<size>` | A square of chunks with one group of machines per chunk, every block at chunk-local 3 to 12 so that no rule touches a neighbouring column. `observer`, `dust` and `hopper` draw no random numbers; `piston`, `dropper` and `items` do |
| `idle_16_stone`, `idle_16_grass` | 441 loaded columns with a floor, the server's game rules and one player, and no machine |
| `players_N` | N idle players 640 blocks apart on a stone floor, each loading a view distance of 10 (625 loaded columns per player) |
