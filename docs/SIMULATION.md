# Headless simulation core

## Status

The Part I core runs headless: shared contracts, a set of reference rules, a sequential scheduling oracle, and a region-parallel runtime checked against the oracle. The oracle shares gameplay rules with the runtime; it checks scheduling equivalence, while external reference observations and hand-derived expectations check the rules themselves. The [network server](SERVER.md) and [persistence](PERSISTENCE.md) are built on it.

The rules are **circuit-reference-rules-v7**. They are Circuit's own definitions, written from public documentation and from traces recorded by an instrumented server outside this repository.

Two version bundles run them: `java-1.21.8` and `java-1.21.4`. Each version has 30 traces recorded on the unmodified server:
- 1.21.8 traces are in `tests/fixtures/harness/`;
- 1.21.4 traces are in `tests/fixtures/harness-1.21.4/`.

The 1.21.4 traces equal the 1.21.8 traces apart from their headers. All 60 pass on both the parallel runtime and the oracle.

The compatibility table is part of the version bundle (`src/contracts/version.cpp`). `circuit-server --compatibility` prints it. Each row states a target tier, a claimed tier, the evidence source, and its limitations:

- **L0 exact**: a row claims L0 only when target-version traces pass and no gap is known. "Not covered" lists aspects that no trace exercises.
- **L2**: every other row claims L2 and lists its gaps, including behavior not yet compared with the target version.

The tiers are defined in the architecture: L0 exact, L1 region-equivalent, L2 declared deviation.

| Mechanism | Claimed | Evidence |
| --- | --- | --- |
| `command_timing` | L0 | lamp_timing, vertical_slice, comparator_signal |
| `neighbor_updates` | L0 | vertical_slice, chain_limit, piston_observed, lever_attachment, dust_steps, support |
| `neighbor_chain_limit` | L0 | chain_limit |
| `scheduled_ticks` | L0 | tick_priority, comparator_priority, diode_priority, observer_clock |
| `redstone_wire` | L0 | vertical_slice, chain_limit, strong_power, weak_power, dust_shape, dust_steps |
| `repeater` | L0 | vertical_slice, observer_pulse, tick_priority, diode_priority, strong_power, repeater_lock |
| `comparator` | L0 | comparator_signal, comparator_priority, diode_priority, double_chest, weak_power |
| `support` | L0 | support, lever_attachment |
| `redstone_lamp` | L0 | lamp_timing, vertical_slice, tick_priority |
| `observer` | L0 | observer_pulse, observer_clock, observer_moved, piston_observed |
| `block_events` | L0 | piston_observed, piston_slime, piston_timing |
| `double_chest` | L0 | double_chest, double_chest_left_input |
| `block_entity_order` | L0 | piston_slime, piston_observed, hopper_competition |
| `hopper` | L0 | hopper_chain, hopper_competition, double_chest, hopper_chained |
| `water` | L0 | water_channel, water_drop, water_washes |
| `piston` | L2 | Harness traces pass. Gaps are listed in the table: drop offsets and item pushing. |
| `random_ticks`, `wheat`, `item_entity`, `chunk_tickets`, `entity_order` | L2 | Documented behavior only. These are not compared with the target version; circuit-trace v1 has no entity observations. |
| `container_menus` | L2 | A 48-step client scenario gives the same windows as the official servers of both versions. Crafting is not modeled, and no harness traces of menus exist. |
| `survival_actions` | L2 | Documented hardness, tools and pickup rules. Thrown items have no random spread, death drops spread in a fixed pattern, and loot covers only the modeled blocks. |

An L0 claim rests on the scenarios its fixtures exercise. It is not a proof for untested configurations.

## Code layout

| Path | Contents |
| --- | --- |
| `include/circuit/contracts/` | Coordinates and identities, generations, logical time and order keys, stable error codes, commands, trace records, version bundles and the compatibility table, replay records |
| `include/circuit/gameplay/`, `src/gameplay/` | Block registry, `RuleContext`, the update queue, and the reference rules |
| `include/circuit/runtime/`, `src/runtime/` | `Engine`, region contexts, job runners, tick clock |
| `include/circuit/evidence/`, `src/evidence/` | circuit-trace fixtures, content mapping, recording and evaluation |
| `tests/oracle/` | Separate sequential executor using shared gameplay rules, never linked into the server |
| `tests/rules/` | Rule fixtures with hand-derived expectations |
| `tests/sim/` | Differential scenarios: runtime against oracle |
| `tests/fixtures/` | Skeletons, reference recordings, target-version traces, documented fixtures and profiling workloads |
| `tests/control/`, `tests/contracts/`, `tests/evidence/` | Runner, clock, digest, contract and fixture tests |

Dependencies point one way: `contracts` ← `gameplay` ← `runtime` ← `app`. Rules see the world only through `RuleContext`, so the oracle and the runtime execute the same rule functions over different storage and ordering machinery.

## Tick structure

Logical time advances by one per executed tick. Each tick runs these phases in order (the target version's order). Within a phase, work is a sequence of *roots* in a fixed serial order. Immediate reactions (neighbor and shape updates) finish inside their root.

| Phase | Roots in serial order |
| --- | --- |
| commands | admitted commands by admission id, then one root applying chunk-level changes |
| block ticks | scheduled block ticks due now in ticking chunks, by (due, priority, sub-order), at most 65,536 |
| fluid ticks | scheduled fluid ticks, same order and limit |
| random ticks | ticking chunks in coordinate order, then sections bottom to top |
| block events | the level-wide FIFO, including events queued while it drains; events in non-ticking chunks stay queued in order |
| entities | entities in ticking chunks, ascending id, at most once per tick |
| block entities | block entities in ticking chunks, ascending creation order |

Commands are external inputs applied between ticks, so they observe the previous tick's time. A delay that a command starts counts from that time.

Commands apply serially in admission order, each with its immediate reactions, so a command sees everything the earlier commands of the tick changed. Player actions use this: `PlayerUse` (a click on a block face) and `PlayerBreak` decide what to use, where to place and whether the block is supported when they are applied, not when they are admitted. A player's actions therefore give the same world however they are grouped into ticks. The server checks only reach, player overlap and timing at admission; the held item is read from the inventory when the command is applied.

Player inventories are simulation state, changed only by commands in the command phase:

| Command | Effect |
| --- | --- |
| `SetPlayerSlot` | Sets one slot (creative inventory) |
| `PlayerUse` | Uses a block or places the held item; in survival the item is used up (a water bucket leaves a bucket) |
| `PlayerBreak` | Breaks a block; in survival it drops what the tool harvests and wears the tool |
| `PickUp` | Moves an item entity, or part of it, into the inventory |
| `DropItem` | Throws one item or the stack from a slot |
| `ContainerClick` | One click in the player's own window or an open container's window |
| `CloseContainer` | Returns the carried stack and the crafting grid to the inventory, or throws them |
| `PlayerDeath` | Drops the whole inventory, the crafting grid and the carried stack around the player; below the world the items are lost |

Every slot change records `player_slot_changed`; a use that opens a container records `container_opened`.

Every observable action produces a trace record keyed by (tick, phase, root, sequence within the root). Two executions are equivalent only if their trace records are identical, including order. Their canonical state dumps must also be identical.

## Reference rules v7

- **Update queue:** neighbor updates and shape updates are queued entries.
  - **Entries:**
    - A neighbor-update entry notifies six neighbors in the order west, east, down, up, north, south, optionally skipping one.
    - A single entry notifies one block.
    - A shape entry updates one neighbor; a change sends shape updates west, east, north, south, down, up.
  - **Order:** an entry emitted while another runs joins the current layer. After each delivery, the layer runs in emission order before the next sibling delivery. An entry emitted outside a chain starts one.
  - **Chain limit:** a chain counts the entries queued in it. Once the count reaches the limit (1,000,000), new entries are dropped and the drop is recorded; entries already queued still run.
  - **Shape nesting:** shape-update nesting is limited to 512.
- **Changes:** a full change runs the removal reactions of the old block and the placement reactions of the new one, then neighbor updates, then shape updates. Quiet changes (state-only changes of repeaters, observers, lamps, comparators, dust, chest pairing and hoppers) skip the neighbor updates. Every state change of a diode updates the block in front of it, and that block's neighbors except the diode itself, before the shape updates.
- **Power:**
  - **Sources:** levers, redstone blocks, repeaters, comparators, observers and dust give power to their neighbors. A lever strongly powers the block it hangs on, a repeater or comparator the block in front, an observer the block at its back.
  - **Conductors:** stone, obsidian and lamps. A conductor that a source powers strongly powers dust and components next to it. Dust powers the block below it and the blocks it points into; a conductor powered only by dust powers components, not dust.
- **Dust:**
  - **Power:** the strongest power its neighbors give it (dust giving nothing), or one less than the strongest dust it reaches on its level, one step up (unless a conductor lies above it) or one step down (unless the block at the side is a conductor). A power change updates the neighbors of the dust and of its six neighbors, in Java hash-set order.
  - **Shape:** stored as the target version stores it: each side none, side or up. Shape updates change it; a dot (no sides) stays a dot while nothing connects; a lone connection extends to the opposite side; a player places a cross. Dust points into the sides its current shape connects, and changes of shape update diagonal dust.
  - **Placement and removal:** dust updates the blocks above and below it and the dust around it, also one step up or down.
  - **Use:** using an isolated cross makes a dot and the other way round.
- **Components:**
  - repeaters: delay 1–4, pulse extension; locked, with the `locked` state, while a powered diode faces into either side;
  - lamps: light at once when an update finds them powered, turn off after 4 ticks;
  - observers: fire 2 ticks after a shape update from the observed side, or after being moved by a piston, with a 2-tick pulse.
- **Support:** dust needs a whole top face or a hopper below it, repeaters and comparators a whole top face, levers a whole face to hang on. An update that finds the support gone breaks them with their drops.
- **Levers:** a toggle updates around the lever and around the block it hangs on; so does removing a powered lever.
- **Scheduled ticks:**
  - **Uniqueness:** at most one pending tick per (position, block). A drained tick counts as scheduled until it runs.
  - **Sub-order:** the level-wide counter is shared by block and fluid ticks.
  - **Priorities:**
    - repeaters: −3 when feeding a diode that does not face back into them, −2 when turning off or ending an extended pulse, −1 otherwise;
    - comparators: −1 when feeding a diode, 0 otherwise;
    - lamps and observers: 0.
- **Pistons:** regular, sticky and slime structures of up to 12 blocks, with quasi-connectivity. Each move takes two ticks.
  - **Extension write order:** a block event writes, in this order:
    1. destroyed blocks, in reverse discovery order;
    2. moving pistons at the destinations, last discovered first;
    3. the head;
    4. vacated cells, in hash order;
    5. the extended base.

    Every write sends shape updates at once. Neighbor updates follow when the action ends.
  - **Retraction write order:**
    1. a still-extending head is cleared;
    2. the base becomes a moving piston;
    3. the head becomes air;
    4. a sticky piston drops its block or pulls.
  - **Moving blocks:** moving pistons finish in the block-entity phase two ticks after the event. They block movement and do not conduct power. A sticky piston that retracts before its push finished leaves the block behind.
  - **Destroyed blocks:** dust, levers, repeaters, comparators, water and wheat in a push line are destroyed. They drop fixed items at the block center without velocity. Fully grown wheat drops wheat and seeds, younger wheat seeds only, and water nothing.
  - **Pushing:** on each of its two steps, a moving block pushes item entities in its cell half a block along its motion. A retracting base pushes nothing.
- **Hoppers and chests:** push before pull, an 8-tick cooldown, disabled while powered, and pickup of item entities in the cell above. Competing hoppers act in block-entity creation order. A hopper that receives an item into an empty inventory from another hopper waits 8 game ticks, or 7 when it already ticked in that tick, so the item moves on 8 ticks later either way.
- **Double chests:** two halves with the same facing pair as left and right; a left half pairs clockwise of its facing. A half claims a single chest at its partner position and becomes single when its partner disappears; pairing changes are quiet. Hoppers and comparators see one 54-slot container with the right half's slots first, even across a chunk edge.
- **Comparators:** compare and subtract modes with a 2-tick delay; the output signal (0–15) lives in a block entity.
  - **Rear input:** power, also through a conductor, or the fullness of a container behind the comparator; a container behind a conductor replaces a weaker signal.
  - **Side inputs:** dust of any shape, redstone blocks, and the strong power of repeaters, comparators and observers facing in.
  - **Updates:** container changes notify the comparators reading them. A mode change refreshes the output at once.
- **Use:** using a block toggles a lever, cycles a repeater's delay, toggles a comparator's mode, or toggles isolated dust between a cross and a dot.
- **Water:** sources, flowing distance 1–7, falling water, 5-tick fluid ticks.
  - **Spread:** down first; otherwise to the sides in the order north, south, west, east, but only toward the nearest drops within five blocks when there are any. Flowing water above water stops; sources keep spreading.
  - **New sources:** a cell between two sources becomes a source when it stands on solid ground or a source.
  - **Washing away:** water replaces dust, levers, repeaters, comparators and wheat, which drop their items.
- **Wheat:** grows on random ticks. Random-tick positions and rule draws use a stream derived from (seed, tick, chunk, section) instead of a level-wide stream.
- **Item entities:** fixed-point gravity, drag and collision, migration between chunks, despawn after 6000 ticks. They are spawned by commands and by destroyed blocks, and pushed by moving blocks.
- **Items:** every item of both versions, 1,415 in all, with its stack size and durability, compiled from PrismarineJS minecraft-data (MIT) at the pinned commit by `tools/compile_item_registry.py`. Items without rules are inert: they can be stored, moved, thrown and picked up. Stacks keep their data components (see [compatibility](COMPATIBILITY.md#content)); stacks merge only when their item, damage and components are equal. A `max_stack_size` component sets the stack's limit, and `max_damage` its durability. Container slots hold up to 99 items, the stack's own limit (usually 64 or less) applying first; a comparator reads a container's fullness against those limits.
- **Other blocks:** every block state of both versions exists, from the block registry `circuit-blocks-1` (compiled from PrismarineJS minecraft-data, MIT, at the pinned commit by `tools/compile_block_registry.py`). Blocks without rules of their own keep all their properties and are inert: solid for item entities when they have a collision shape, conductors when opaque, sturdy on faces their collision shape covers (every face when opaque), immovable by pistons, without drops, and water does not flow into them. Their hardness, effective tool and harvest tier come from the same data, or, when the server loads data packs, their tool and tier from the block tags (see [data packs](DATA_PACKS.md)); an engine's data is fixed for its life.
- **Player inventories:** each player (profile UUID) has the 46 slots of the player window (crafting result and grid, armor, main inventory, hotbar, off hand), a carried stack and a drag in progress.
  - **Survival use:** placing a block uses one item. A tool wears by one for each block with nonzero hardness it breaks, and breaks at its durability.
  - **Breaking:** a block drops what the tool harvests: stone drops cobblestone, and stone, obsidian, redstone blocks, observers and hoppers need a pickaxe of the required tier (obsidian a diamond one). Breaking a container drops its contents; breaking a piston head removes its base too.
  - **Dig time:** per tick, a block gains the tool's speed divided by its hardness and by 30, or by 100 when the tool does not harvest it, five times less off the ground. Pickaxes, axes, shovels and hoes have the speeds wood 2, stone 4, iron 6, diamond 8, netherite 9, gold 12; swords and shears are not modeled tools.
  - **Pickup:** an item entity whose pickup delay has run out joins the inventory: first the selected hotbar slot and the off hand when they hold the same item with room, then the first slot with room, then the first empty slot. Block drops wait 10 ticks, thrown items 40.
- **Container menus:** a chest (27 slots), a double chest (54, the right half first) or a hopper (5), followed by the main inventory and the hotbar; or the player's own window.
  - **Click modes:** pick up and place (left: the stack, right: half or one), quick move (shift), swap with a hotbar slot or the off hand, clone (creative only), throw (one or the stack), drag (split evenly, one each, or full stacks in creative) and collect (double click), with the target version's slot order for quick moves.
  - **Slot rules:** armor slots take only matching armor, one item each. The crafting grid holds items, but the rules do not craft: the result slot stays empty.
  - **Closing:** the carried stack and the crafting grid go back to the inventory, or are thrown when it is full.
- **Death drops:** in the target version's order (hotbar, main inventory, armor from the feet up, off hand), then the crafting grid and the carried stack, each with a 40-tick pickup delay. They spread in eight directions at 0.25 and then 0.125 blocks per tick, and 0.2 upwards; the target version picks each direction and speed at random.
- **Chunk tickets:** a ticket of radius r makes chunks within Chebyshev distance r ticking and loads the chunks within max(r + 1, its load radius) at border level, without ticking. A player's load radius is their view distance. Changes apply at the end of the command phase. Unloading keeps all chunk state, including pending ticks. A reload increments the chunk generation, and commands can name the generation they expect.

## Part I mechanism contracts

Every supported mechanism runs in the region-parallel runtime under the same rule.

- **Execution:** a root may touch only the columns its region owns.
- **Conflict fallback:** touching another region's column aborts the attempt. The affected regions roll back, merge, and run again in serial order. Repeated conflicts end in one region, which is serial execution through the same machinery.
- **Level-wide order:** level-wide counters are staged and assigned at commit in serial order.

The table lists what is specific to each mechanism.

| Mechanism | Read/write footprint | Ordering contract | Sequential expected traces | Differential coverage | Conservative fallback |
| --- | --- | --- | --- | --- | --- |
| Commands and tickets | Target position, plus the update reach of its change; tickets are global | Admission id; tickets after all commands | lamp_timing; rules tests | every scenario; `tickets` | Ticket changes run as one root at the end of the commands phase |
| Neighbor and shape updates | One block around each entry, transitive within one root | Per-root queue, depth-first in emission order; chain limit counts entries | vertical_slice, chain_limit, piston_observed; rules tests | `vertical_slice`, `observer_clocks`, `observed_pistons` | A delivery into another region aborts and merges; truncation is a recorded event |
| Scheduled ticks | Ticked block plus its update reach | (due, priority, sub-order); sub-orders staged per (root, sequence) | tick_priority, comparator_priority, diode_priority, observer_clock | `vertical_slice`, `observer_clocks`, `observed_pistons` | Staged sub-order; at most 65,536 per list and tick |
| Redstone components | Dust: its 7-position set and their neighbors. Diodes: front block and its neighbors. Comparators: up to two blocks behind and both sides | Through updates and scheduled ticks | vertical_slice, observer_pulse, observer_clock, observer_moved, comparator_signal | `vertical_slice`, `observer_clocks`, `comparators_and_double_chests`, `observed_pistons` | Merge on cross-region reach |
| Block events and pistons | Up to 12 moved blocks, head, slime branches, destroyed blocks, plus one block of update reach | Level-wide FIFO rebuilt at commit; moves finish in block-entity creation order | piston_timing, piston_slime, piston_destroys, piston_observed, quasi_connectivity | `pistons`, `piston_chains`, `piston_pulses`, `drops_and_pushes`, `observed_pistons` | Structures reaching another region merge; FIFO positions and creation order staged |
| Hoppers and (double) chests | Hopper, its source and target containers, the cell above; both chest halves, possibly across a chunk edge | Block-entity creation order; each transfer is atomic within one root | hopper_chain, hopper_competition, double_chest, double_chest_left_input | `hoppers_and_items`, `comparators_and_double_chests` | A transfer that reaches another region merges first; nothing is published partially |
| Water | Water block, its four sides and below | Fluid ticks | water_channel | `water_corner` | Merge on cross-region reach |
| Random ticks and wheat | One section | Chunk coordinate order, sections bottom to top; per-section derived stream | Documented behavior; rules tests | `wheat_observers`, injected aborts | A retried attempt redraws the same values; no shared stream |
| Item entities | Entity cell and its collision neighbors; migration between columns | Ascending id; ids of spawned entities staged at commit | Documented behavior; rules tests | `drops_and_pushes`, `hoppers_and_items` | Crossing into another region merges; provisional ids are rewritten at commit |
| Player inventories and menus | The player's inventory, the open container's halves, item entities thrown or picked up | Admission order; the command phase runs serially | Rules tests (every click mode, random clicks conserving items); server tests; a 48-step window scenario compared with the official 1.21.8 and 1.21.4 servers | Command phase only | Not parallel: the command phase runs on the coordinator |

### Coverage of the Part I cases

| Case | Covered by |
| --- | --- |
| Pulses | Observer pulses and clocks, repeater pulse extension, short piston pulses (`observer_pulse`, `observer_clock`, `diode_priority`, `piston_pulses`) |
| Delayed events | Scheduled ticks, block events, two-tick moving pistons (`tick_priority`, `piston_timing`, `piston_chains`) |
| Atomic inventory operations | Hopper transfers, competition for the last free space, double chests across a chunk edge; item conservation is checked (`hopper_competition`, rules tests, `comparators_and_double_chests`) |
| Multi-block effects | Slime structures, block dropping, destroyed blocks, double chest pairing (`piston_slime`, `piston_destroys`, `double_chest`) |
| Migration | Item entities crossing chunk edges in parallel phases (`drops_and_pushes`, rules tests) |
| Stale reads | Generation-checked commands and `WorldView` queries that report `stale_view` after the engine advances (rules and simulation tests) |
| RNG retry | Per-section derived streams under injected aborts (`wheat_observers`, injected-abort runs) |
| Global resources | Staged sub-orders, block-event positions, block-entity creation order and entity ids, consumed through a creation ledger even for objects removed before commit (`transient_heads`); ticket changes at the phase boundary |
| In-flight work | Worker failure, atomic publication and future events in `runtime_tests`; a failed tick is not published and leaves the engine unusable |

### Coverage limits

- **Not modeled:** dimensions other than one overworld, lighting, world generation (chunks start empty), mobs and mob spawning, crafting, and analog comparator sources other than containers.
- **Not compared with the target version:** random-tick outcomes, wheat growth, item-entity physics and entity order. The circuit-trace format has no entity observations yet.
- **L2 behavior:** every row claimed L2 is Circuit's own definition. The gaps listed in the compatibility table apply.

## Region-parallel runtime

**Ownership.** Every loaded chunk column belongs to exactly one region. A region task runs its roots in the phase's serial order and may touch only its own columns. A read or write of a column owned by another region throws before touching it.

**Conflicts.** The coordinator rolls back an aborted attempt using copy-on-first-write snapshots of the written columns. It then merges the two regions, discards any staged results of the merged regions, and runs the merged region again. Regions that never touched each other keep their results. Each conflict reduces the number of regions, so a phase always finishes; in the worst case everything becomes one region and runs serially.

**Level-wide counters.** These are not drawn from shared counters during a phase:
- scheduled-tick sub-orders;
- block-event queue positions;
- the creation order of block entities created during a phase (moving pistons);
- the ids of entities spawned during a phase (drops).

Each region context keeps a creation ledger per counter. Every creation adds a (root, sequence) entry, and the staged object points at its entry.

- **Commit:** the coordinator sorts the entries of all regions and assigns counter values in serial order. Then it applies the values to the objects that still exist.
- **Removed objects:** creations whose objects were removed again within the phase still consume their values, as they do in the serial order. A moving piston's head that is created and removed in the same block-event phase is one example.
- **Rollback:** an aborted attempt discards its ledger along with its writes.

Entities spawned in parallel roots hold provisional ids until commit. Commit rewrites them in storage, the entity index and the trace, including the ids of entities that were removed again.

**Block-event order.** Each region drains its own FIFO. At commit, the global FIFO order is rebuilt from the initial queue and the children each event appended. Trace records are renumbered to their global positions.

**Why layout cannot change results.** Within a phase, regions that finished without conflict accessed disjoint columns. Their roots therefore commute with each other's roots and see exactly the column states they would see in the serial order. Region layout decides only what runs concurrently.

**Layouts.** `adaptive` merges on conflict and, every 20 ticks, splits regions whose columns have not interacted for 40 ticks. `single_region` runs everything through one region. `perturbed` applies seeded random merges and splits for testing.

**Threads.** `PoolRunner` runs each region task as one job on the Asio pool; workers never wait for other jobs. `SeededRunner` interleaves task steps deterministically on one thread from a seed, optionally injecting aborts, so any failure replays exactly.

## Verification

- `rules_tests` checks hand-derived expectations for every mechanism, including:
  - chunk-edge crossings, and a chain returning across an edge within one tick;
  - pulse extension, priority order and diode priorities;
  - chain truncation;
  - piston limits, and observers watching and being moved by pistons;
  - conservation in hopper races, water timing and stream locality;
  - generation checks, command validation and capacity failure;
  - ticket and spawn bounds at their endpoints and at extreme or aliasing coordinates.
- `simulation_tests` compares fourteen scenarios with the oracle, trace by trace every tick and state by state at checkpoints.
  - **Configurations:** 1/2/4/8/16 pool workers, a single region, seeded random and priority interleavings, injected aborts, and perturbed layouts.
  - **Transient objects:** the `transient_heads` scenario and a hand-derived case check that block entities created and removed within one phase still consume their creation order.
  - **Bounded exploration:** 64 seeds of random placements of redstone components, pistons, slime, hoppers, chests and stone around a chunk corner, for 60 ticks each, under seeded interleavings and perturbed layouts. Every tick is compared with the oracle.
  - **Input rejection:** out-of-range spawns and tickets are rejected on both executors without effect.
  - **Negative check:** a deliberately unsafe mode without ownership checks must produce a detected divergence.
- `control_tests` checks seeded-schedule reproducibility, tick-clock catch-up and the state digest.
- `evidence_tests` covers:
  - the [trace fixture format](TRACE_FORMAT.md) and the content mapping;
  - recording and evaluation on both the engine and the oracle;
  - the committed reference recordings;
  - every target-version trace in `tests/fixtures/harness/`, on a 4-worker engine and on the oracle.
- `contract_tests` checks the compatibility table:
  - every row claims a tier;
  - L0 claims need target-version traces and list no gap;
  - L2 claims list their gaps.

GCC AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer runs of the test programs are part of milestone checks. The `ubsan` mode stops at the first diagnostic. ThreadSanitizer needs ASLR disabled for the test process (`setarch "$(uname -m)" -R`). See the [development guide](DEVELOPMENT.md).

## Workload profile

The profile measures five generated workloads (`tests/fixtures/workloads/`, from `tools/generate_workloads.py`) for 600 ticks after their setup settles. The target version was measured by the external harness, and Circuit by `--profile-fixture` on a release build.

- **Machine:** Intel Core Ultra 7 270K Plus, 24 logical CPUs, Linux 6.18 under WSL2.
- **Target version:** OpenJDK 21.0.12.
- **Circuit:** GCC 13.3.

| Workload | Target version: mean tick, largest phases | Circuit, 1 worker | Circuit, 8 workers |
| --- | --- | --- | --- |
| 256 observer clocks | 0.48 ms: block ticks 74%, random ticks 22% | 0.54 ms | 0.69 ms |
| 64 dust clocks | 3.97 ms: block ticks 97% | 2.49 ms | 2.61 ms |
| hopper lines | 0.27 ms: block entities 57%, random ticks 37% | 0.24 ms | 0.30 ms |
| piston engines | 0.75 ms: block events 43%, block ticks 25%, block entities 16%, random ticks 16% | 0.61 ms | 0.75 ms |
| water flood | 0.23 ms: fluid ticks 56%, random ticks 36% | 0.12 ms | 0.19 ms |

In these workloads the ordered phases dominate the target version's tick: block ticks, fluid ticks, block events and block entities. Their roots belong to independent machines. The claimed tiers therefore do not force serial execution: regions that do not interact may run concurrently, because commit restores the level-wide order.

Circuit does not yet gain from this. With 8 workers it is 5–55% slower than with one, because coordination, snapshots and serial commit cost more than the parallel work saves on ticks of this size.

These numbers are not a performance claim:
- the semantics differ (L2 rows, no lighting, no networking or chunk sending);
- only means were recorded;
- tail latency is not measured.

Optimization belongs to M8.

## Running

```bash
xmake run circuit-server --simulate --ticks 300 --workers 4 --record "$PWD/demo.crpl"
xmake run circuit-server --simulate --replay "$PWD/demo.crpl" --workers 16
xmake run circuit-server --simulate --ticks 200 --realtime
xmake run circuit-server --compatibility
xmake run circuit-server --profile-fixture "$PWD/tests/fixtures/workloads/observer_clocks.ctrace" --workers 8
```

`--simulate` without `--replay` runs a built-in demonstration with machines that straddle chunk edges.

- **Replay records:** records (format version 5) store the bundle, the rule-set id and the block registry id, whose encoding their block states use; item stacks carry their data components, which the simulator checks against the bundle's protocol. A record made under different reference rules or another block registry is refused with `rules_mismatch` instead of replaying with silently different behavior. Version 4 records have stacks without components; version 3 records also predate the registry and are read as `circuit-blocks-1`.
- **Digest:** the output includes a 64-bit state digest. Equal digests across worker counts are a quick check, not proof.
- **`--realtime`:** paces ticks at 20 Hz. When it falls more than 40 ticks behind, it drops the wall-clock backlog; logical ticks are never skipped.

Fixtures in [circuit-trace format](TRACE_FORMAT.md) can be checked with `--check-fixture <file>`, or recorded with `--record-fixture <skeleton> --output <file>`.

## Limitations

- Rows claimed L2 differ from the target version as listed in the compatibility table. The main gaps are:
  - piston drops without random offsets and simplified item pushing;
  - simplified item physics without merging, and entity order without target-version trace observations;
  - per-section random streams and coordinate-ordered random-tick chunks;
  - wheat ignoring light and hydration;
  - chunk tickets without vanilla ticket types or level propagation;
  - container menus without crafting and survival loot limited to modeled blocks.
- Blocks without rules of their own, among them TNT, honey blocks, rails and coral fans, are inert: solid when they have a collision shape, conductors when opaque, immovable by pistons and without drops. Primed TNT entities and explosions are not modeled. TNT duplication machines cannot run; piston trace coverage does not establish compatibility for them. See [technical machines](COMPATIBILITY.md#technical-machines).
- There is no lighting, world generation, mob spawning or second dimension.
- Snapshots copy whole column data on first write, and commit is serial. On the measured workloads, coordination overhead outweighs parallel gains.
- Commands validate their inputs at full width before converting them; for example, a spawn position is checked against the world border and height in fixed-point units before it is narrowed to a block. Out-of-range input is rejected with no effect.
- A capacity failure, such as exceeding 1,000,000 block events in one tick, fails the tick and leaves the engine unusable. Nothing is silently dropped. The neighbor-update chain limit is the exception: it drops updates as the target version does, and records it.
