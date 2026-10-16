# Headless simulation core

## Status

The Part I core runs headless: shared contracts, a set of reference rules, a sequential scheduling oracle, and a region-parallel runtime checked against the oracle. The oracle shares gameplay rules with the runtime; it checks scheduling equivalence, while external reference observations and hand-derived expectations check the rules themselves. The [network server](SERVER.md) and [persistence](PERSISTENCE.md) are built on it.

The rules are **circuit-reference-rules-v10**. They are Circuit's own definitions, written from public documentation and from traces recorded by an instrumented server outside this repository.

Two version bundles run them: `java-1.21.8` and `java-1.21.4`. 1.21.8 has 72 traces and 1.21.4 has 71 recorded on the unmodified server (1.21.4 lacks the tntExplodes game rule, so `tnt_explodes_disabled` is 1.21.8's only):
- 1.21.8 traces are in `tests/fixtures/harness/`;
- 1.21.4 traces are in `tests/fixtures/harness-1.21.4/`.

The 1.21.4 traces equal the 1.21.8 traces apart from their headers. All 143 pass on both the parallel runtime and the oracle.

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
| `light` | L0 | light_spread, sky_light, light_shapes, light_timing; every state's emission, dampening and occlusion shape (tests/fixtures/light) |
| `game_rules` | L2 | 45 `/gamerule` commands give the official replies on both versions. The other rules change nothing yet; compared through a protocol client, not traces. |
| `day_time` | L2 | 59 `/time` commands give the official replies and `set_time` packets on both versions. Nothing reads the time of day yet; compared through a protocol client, not traces. |
| `weather` | L2 | 16 `/weather` commands give 1,348 identical replies and events on both versions. Durations come from Circuit's own stream; no lightning, snow or other effects of rain. |
| `piston` | L2 | Harness traces pass, including the level-random draws of piston sounds (piston_draws). Gap listed in the table: item pushing. |
| `chunk_tickets` | L2 | ring_blocks, unload_reload. New chunks load at once and dropped chunks unload at once, where the official servers take a varying number of ticks. |
| `item_entity`, `entity_order` | L2 | item_hopper_pickup, item_water_current, item_merge, item_merge_moving, item_merge_crossing, item_merge_vertical, item_merge_timers, item_hopper_area, item_hopper_inside, item_chest_scatter, item_water_still, item_water_channel, item_water_ice_corner, item_water_falling, item_water_fall, item_chunk_edge: positions, velocities, merges, ages, pickup delays, collection by hoppers, a removed chest's scatter, currents and floating, leaving and coming back with a chunk, and the order of the items match both official servers exactly. Moving blocks push items differently (item_piston_push, item_drop_piston). |
| `falling_block` | L2 | falling_drop, falling_column, falling_onto, falling_void: when sand, gravel and concrete powder fall, how they move, where they land, the items they break into and the concrete powder makes in water match both official servers exactly. Not modeled: concrete powder beside water, anvils, pointed dripstone, scaffolding, suspicious blocks, damage. |
| `tnt` | L2 | tnt_ignition, tnt_motion, tnt_water_current, tnt_reload, tnt_explodes_disabled: which redstone primes TNT and when, the launch and every position, velocity and fuse of primed TNT on the ground, in the air, in currents, across a chunk edge and across an unload and reload match both official servers exactly. tnt_redstone_ignition, tnt_obsidian, tnt_drops, tnt_void, tnt_entity_order, tnt_knockback, tnt_items, tnt_chain_edge, tnt_water, tnt_power: explosions break the same blocks in the same order, drop the same merged stacks, prime the same TNT with the same fuses, and destroy, hurt and push the same entities by the same amounts, in the same order, with the same draws of the level random. Fire, players, projectiles and dispensers do not ignite TNT yet, and explosions do not hurt players. |
| `random_ticks`, `wheat` | L2 | Documented behavior only; not compared with the target version. |
| `container_menus` | L2 | A 48-step client scenario gives the same windows as the official servers of both versions, and so does a 48-step crafting scenario in the own window (with data packs). The crafting table, special recipes and the recipe book are not modeled, and no harness traces of menus exist. |
| `survival_actions` | L2 | Documented hardness, tools and pickup rules; block loot tables and 2x2 crafting from data packs, compared with the official servers. Thrown items have no random spread, death drops spread in a fixed pattern, and loot tables with random parts drop nothing. |

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

**Levels.** A world has levels: the overworld, the Nether and the End.
- **Own state:** each level keeps its own columns, scheduled ticks, block events, block entities and entities. Positions mean nothing outside their level, and rules never reach into another level.
- **Commands and tickets:** every command, and every ticket, names its level.
- **Order:** the command phase runs once, for all levels. Then each level runs the phases below, the overworld first, then the Nether, then the End (minecraft.wiki "Tick"): its scheduled ticks where columns are loaded, its chunk stage, and the rest where columns are loaded after it.
- **Counters:** tick sub-orders, block-event positions and block-entity creation order count per level; entity ids count over all levels.
- **Random streams:** random ticks in another level draw from streams that include the level. Each level also has its own level random (below).
- **Not yet reachable:** only the overworld is generated and reachable by players. The other levels exist for the engine, its tests and the formats; they have no rules of their own yet (C7).

| Phase | Roots in serial order |
| --- | --- |
| commands | admitted commands by admission id |
| block ticks | scheduled block ticks due now in block-ticking chunks, by (due, priority, sub-order), at most 65,536 |
| fluid ticks | scheduled fluid ticks, same order and limit |
| chunks | the level's tickets whose lifetime is over go, then one root applies its chunk-level changes |
| random ticks | chunks whose random ticks run (below) in coordinate order, then sections bottom to top |
| block events | the level's FIFO, including events queued while it drains; events in chunks that do not block tick stay queued in order |
| entities | entities in entity-ticking chunks, ascending id, at most once per tick, while the level is awake |
| block entities | block entities in block-ticking chunks, ascending creation order, while the level is awake |

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

Every observable action produces a trace record keyed by (tick, level, phase, root, sequence within the root); a record also names the level of its position. The command phase counts as the overworld's, ahead of every level. Two executions are equivalent only if their trace records are identical, including order. Their canonical state dumps, one part per level, must also be identical.

## Reference rules v10

v10 is v9 with what C2.3 and C2.4 added to the rules: crops that need light, game rules as world state, the day time and the weather. Worlds of v9 move to v10 with `--migrate-world` ([persistence](PERSISTENCE.md#migrating-between-versions)).

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
  - **Destroyed blocks:** dust, levers, repeaters, comparators, water and wheat in a push line are destroyed. They drop fixed items, placed and launched like every block drop (below), before the block changes. Fully grown wheat drops wheat and seeds, younger wheat seeds only, and water nothing.
  - **Sounds:** after a piston extends, and after it retracts, it draws one number from the level random for its sound's pitch, as both official servers do.
  - **Pushing:** on each of its two steps, a moving block pushes item entities in its cell half a block along its motion. A retracting base pushes nothing.
- **Hoppers and chests:** push before pull, an 8-tick cooldown, disabled while powered. A hopper collects item entities whose box reaches into the cell above it or into its bowl (above 11/16), the first by id of which something fits, unless a container or a solid block is above it. An item that moves inside a hopper's cell into that space is also taken during its own tick, when the hopper is enabled and not cooling down. Competing hoppers act in block-entity creation order. A hopper that receives an item into an empty inventory from another hopper waits 8 game ticks, or 7 when it already ticked in that tick, so the item moves on 8 ticks later either way.
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
- **Falling blocks** (both official servers; minecraft.wiki "Falling Block"): sand, red sand, gravel and the sixteen concrete powders check, two ticks after they are placed or a neighbor changes, whether the cell below can be replaced (air, water, lava, fire, grasses, ferns, vines, seagrass, snow and the other blocks both official servers report as replaceable; the cell below the world counts as air). If so, the block becomes air and a falling block 0.98 wide and tall (single precision, as entity sizes are on both official servers) starts at the cell's bottom center. Each tick it counts its time, gains 0.04 downwards, moves against the blocks (as items do, without currents), and loses 2% of its speed. When it lands, or when concrete powder reaches water, it becomes its block where its bottom center is if that cell can be replaced (snow only with one layer) and the cell below cannot, and concrete powder in water then turns into concrete; otherwise it breaks into its item, launched as every new item is, collectable after 10 ticks. One that has fallen 100 ticks outside the world's height, or 600 ticks anywhere, breaks too; nothing else removes it below the world.
- **TNT** (both official servers; minecraft.wiki "TNT"): a TNT block that has a redstone signal from any neighbor (the test lamps and pistons use) when it is placed, moved in by a piston, or updated becomes primed TNT: the entity first, at the cell's bottom center, launched 0.2 up (single precision) and 0.02 sideways in a direction drawn from the level random, with a fuse of 80 ticks; then the block becomes air. Pistons move TNT. With the 1.21.8 game rule tntExplodes off nothing primes TNT; 1.21.4, which lacks the rule, always does. Each tick primed TNT falls by 0.04, moves its 0.98-block box as items do, loses 2% of its speed, and on the ground loses 30% more of its horizontal speed (on any block) and bounces back up at half its downward speed. Its fuse goes down; at zero it is removed and explodes with power 4, a sixteenth of its height (0.98 in single precision) above its feet; otherwise currents push it once, after the move, as they push items. Water neither slows nor lifts it, and nothing removes it below the world.
- **Explosions** (both official servers; minecraft.wiki "Explosion"): rays leave the center towards the 1,352 points on the surface of a 16x16x16 grid. Each starts with the power times 0.7 to 1.3 (one single-precision draw of the level random) and steps 0.3 blocks (in single precision) at a time: a cell holding a block or fluid takes (blast resistance + 0.3) x 0.3 from the intensity (waterlogged blocks resist as water, 100), a cell reached with intensity left joins the explosion (air too), and each step takes 0.22500001 more; a ray stops at the world's height limits. Blast resistance comes from the block registry (minecraft-data, equal to both official servers' report for every block). Before anything breaks, entities whose box meets the explosion's reach (twice the power, plus one, in whole blocks) and whose feet are within twice the power are hurt and pushed: the exposure is the share of a grid of points over the entity's box (about 2 x size + 1 per axis, offset on the horizontal axes as minecraft.wiki describes) that the center sees past block collision shapes; the impact is (1 - distance / (2 x power)) x exposure; the push is the impact along the direction from the center to the entity's eyes (to primed TNT's feet); items lose (impact² + impact) / 2 x 7 x 2 x power + 1 of their 5 health, kept in whole points, and are destroyed at none (nether stars are immune). Entities are visited section by section (by chunk x, then chunk z and section y with negative ones after positive ones). Then the cells, in the order a Java hash set of them iterates, are shuffled by the level random and cleared in turn: TNT becomes air and primed TNT with a fuse of 10 to 29 ticks (the launch draw first); another block's drops (without a tool, all of them) join earlier drops of their kind in stacks of at most 16, and the block becomes air with its usual updates (a container scatters its contents). Once every cell is cleared, each merged stack falls as a block drop at the first cell it came from.
- **Item health:** items have 5 health, saved with them; only explosions take it away.
- **Level random:** each level has a random sequence that behaves like `java.util.Random` (the 48-bit generator and the methods its API documentation specifies), as the target versions' level random does. The rules draw from it where both official servers do, in the same order: block drops, the scatter of removed containers and piston sounds. Its state is world state: saved, replayed and part of the digest. A new world seeds it from the world seed and the level; fixtures seed it with 0, as the reference harness does, and compare the number of draws per stage with the official recording. Under the region-parallel engine, a draw needs the level's random sequence, which serializes drawing roots of one level like any other shared state.
- **Item entities:** positions and velocities are doubles; each tick, as both official servers do it (checked against their traces):
  - the tick count goes up (it is never reset), and the pickup delay down unless it is 32767 (never);
  - water the item's box reaches (less 0.001 on each side) pushes it, before the move and again after it, by 0.014 along the mean direction of the currents of those cells; each cell's current points toward neighbors holding less water (a cell holds level/9 of a block, a full block under more water) and toward water below an open neighbor, and falling water beside a whole face also flows down; cells reaching less than 0.4 above the item's bottom weigh by that depth (minecraft.wiki "Water"; fitted to the official traces);
  - an item more than 0.1 deep loses 1% of its horizontal speed and rises by 0.0005 a tick while slower than 0.06 (single-precision factors); otherwise gravity takes 0.04 from the vertical velocity;
  - the item moves its 0.25-block box against the collision boxes of every block state (registry data, per version), vertical axis first, then the horizontal axis with the larger movement; an item on the ground with a horizontal speed whose square is at most 1e-5 moves only when its tick count plus its id is a multiple of 4; a move that a collision shortened to a squared length below 1e-7 is not made;
  - collided components stop (a horizontal one only when the collision shortened it by 1e-5 or more); the velocity is damped by 0.98 (horizontally, on the ground, by the slipperiness of the block below times 0.98, in single precision) and an item that lands bounces at half its speed;
  - when its tick count is a multiple of 40, or even after a move into another block, the item merges with items whose boxes come within half a block of its own sideways (none above or below), in id order: the stacks must be of the same kind and fit in one stack; it takes in a smaller stack and joins an equal or larger one, and the result keeps the younger age and the longer pickup delay. Items of age -32768 or pickup delay 32767 never merge;
  - the age goes up unless it is -32768 (never); at 6000 the item despawns;
  - unloaded cells are solid and cells above or below the world empty; an item more than 64 blocks below the world is removed.
  Items move between chunks with their entities' chunk. They are spawned by commands, by broken and destroyed blocks, by players and on death, and pushed by moving blocks.
- **Block drops:** each item a block drops appears within 0.25 blocks of the block's center on each axis, lowered by 0.125, and moves 0.2 up and up to 0.1 to each side, all drawn from the level random (x, y, z, then the two horizontal speeds), as both official servers do. A removed container scatters its contents as both official servers do (exact in their traces): each slot, empty or not, draws a point 0.125 to 0.875 across and 0 to 0.75 up in the cell; its stack leaves in parts of 10 to 30 items, each drawing a block drop's velocity and then a velocity around (0, 0.2, 0) that differs from it by up to about 0.115 on each axis (the difference of two draws times a fitted spread). These parts can be picked up at once.
- **Items:** every item of both versions, 1,415 in all, with its stack size and durability, compiled from PrismarineJS minecraft-data (MIT) at the pinned commit by `tools/compile_item_registry.py`. Items without rules are inert: they can be stored, moved, thrown and picked up. Stacks keep their data components (see [compatibility](COMPATIBILITY.md#content)); stacks merge only when their item, damage and components are equal. A `max_stack_size` component sets the stack's limit, and `max_damage` its durability. Container slots hold up to 99 items, the stack's own limit (usually 64 or less) applying first; a comparator reads a container's fullness against those limits.
- **Other blocks:** every block state of both versions exists, from the block registry `circuit-blocks-1` (compiled from PrismarineJS minecraft-data, MIT, at the pinned commit by `tools/compile_block_registry.py`). Blocks without rules of their own keep all their properties and are inert (sand, gravel and concrete powder fall, and TNT is primed, above): entities collide with their collision boxes, conductors when opaque, sturdy on faces their collision shape covers (every face when opaque), immovable by pistons, and water does not flow into them. Their hardness, effective tool and harvest tier come from the same data, or, when the server loads data packs, their tool and tier from the block tags (see [data packs](DATA_PACKS.md)). With data packs they drop what their loot tables give, when a table draws no random numbers; otherwise they drop nothing. An engine's data is fixed for its life.
- **Player inventories:** each player (profile UUID) has the 46 slots of the player window (crafting result and grid, armor, main inventory, hotbar, off hand), a carried stack and a drag in progress.
  - **Survival use:** placing a block uses one item. A tool wears by one for each block with nonzero hardness it breaks, and breaks at its durability.
  - **Breaking:** a block drops what the tool harvests: stone drops cobblestone, and stone, obsidian, redstone blocks, observers and hoppers need a pickaxe of the required tier (obsidian a diamond one). Breaking a container drops its contents; breaking a piston head removes its base too.
  - **Loot tables:** a block without rules of its own drops from its loot table (`gameplay/loot.hpp`), when the tool harvests it. Pools roll once; entries are items, alternatives, groups, sequences or empty; conditions test survival of an explosion (always, for a player's break), block state properties, the tool's item and enchantment levels, and their combinations; functions set and limit counts. A roll with more than one candidate, or a fortune bonus at a level above zero, would need a random number and drops nothing. Stacks above the item's limit split. The drops are placed like every block drop.
  - **Dig time:** per tick, a block gains the tool's speed divided by its hardness and by 30, or by 100 when the tool does not harvest it, five times less off the ground. Pickaxes, axes, shovels and hoes have the speeds wood 2, stone 4, iron 6, diamond 8, netherite 9, gold 12; swords and shears are not modeled tools.
  - **Pickup:** an item entity whose pickup delay has run out joins the inventory: first the selected hotbar slot and the off hand when they hold the same item with room, then the first slot with room, then the first empty slot. Block drops wait 10 ticks, thrown items 40.
- **Container menus:** a chest (27 slots), a double chest (54, the right half first) or a hopper (5), followed by the main inventory and the hotbar; or the player's own window.
  - **Click modes:** pick up and place (left: the stack, right: half or one), quick move (shift), swap with a hotbar slot or the off hand, clone (creative only), throw (one or the stack), drag (split evenly, one each, or full stacks in creative) and collect (double click), with the target version's slot order for quick moves.
  - **Slot rules:** armor slots take only matching armor, one item each. Nothing can be placed in the crafting result.
  - **Crafting:** with data packs, the result slot shows what the first matching recipe (in id order) makes from the 2x2 grid (`gameplay/recipes.hpp`):
    - shaped recipes anywhere in the grid, also mirrored;
    - shapeless ones in any order;
    - transmute recipes, whose result keeps the input's components unless the input already is the result item.
    Taking the result crafts once: onto the cursor (left or right click, when all of it fits), into an empty hotbar slot or the off hand (number keys), thrown (`Q`, or `Ctrl+Q` again while the result stays the same item), or shift-clicked into the inventory from the end of the hotbar until the result changes item or nothing moves (what does not fit is thrown). Each crafting uses one item of every grid slot; buckets of milk, water and lava leave a bucket, honey bottles and dragon's breath a glass bottle, in the slot when it is empty, else in the inventory or thrown. Without data packs there are no recipes.
  - **Closing:** the carried stack and the crafting grid go back to the inventory, or are thrown when it is full; the result empties.
- **Death drops:** in the target version's order (hotbar, main inventory, armor from the feet up, off hand), then the crafting grid and the carried stack, each with a 40-tick pickup delay. They spread in eight directions at 0.25 and then 0.125 blocks per tick, and 0.2 upwards; the target version picks each direction and speed at random.
- **Chunk tickets and levels** (minecraft.wiki "Chunk", observed on both official servers):
  - **Tickets:** a ticket has a type, a level, a square radius and an optional lifetime in ticks. The chunks of its square take its level; every chunk further out takes one level more per chunk of Chebyshev distance. A chunk takes the lowest level of any ticket.
  - **Load level:** from every ticket but a player's simulation ticket. 31 and lower is entity ticking, 32 block ticking, 33 border; higher is not loaded.
  - **Simulation level:** from every ticket but a player's loading ticket, and never lower than the load level. At 32 and lower scheduled ticks, block events and block entities run; at 31 and lower entities too.
  - **Random ticks:** 1.21.8 runs them where the simulation level is 31 or lower. 1.21.4 runs them where it is 32 or lower and a non-spectator player stands less than 128 blocks horizontally from the chunk's center.
  - **Idle levels:** a level with neither players nor forced chunks stops its entities and block entities 300 ticks after it last had them, or after the server started. They resume as soon as it has them again.
  - **Server tickets:** the start ticket at the spawn chunk (level 32 − spawnChunkRadius, 2 by default); per player, a loading ticket over their view distance (level 31) and a simulation ticket at their chunk (level 31 − simulation distance). Only forced and portal tickets survive a restart.
  - **Changes** apply in the level's chunk stage, after its scheduled ticks, as on the official servers: a chunk that a command drops still runs that tick's scheduled ticks. Lifetimes end at the chunk stage they reach.
  - **Unload and reload** (both official servers): unloading keeps all chunk state. A dropped chunk's pending ticks keep the delay they had left, counted from the tick after it dropped; after a reload they wait out that delay, and the chunk's scheduled ticks and block entities run again two ticks after the reload, its pending block events at once. A reload increments the chunk generation, and commands can name the generation they expect.
  - **Entities with their chunk:** the entities of a chunk that unloads leave the level (removed as unloaded) and are kept with it; after a reload they come back in the chunk stage of the next tick, column by column in coordinate order and by their old ids, under new ids from the shared counter and with their tick counts starting again, and they tick again two ticks after the reload. The official servers load them back in the order of their chunk maps, which varies; a reloaded entity's new id changes when it moves at rest (item_chunk_edge passes; three entities in three chunks came back in different orders on the two versions).
- **Light** (minecraft.wiki "Light"; both official servers, compared with light probes):
  - **Per state:** an emission (0–15), a dampening (0, 1 or 15) and, for slabs, stairs, snow, farmland, dirt paths, pistons and a few others, an occlusion shape. The block registry holds them for every state of both versions, from minecraft-data with Circuit's own state rules (lit blocks, candles, sea pickles, waterlogged states and so on); a report of every state from both official servers matches it exactly.
  - **Spreading:** light passes to a neighbor one level lower, or lower by the neighbor's dampening when that is more, unless the faces of the two cells' occlusion shapes between them together cover the face. Block light starts at emitters; sky light starts above the world, and level 15 passes straight down undiminished into cells that dampen nothing.
  - **Timing:** light follows the blocks as they were at the end of the previous tick: a change made in one tick shows in the next. The engine keeps those blocks and brings the light up to date when it is read, following changes incrementally (columns that load and unload, cells that change); the result does not depend on when it is read.
  - **Rules that read it:** in the random-tick phase, a rule reads the most of a cell's block and sky light. Wheat grows only with light 9 or more at its own cell (minecraft.wiki "Wheat Crops").
- **Game rules** (minecraft.wiki "Game rule"; both official servers):
  - **World state:** a `SetGameRule` command sets one of the version's rules, but not an experimental one. A boolean rule takes 0 or 1, spawnChunkRadius 0–32, and every other integer rule any value, as on the official servers; other values are refused. The state holds the rules away from their defaults; it is saved in checkpoints, and the commands are logged.
  - **Read by the rules:** randomTickSpeed is the number of random positions each section of a random-ticking chunk gets per tick; none at 0 or below. The server reads spawnChunkRadius for the spawn chunks and sets rules with `/gamerule` ([server guide](SERVER.md#survival-and-inventories)). The other rules are kept but change nothing yet.
- **Day time** (minecraft.wiki "Daylight cycle"; both official servers): the world has one day time, in ticks (24,000 a day), as only the overworld has a daylight cycle. A new world starts at 0. A `SetDayTime` command sets it (never negative); after the commands of each tick it advances by one while doDaylightCycle is on, so a change in a tick shows one more at its end. It is saved in checkpoints. Nothing in the rules reads it yet.
- **Weather** (minecraft.wiki "Weather" and "Commands/weather"; both official servers, compared with a protocol client and their saved level data): one weather for the world, the overworld's.
  - **Flags and timers:** a rain flag and a thunder flag, each with a timer, and a clear timer. While the clear timer runs, both flags are off and their timers wait at 1; when it ends, both flags turn on, so a clear spell ends in a thunderstorm. Otherwise a timer counts down and toggles its flag when it reaches 0; a timer at 0 draws its flag's next duration: rain on 12,000–24,000 ticks, off 12,000–180,000; thunder on 3,600–15,600, off 12,000–180,000. Timers run only while doWeatherCycle is on.
  - **Commands:** a `SetWeather` command is `/weather`: clear for a duration sets the clear timer and turns both flags off with their timers at 0; rain or thunder sets both timers to the duration and turns rain on, and thunder only for thunder. A duration of 0 draws the kind's default (clear 12,000–180,000, rain 12,000–24,000, thunder 3,600–15,600).
  - **Levels:** after the timers, the rain and thunder levels move toward their flags by 0.01 a tick in single precision, within 0 and 1, whatever the game rule. Rain shows while the rain level is above 0.2. A start (`LevelsStarted`) sets the levels to their flags, as when the official servers load a world.
  - **Order:** in each tick, after the commands, the weather, then the day time.
  - **Randomness:** the official servers draw durations from the level's random sequence; Circuit draws them from its own stream for each tick, so durations differ while their ranges are the same. Everything else matched the official servers exactly, level values included. Lightning, snow and ice, filling cauldrons and other effects of rain are not modeled.

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
| Item entities | Entity cell and its collision neighbors; migration between columns | Ascending id; ids of spawned entities staged at commit | item_hopper_pickup, item_water_current; rules tests | `drops_and_pushes`, `hoppers_and_items` | Crossing into another region merges; provisional ids are rewritten at commit |
| Level random | The level's random sequence | The order its draws are made in | piston_draws, item_drop_piston (draws) | `drops_and_pushes`, `pistons` | Drawing needs the sequence, which belongs to one region: roots that draw in one level merge their regions, and rollback restores it |
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

- **Not modeled:** generation and rules of the Nether and the End (their levels keep separate state but are not reachable), lighting, world generation (chunks start empty), mobs and mob spawning, crafting outside the player's grid, and analog comparator sources other than containers.
- **Not compared with the target version:** random-tick outcomes and wheat growth. Item-entity physics and entity order are compared in two traces only.
- **L2 behavior:** every row claimed L2 is Circuit's own definition. The gaps listed in the compatibility table apply.

## Region-parallel runtime

**Ownership.** Every loaded chunk column belongs to exactly one region. A region's columns are all of one level. A region task runs its roots in the phase's serial order and may touch only its own columns. A read or write of a column owned by another region throws before touching it.

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

- **Replay records:** records (format version 12) store the bundle, the rule-set id and the block registry id, whose encoding their block states use. Each command names its level. Item stacks carry their data components, which the simulator checks against the bundle's protocol. A record made under different reference rules or another block registry is refused with `rules_mismatch` instead of replaying with silently different behavior. Older records are still read:
  - version 11: the same commands, without the pickup delay and age of spawned items;
  - version 10: also with positions and velocities in 1/4096-block units, read exactly as doubles;
  - version 9: also without weather;
  - version 8: also without day times;
  - version 7: also without game rules;
  - version 6: tickets are read as forced ones of level 31 over their radius;
  - version 5: also, every command is the overworld's;
  - version 4: also, stacks have no components;
  - version 3: also, the record predates the registry and is read as `circuit-blocks-1`.
- **Digest:** the output includes a 64-bit state digest. Equal digests across worker counts are a quick check, not proof.
- **`--realtime`:** paces ticks at 20 Hz. When it falls more than 40 ticks behind, it drops the wall-clock backlog; logical ticks are never skipped.

Fixtures in [circuit-trace format](TRACE_FORMAT.md) can be checked with `--check-fixture <file>`, or recorded with `--record-fixture <skeleton> --output <file>`.

## Limitations

- Rows claimed L2 differ from the target version as listed in the compatibility table. The main gaps are:
  - simplified item pushing by moving blocks;
  - items are not moved by lava, bubble columns or waterlogged blocks; entities come back with their chunks in coordinate order, not in the order of the official chunk maps; recovery after a restart continues the simulation exactly, so entities keep their ids and tick counts, where an official server loads them with new ones; entity UUIDs are not world state (the server derives them from entity ids; C11);
  - per-section random streams and coordinate-ordered random-tick chunks;
  - wheat ignoring hydration and neighboring crops;
  - chunk tickets: no teleport, portal or ender pearl tickets yet; new chunks load at once (the official servers take 2 to 4 ticks, varying between runs); dropped chunks unload at once and reloads happen in the chunk stage (the official servers may keep a dropped chunk at border for a tick or two, and restore a cached chunk during the command, which moves restored ticks by a tick between runs; in the open trace `tnt_reload_pending` a repeater's pending tick runs a tick later than Circuit's);
  - container menus without the crafting table, special recipes and the recipe book; loot tables without their random parts, and fixed drops for the modeled blocks.
- Blocks without rules of their own (but those that fall and TNT, above), among them honey blocks, rails and coral fans, are inert: solid when they have a collision shape, conductors when opaque, immovable by pistons, and drop only from deterministic loot tables (without data packs, nothing, also when an explosion breaks them). Only redstone and explosions prime TNT; explosions do not hurt or push players, and entities of one chunk section are hurt in id order rather than the order they entered it. TNT duplication machines cannot run; piston trace coverage does not establish compatibility for them. See [technical machines](COMPATIBILITY.md#technical-machines).
- Light at the edge of the loaded area is not compared with the target versions. There is no world generation or mob spawning, and players cannot reach the Nether or the End.
- Snapshots copy whole column data on first write, and commit is serial. On the measured workloads, coordination overhead outweighs parallel gains.
- Commands validate their inputs before using them; for example, a spawn position must be finite and inside the world border and the world's height, and a velocity at most 64 blocks per tick on each axis (a Circuit capacity). Out-of-range input is rejected with no effect.
- A capacity failure, such as exceeding 1,000,000 block events in one tick, fails the tick and leaves the engine unusable. Nothing is silently dropped. The neighbor-update chain limit is the exception: it drops updates as the target version does, and records it.
