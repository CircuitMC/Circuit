# Headless simulation core

## Status

The Part I core runs headless: shared contracts, a set of reference rules, an independent sequential oracle, and a region-parallel runtime checked against the oracle. It opens no network port and has no persistence.

The rules are **circuit-reference-rules-v0**, Circuit's own definitions written from publicly documented Java Edition behavior. No mechanism has been checked against Java Edition 1.21.8: the instrumented reference server that would provide target-version traces does not exist yet. Passing tests here shows that the parallel runtime reproduces the reference rules exactly. It does not show vanilla compatibility.

The compatibility table is part of the version bundle (`src/contracts/version.cpp`, exposed through `BundleRegistry`). Each row states a target tier (L0 exact, L1 region-equivalent, L2 declared deviation), the tier currently claimed, the evidence source, and the known limitations. No row claims L0. Rows with known simplifications claim L2.

## Code layout

| Path | Contents |
| --- | --- |
| `include/circuit/contracts/` | Coordinates and identities, generations, logical time and order keys, stable error codes, commands, trace records, version bundles, replay records |
| `include/circuit/gameplay/`, `src/gameplay/` | Block registry, `RuleContext`, and the reference rules |
| `include/circuit/runtime/`, `src/runtime/` | `Engine`, region contexts, job runners, tick clock |
| `tests/oracle/` | Independent sequential executor, never linked into the server |
| `tests/rules/` | Rule fixtures with hand-derived expectations |
| `tests/sim/` | Differential scenarios: runtime against oracle |
| `tests/control/`, `tests/contracts/` | Runner, clock, digest and contract tests |

Dependencies point one way: `contracts` ← `gameplay` ← `runtime` ← `app`. Rules see the world only through `RuleContext`, so the oracle and the runtime execute the same rule functions over different storage and ordering machinery.

## Tick structure

Logical time advances by one per executed tick. Each tick runs these phases in order. Within a phase, work is a sequence of *roots* in a fixed serial order. Immediate reactions (neighbor updates) finish inside their root.

| Phase | Roots in serial order |
| --- | --- |
| commands | admitted commands by admission id, then one root applying chunk-level changes |
| block ticks | scheduled block ticks due now in ticking chunks, by (due, priority, sub-order), at most 65,536 |
| fluid ticks | scheduled fluid ticks, same order and limit |
| random ticks | ticking chunks in coordinate order, then sections bottom to top |
| block events | the level-wide FIFO, including events queued while it drains; events in non-ticking chunks stay queued in order |
| entities | entities in ticking chunks, ascending id, at most once per tick |
| block entities | block entities in ticking chunks, ascending creation order |

Every observable action produces a trace record keyed by (tick, phase, root, sequence within the root). Two executions are equivalent only if their trace records are identical, including order. Their canonical state dumps must also be identical.

## Reference rules v0

- **Neighbor updates:** a changed block notifies its six neighbors in the order west, east, down, up, north, south. Notifications emitted during one delivery run depth-first in emission order. A chain stops after 1,000,000 deliveries, and the truncation is recorded.
- **Redstone:** levers, redstone blocks, dust (same-level horizontal connections, decay one per block), repeaters (delay 1–4, pulse extension), lamps (4-tick turn-off), and observers (2-tick delay and pulse). There is no power conduction through solid blocks.
- **Scheduled ticks:** at most one pending tick per (position, block). The level-wide sub-order counter is shared by block and fluid ticks.
- **Pistons:** regular, sticky and slime structures of up to 12 blocks, with quasi-connectivity. A move completes inside one block event and has no moving-block states. A piston acts at most once per tick and re-checks its power on the next tick.
- **Hoppers and chests:** push before pull, 8-tick cooldown, disabled while powered, and pickup of item entities in the cell above.
- **Water:** sources, flowing distance 1–7, falling water, 5-tick fluid ticks.
- **Wheat:** grows on random ticks. Random-tick positions and rule draws use a stream derived from (seed, tick, chunk, section). This is a declared L1 choice relative to a level-wide stream.
- **Item entities:** fixed-point gravity, drag and collision, migration between chunks, despawn after 6000 ticks.
- **Chunk tickets:** a ticket of radius r makes chunks within Chebyshev distance r ticking and the next ring border-loaded. Changes apply at the end of the command phase. Unloading keeps all chunk state, including pending ticks. A reload increments the chunk generation, and commands can name the generation they expect.

## Region-parallel runtime

**Ownership.** Every loaded chunk column belongs to exactly one region. A region task runs its roots in the phase's serial order and may touch only its own columns. A read or write of a column owned by another region throws before touching it.

**Conflicts.** The coordinator rolls back an aborted attempt using copy-on-first-write snapshots of the written columns. It then merges the two regions, discards any staged results of the merged regions, and runs the merged region again. Regions that never touched each other keep their results. Each conflict reduces the number of regions, so a phase always finishes; in the worst case everything becomes one region and runs serially.

**Level-wide counters.** Scheduled-tick sub-orders and block-event queue positions are not drawn from shared counters during a phase. New entries carry their (root, sequence) key. At commit, the coordinator sorts all staged entries and assigns counter values in serial order. Entity ids and block-entity creation order are assigned only in the serial command phase.

**Block-event order.** Each region drains its own FIFO. At commit, the global FIFO order is rebuilt from the initial queue and the children each event appended. Trace records are renumbered to their global positions.

**Why layout cannot change results.** Within a phase, regions that finished without conflict accessed disjoint columns. Their roots therefore commute with each other's roots and see exactly the column states they would see in the serial order. Region layout decides only what runs concurrently.

**Layouts.** `adaptive` merges on conflict and, every 20 ticks, splits regions whose columns have not interacted for 40 ticks. `single_region` runs everything through one region. `perturbed` applies seeded random merges and splits for testing.

**Threads.** `PoolRunner` runs each region task as one job on the Asio pool; workers never wait for other jobs. `SeededRunner` interleaves task steps deterministically on one thread from a seed, optionally injecting aborts, so any failure replays exactly.

## Verification

- `rules_tests`: hand-derived expectations for every mechanism, including chunk-edge crossings, a chain returning across an edge within one tick, pulse extension, priority order, chain truncation, piston limits, conservation in hopper races, water timing, stream locality, generation checks, command validation and capacity failure.
- `simulation_tests`: nine scenarios compared with the oracle trace-by-trace every tick and state-by-state at checkpoints. Configurations: 1/2/4/8/16 pool workers, a single region, seeded random and priority interleavings, injected aborts, and perturbed layouts. A deliberately unsafe mode without ownership checks must produce a detected divergence.
- `control_tests`: seeded-schedule reproducibility, tick-clock catch-up, and the state digest.
- `evidence_tests`: the [trace fixture format](TRACE_FORMAT.md), the content mapping (every reference state round-trips through target-version notation), recording and evaluation on both the engine and the oracle, and the committed fixtures in `tests/fixtures/`.

GCC ThreadSanitizer runs once ASLR is disabled for the test process (`setarch "$(uname -m)" -R`). See the [development guide](DEVELOPMENT.md).

## Running

```bash
xmake run circuit-server --simulate --ticks 300 --workers 4 --record "$PWD/demo.crpl"
xmake run circuit-server --simulate --replay "$PWD/demo.crpl" --workers 16
xmake run circuit-server --simulate --ticks 200 --realtime
```

`--simulate` without `--replay` runs a built-in demonstration with machines that straddle chunk edges. The output includes a 64-bit state digest; equal digests across worker counts are a quick check, not proof. `--realtime` paces ticks at 20 Hz. When it falls more than 40 ticks behind, it drops the wall-clock backlog; logical ticks are never skipped.

Fixtures in [circuit-trace format](TRACE_FORMAT.md) can be checked or recorded with `--check-fixture <file>` and `--record-fixture <skeleton> --output <file>`.

## Limitations

- No mechanism is verified against the target version. Pistons, dust, water, wheat and item physics are declared simplifications.
- There is no lighting, world generation (chunks start empty), mob spawning, comparator, double chest or second dimension.
- Snapshots copy whole column data on first write, and commit is serial. On the current small fixtures, coordination overhead outweighs parallel gains; performance work belongs to M8.
- A capacity failure, such as exceeding 1,000,000 block events in one tick, fails the tick and leaves the engine unusable. Nothing is silently dropped.
