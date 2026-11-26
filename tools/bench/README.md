# Benchmarks

The first generation's benchmark programs measured its region-parallel engine
and were removed with it (tag `pre-refactor`, `tools/bench/`). The driver
returns with the new runtime and measures it, the official server, Paper and
Folia on the same workloads.

| File | Purpose |
| --- | --- |
| `workloads.py` | Writes the constructed workloads as `.wl` files into `build/bench/workloads/` |
| `table.py` | Turns benchmark output lines into Markdown tables |

## Workloads

**Recorded** workloads are the five `.ctrace` files of
`tests/fixtures/workloads/` (`tools/generate_workloads.py`): setup, the
fixture's settle time, then the timed ticks. A fixture's area is at most 128
blocks wide.

**Constructed** workloads are written by `workloads.py` and are not committed
(about 4 MB). They use a small line format without the area limit:

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
