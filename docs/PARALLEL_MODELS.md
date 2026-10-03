# Executable parallel reasoning models

These small C++23 models explore concurrency requirements for Part I (M1, M2, M3, M7). They are isolated from the server executable. They are not a Minecraft simulator, a production transaction engine, or evidence of exact vanilla behavior.

## Run

Configure dependencies as described in [the development guide](DEVELOPMENT.md), then run:

```bash
export XMAKE_GLOBALDIR="$PWD/.xmake-global"
xmake build parallel_models
xmake run parallel_models
```

`tools/check.sh` includes this executable. Its checks remain active in release builds. The model interfaces are in `include/circuit/exploration/models.hpp`; implementations are in `src/exploration/models.cpp`; scenarios and independent expectations are in `tests/exploration/parallel_models.cpp`.

## Scenarios and implications

| ID | Mechanism motivating the model | Failure exposed | Contract to carry into implementation |
| --- | --- | --- | --- |
| S01 | Independent chunks and read/write dependencies | Running a later writer before an earlier reader changes observations | Derive dependencies from complete resource footprints and established semantic order; parallelize only independent actions |
| S02 | A boundary chain returning to earlier chunks; delayed components | Two fixed rounds miss reactions; eagerly running future events changes time | Drain required immediate work without repeating basic ticks; preserve due times |
| S03 | A pulse observed by an edge-sensitive receiver | Final state is unchanged but the receiver should observe a transition | Preserve ordered events; do not coalesce edge-sensitive updates |
| S04 | Scheduled priorities and phase boundaries | Arrival order determines behavior or an earlier phase reopens after commit | Version rules assign semantic keys; reject late work until safe ordering is established |
| S05 | Two hoppers competing for one item | Both read the same item and duplicate it | Coordinate source, destination, logical inventory and timing state; publish effects only on success |
| S06 | Fuel insertion before a furnace update | A speculative read uses old fuel state | Validate read versions, then recompute from the same logical update without duplicate effects |
| S07 | A piston/structure reaching outside its planned footprint | Partial movement or an undeclared third-owner write | Re-resolve/reserve the footprint before commit and retain required internal ordering |
| S08 | Entity migration, including time-sensitive entities | A source and destination both advance the same entity | Carry last-tick identity through handoff; invalidate old ownership tickets |
| S09 | Chunk unload/reload with the same coordinates and values | Old asynchronous work affects a new object lifetime | Validate generations as well as revisions |
| S10 | Distant spawn attempts or shared logical resources | Geographically independent tasks exceed one global budget | Include non-spatial quota/resource owners in dependencies and validation |
| S11 | Pathfinding, collision candidates, or light-dependent queries | A result is consumed after its input changes | Separate read-only computation from validated authoritative use |
| S12 | Random update opportunities retried after conflict | A retry consumes an extra random opportunity | Capture decisions at the correct semantic point; preserve the version's RNG consumption contract |
| S13 | Empty queues while a producer is still active | A stage closes before descendant events arrive | Register all roots, register children before retiring parents, and count in-flight work |

S01 runs on the actual Asio pool at 1/2/4/8/16 workers with forward, reversed, and rotated submission plus yield injection. Expected values and per-action observations are explicit arithmetic constants. Other scenarios are deliberately sequential models of conflicting plans and invalid state transitions. They do not claim concurrent transaction correctness.

## What the models establish

`dependency_waves` adds a dependency from every earlier conflicting action to a later one. It includes read/write anti-dependencies, not just write/write conflicts. Actions in one wave touch disjoint mutable resources and may finish in any order. Results are collected into per-action slots before canonical publication.

That conclusion assumes the input order is already semantically valid and every effect is declared. Logical inventories, RNG streams, global counters, callbacks, and output ordering may all introduce dependencies beyond block coordinates. Hidden effects invalidate the model's independence argument. The quadratic planner is an inspectable reference tool, not a performance proposal.

`Agenda` orders explicit `(tick, phase, order)` keys and rejects duplicates or events behind its consumed order. Numeric phase/priority values are synthetic. Sorting available messages alone does not prove no earlier message is still in flight; callers need a separate dependency/completion protocol. Its horizon can drain overdue work without rewriting that work's original logical time.

`State` validates complete observed values, revisions and generations before publishing writes and an ordered outbox. All methods are coordinator-only. Copying the entire model state makes failure behavior easy to inspect, but is not suitable for a real world. It has no reservations, operation deduplication, durable log, permission checks, or actual callbacks.

In S07 the event outbox preserves labels for internal steps. It does not implement intermediate world views for piston callbacks. A real coordinated operation must reproduce those views and any required interleaving; copying final state and replaying labels is not sufficient.

`EntityClock` demonstrates lifetime and once-per-logical-tick advancement, not movement physics. `StageLedger` proves accounting only if all root producers were registered and every descendant receives a live ticket before its parent retires. It is single-threaded, has no network queue, and does not support mixing tickets from different ledger instances.

## Important cases still requiring version fixtures

- Hopper cooldowns, double-chest slot order, and furnace interaction ordering.
- Piston callbacks, moving block entities, collision impulses, and dynamically expanding access sets.
- Fluid scheduling versus a sponge's region traversal; support/shape changes versus later destruction.
- Tree generation footprints, shared RNG streams, and branch-dependent random consumption.
- Fast entities/projectiles whose swept path spans more than neighboring chunks; interacting collision groups.
- Lighting revision visibility when used by gameplay; stale read-only results alone do not define permitted lag.
- Vibration candidate selection, occlusion and arrival times, and non-spatial resource claims.
- Tick-stage closure under real transport/cancellation, entity unloading, and global-state changes.

For each mechanism, capture the target-version ordering and observable trace, implement an independent sequential rule fixture, then adapt the scheduler. A model pass must never be promoted into a compatibility claim merely because its case name resembles a Minecraft feature.
