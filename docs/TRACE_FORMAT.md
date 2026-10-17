# circuit-trace fixture format (v1 and v2)

## Purpose

A circuit-trace fixture describes one small scenario and what was observed when it ran. The same file format serves three roles:

| Role | `source` | Produced by |
| --- | --- | --- |
| Skeleton | any | A person writing a scenario: area, actions, and `?` placeholders for requested observations |
| Reference recording | `circuit_reference` | `circuit-server --record-fixture`, which runs Circuit's reference rules |
| Target-version trace | `harness_trace` | An external instrumented target-version server (not part of this repository) |

Hand-written fixtures can also use `documented_behavior`, for expectations taken from public documentation, or `controlled_experiment`. Only passing `harness_trace` fixtures can support raising a mechanism's claimed compatibility tier. Even then, the change to the compatibility table is reviewed by hand. Reference recordings document Circuit's own behavior and are regression tests, not compatibility evidence.

## Syntax

A fixture is UTF-8 text with one statement per line. Blank lines and lines starting with `#` are ignored; comments cannot follow a statement on the same line. The first line is `circuit-trace 1`. Header keys come next, each at most once, and must precede all body lines.

| Header key | Value |
| --- | --- |
| `title` | 1–200 characters (required) |
| `game-version` | Target game version, for example `1.21.8` (required) |
| `source` | `harness_trace`, `documented_behavior`, `controlled_experiment` or `circuit_reference` (required) |
| `harness` | Build identity of the recorder, or `-` (required) |
| `mechanisms` | One or more mechanism ids from the compatibility table (required) |
| `area` | `x1 y1 z1 x2 y2 z2`, inclusive, at most 128 blocks per horizontal axis (required) |
| `ticks` | Scenario ticks after the setup tick, 1–10000 (required) |
| `random-tick-speed` | Random tick speed game rule, default 0 |
| `neighbor-update-limit` | Chained neighbor update limit, 1–1,000,000; default: the bundle's limit (1,000,000). A harness sets the server's `max-chained-neighbor-updates` property |
| `changes` | `ordered`, `unordered` or `none` (default) |

| Body line | Meaning |
| --- | --- |
| `at <t> set <x> <y> <z> <state>` | Replace the block at the start of tick `t` |
| `at <t> use <x> <y> <z>` | Use the block at the start of tick `t`: toggle a lever, press a button, cycle a repeater's delay, toggle a comparator's mode, or open or close a door, trapdoor or fence gate, as a player facing south (the reference harness's player) |
| `at <t> insert <x> <y> <z> <slot> <item> <count>` | Add items to a container slot at the start of tick `t` |
| `change <t> <x> <y> <z> <old> <new>` | A block changed during tick `t ≥ 1` |
| `block <t> <x> <y> <z> <state or ?>` | Block state at the end of tick `t` |
| `slot <t> <x> <y> <z> <slot> <item or empty> <count>` | Container slot at the end of tick `t`; `slot <t> <x> <y> <z> <slot> ?` requests a recording |

Every position must lie inside the area. Actions run in file order within a tick, and change lines are listed in tick order. Tick 0 is the setup tick: its actions build the scenario, and changes during tick 0 are not observed. Block states use `namespace:id[key=value,...]`; items use `namespace:id`. A file is limited to 16 MiB, 200,000 lines and 4096 characters per line.

## Semantics

- **Loading.** The chunks covering the area plus one chunk of margin are force-loaded (level 31, entity ticking) for the whole run; as in the target versions, the next ring is block ticking and the one after it border. A harness must keep the same chunks force-loaded.
- **Actions.** All actions for a tick are applied before that tick's scheduled ticks, with normal neighbor updates, as inputs at the start of the tick would be.
- **Changes.** With `changes ordered`, the recorder lists every block state change inside the area during each tick from 1 on, in the order the changes were applied, including intermediate states. `unordered` compares each tick's changes as a multiset. Every property of every block state is represented (block registry `circuit-blocks-1`), so every change is compared, including dust shapes and properties the rules only store.
- **Probes.** Probes read state at the end of the tick, after all phases.
- **Randomness.** Random ticks are off unless `random-tick-speed` is set. Circuit's random-tick stream is a declared L1 choice, so exact random outcomes are not expected to match the target version.

Evaluation reports one of four outcomes:

- `PASS` — every compared observation matched.
- `FAIL` — the first mismatch, with its fixture line.
- `UNSUPPORTED` — the fixture uses content or setups the reference rules cannot represent, such as blocks unknown to the target version, or a setup action the rules reject.
- `INVALID` — skeletons with placeholders and unknown mechanisms.

## Content mapping for 1.21.8

The mapping translates target-version notation into Circuit block states. Every block state of the target version has exactly one Circuit state and back. The blocks in the table below have rules of their own and packed states; every other block is a registry state of the block registry `circuit-blocks-1` (`include/circuit/gameplay/block_registry.hpp`), which keeps all of its properties and is inert in the rules (see [the simulation](SIMULATION.md)). A block whose properties differ between versions has one registry block per version: `creaking_heart` is the only one between 1.21.4 and 1.21.8. Notation names every property, in key order, as the target version writes it. Target-version traces confirm the conventions below:

- repeater and comparator `facing` names the input side (vertical_slice, diode_priority, comparator_signal);
- observer `facing` names the observed side (observer_pulse);
- chest `left` and `right` pairing (double_chest, double_chest_left_input);
- water levels (water_channel).

A wrong convention makes fixtures fail rather than pass.

| Block | Properties | Notes |
| --- | --- | --- |
| `air`, `cave_air`, `void_air` | — | three states of the same air block in the rules |
| `stone`, `glass`, `obsidian`, `redstone_block`, `slime_block`, `honey_block` | — | no other properties |
| `lever` | `face`, `facing`, `powered` | `face` and `facing` locate the block the lever hangs on |
| `redstone_wire` | `power` 0–15, `north`/`south`/`east`/`west` (`none`, `side`, `up`) | the stored shape; absent sides are `none`, so dust given without sides is a dot |
| `repeater` | `delay` 1–4, `facing`, `locked`, `powered` | `facing` names the **input** side |
| `redstone_lamp` | `lit` | — |
| `observer` | `facing` (observed side), `powered` | — |
| `piston`, `sticky_piston` | `facing`, `extended` | — |
| `piston_head` | `facing`, `short`, `type` | `short` is stored; the rules place heads with `short=false` |
| `moving_piston` | `facing`, `type` | the moving block is block-entity state and is not part of the notation |
| `hopper` | `facing` (not `up`), `enabled` | — |
| `chest` | `facing`, `type` (`single`, `left`, `right`), `waterlogged` | waterlogging is stored but not simulated; a `left` half pairs clockwise of its facing, `right` counter-clockwise |
| `comparator` | `facing`, `mode`, `powered` | `facing` is taken to name the **input** side; the output signal is block-entity state and is not part of the notation |
| `water` | `level`: 0 source, 1–7 flowing distance, 8–15 falling | the rules treat every falling level alike and write 8 |
| `wheat` | `age` 0–7 | — |
| `redstone_torch` | `lit` | — |
| `redstone_wall_torch` | `facing`, `lit` | `facing` points away from the block the torch hangs on |
| `<kind>_button` (14 kinds) | `face`, `facing`, `powered` | as for levers; each kind is a block of its own |
| `<kind>_pressure_plate` (the same 14 kinds) | `powered` | each kind is a block of its own |
| `light_weighted_pressure_plate`, `heavy_weighted_pressure_plate` | `power` 0–15 | — |
| `target` | `power` 0–15 | — |
| `daylight_detector` | `inverted`, `power` 0–15 | — |
| `note_block` | `instrument`, `note` 0–24, `powered` | — |
| `rail` | `shape` | `waterlogged` |
| `powered_rail`, `activator_rail`, `detector_rail` | `powered`, `shape` | `waterlogged` |
| `<kind>_door`, `<kind>_trapdoor` (21 kinds: the wooden ones, iron and the copper ones) | door: `facing`, `half`, `hinge`, `open`, `powered`; trapdoor: `facing`, `half`, `open`, `powered`, `waterlogged` | each kind is a block of its own; a trapdoor's `waterlogged` is stored only |
| `<kind>_fence_gate` (the 12 wooden kinds) | `facing`, `in_wall`, `open`, `powered` | — |

Items: any item of the rule set's registry (since rules v7, every item of both versions), named `namespace:id`, without data components. Version 1 has no entity observations; dropped and pushed items are visible only through container slots that collect them. Version 2 observes them directly.

## Version 2

A file that starts with `circuit-trace 2` may use everything version 1 allows, plus the stage of each event, entity lifecycles, chunk transitions and random-number draws. A version-1 file keeps its meaning and is written exactly as before. Version 2 is meant for behavior that block changes and slots cannot show: entity creation order and motion, primed TNT, ticket changes and random draws.

**Header.** Additional keys, each optional:

| Key | Value |
| --- | --- |
| `dimension` | The scenario's dimension, default `minecraft:overworld` |
| `entities` | `observed` records entity lines (below); default `none` |
| `chunks` | `observed` records chunk status lines; default `none` |
| `rng` | `observed` records draws from the target version's random streams (requires `entities observed`: the reference harness counts draws while it observes entities); default `none` |
| `tolerance` | Allowed difference of entity positions and velocities, 0–1 blocks; default 0 (exact). A tolerance turns a comparison into a declared approximation and belongs in the fixture's review |
| `load` | `area` (default) force-loads the area as described under Semantics; `none` loads nothing, so that the actions load what the scenario needs |
| `observe <cx0> <cz0> <cx1> <cz1>` | The chunks whose status lines are recorded, inclusive, at most 64 by 64; default every chunk |
| `gamerule <name> <value>` | A game rule set before the scenario starts; repeatable, one line per rule. Circuit knows each version's game rules. randomTickSpeed is world state and takes any integer, replacing `random-tick-speed`; tntExplodes, doDaylightCycle and doWeatherCycle are world state too (`true` or `false`; without a line the day and the weather stand still, as the harness stops their cycles); any other rule at its default is accepted, another value makes the fixture unsupported, and a rule the version lacks makes it invalid. A harness whose version lacks the rule refuses the skeleton |

**Stages.** Every event line names the stage it happened in: `commands`, `block_ticks`, `fluid_ticks`, `random_ticks`, `block_events`, `entities` and `block_entities` (Circuit's phases), `players`, `chunks`, `spawning` and `other` (stages only the target version has), or `-` (unknown). Actions run in the `commands` stage. Comparison includes the stage: an event in a different stage is a different event.

**Lines.**

| Line | Meaning |
| --- | --- |
| `at <t> spawn-item <x> <y> <z> <vx> <vy> <vz> <item> <count> [pickup-delay=<n>] [age=<n>]` | Add an item entity at the start of tick `t`, with a pickup delay of 0 and an age of 0 unless given (in this order): a pickup delay of 0 to 32767 (never picked up) and an age of -32768 (never ages) to 32767 |
| `at <t> command <words>` | Run a server command (without the slash) at the start of tick `t`. Circuit models `forceload add|remove <x> <z> [<x> <z>]` and `forceload remove all` (one forced ticket of level 31 per chunk, at most 256) `gamerule randomTickSpeed <n>`, `summon <type> <x> <y> <z> [{...}]` for `minecraft:tnt`, `minecraft:arrow`, `minecraft:snowball`, `minecraft:minecart` and the boats at absolute coordinates with the data tags `Motion`, `Fire` and `fuse`, `time set <ticks>` (or `day`, `noon`, `night`, `midnight`) and `weather clear|rain|thunder <ticks>` with a duration (without one the official servers draw it from the level random); any other command makes the fixture unsupported |
| `change <t> <stage> <x> <y> <z> <old> <new>` | A block changed (v2 form of the v1 line) |
| `entity-added <t> <stage> <handle> <type> <x> <y> <z> <vx> <vy> <vz> [key=value ...]` | An entity was added, at this position and velocity |
| `entity-removed <t> <stage> <handle> <reason>` | An entity was removed: `killed`, `discarded`, `unloaded_to_chunk`, `unloaded_with_player` or `changed_dimension` |
| `entity-loaded <t> <stage> <handle> <type> <x> <y> <z> <vx> <vy> <vz> [key=value ...]` | An entity removed with `unloaded_to_chunk` or `unloaded_with_player` is back with its chunk, under its handle, at this position and velocity |
| `entity-changed <t> <stage> <handle> <key=value> ...` | An observed property changed, such as an item stack's `count` |
| `entity <t> <handle> <x> <y> <z> <vx> <vy> <vz> [key=value ...]` | A live entity at the end of tick `t` |
| `entity-order <t> <stage> <handle> ...` | The order in which entities ticked in that stage |
| `chunk <t> <stage> <cx> <cz> <status>` | A chunk reached `entity_ticking`, `block_ticking`, `border` or `unloaded` |
| `light <t> <x> <y> <z> <block> <sky>` | Block light and sky light at a position at the end of tick `t` (`?` in a skeleton) |
| `draws <t> <stage> <stream> <count>` | Draws from a random stream in that stage |

Coordinates and velocities are decimal doubles, written as the shortest text that reads back as the same value; recorders may write any decimal form, such as Java's `1.0E-4`. An `entity` line's position is where the entity stands; velocities are in blocks per tick.

**Entities.** Entities added inside the area are observed for their whole lifetime, wherever they go. They are named `e1`, `e2`, … in the order they were added, so handles do not depend on UUIDs or on how each side numbers entities, while the order of creation stays part of the observation. An `entity-added` line introduces the next handle; other lines may only name handles already added, and none may follow an entity's removal. Entity observations start in tick 0, so that entities created by the setup get their handles. An entity its chunk saves (`unloaded_to_chunk`) and loads again keeps its handle: `entity-loaded` makes the handle live again, and uses up the next number, as the reference harness counts it (an entity added after one came back with its chunk is `e3`, not `e2`).

Item entities carry these properties: `item` and `count` (and `damage` when nonzero) when added and changed; `item`, `count`, `damage`, `age`, `pickup-delay` and `on-ground` at the end of each tick. Falling blocks carry `block` (the state, in notation) when added or loaded, and `time` (the ticks they have fallen) and `on-ground` at the end of each tick (harness 0.8.0). Primed TNT (`minecraft:tnt`) carries `fuse` when added or loaded, and `fuse` and `on-ground` at the end of each tick. Arrows carry `in-ground`, `life` (ticks stuck in the ground), `fire` (remaining fire ticks, when burning) and `on-ground` at the end of each tick; other entities `fire` when burning and `on-ground` (harness 0.9.0). Other entity types will define their properties when Circuit models them.

**Event order.** Within a tick, block changes and entity events form one stream in the order they happened. With `changes ordered` the whole stream is compared in order. With `changes unordered` or `none`, changes are compared as in version 1 and the entity events form the ordered stream. `entity` probes are compared by handle, `entity-order` lines as sequences, and `draws` lines exactly.

**Chunk statuses.** The official servers report a tick's chunk changes in the order of their chunk maps, which varies between runs, and may pass a chunk through several statuses within one tick while nothing runs in between. Chunk lines are therefore compared per tick by their effect: the set of chunks whose status at the end of the tick differs from their status before it, with that status. The official servers also load new chunks 2 to 4 ticks after their ticket, varying between runs, so fixtures that compare exactly observe chunks only where the timing repeats; the scenarios `ring_blocks` and `unload_reload` record no chunk lines.

**Comparison with Circuit.** Circuit observes its item entities and chunks from its own trace records: entity spawns with their exact position and velocity, removals (despawning, falling out of the world and pickup are `discarded`, a removal command is `killed`), stack changes, and chunk statuses from each chunk's load level. Positions and velocities are doubles, as on the target versions, so `spawn-item` positions are used exactly and probes compare them bit for bit. Each level has a random sequence like the target versions' level random; for a fixture Circuit seeds it with 0, as the harness does, and counts its raw draws per stage. With `rng observed` these counts must equal the recording's `draws` lines of the stream `level`: a stage where only one side draws fails, whichever side it is. Other streams are not observed and fail. These failures are the point: a version-2 trace that shows a difference stays a visible failing case, not a passing one.

The skeletons `item_hopper_pickup`, `item_piston_push`, `item_water_current` and `item_drop_piston`, and the open skeleton `tnt_redstone_ignition`, are the first version-2 scenarios; `piston_draws` adds the draws of a piston's sounds. The item skeletons `item_merge`, `item_merge_moving`, `item_merge_crossing`, `item_merge_vertical`, `item_merge_timers`, `item_hopper_area`, `item_hopper_inside`, `item_chest_scatter`, `item_chunk_edge`, `item_water_fall`, `item_water_still`, `item_water_channel`, `item_water_ice_corner` and `item_water_falling` observe merging, hopper collection, a removed chest's scatter, chunk transitions and water; `falling_drop`, `falling_column`, `falling_onto` and `falling_void` observe falling blocks; `tnt_ignition`, `tnt_motion`, `tnt_water_current`, `tnt_reload` and `tnt_explodes_disabled` (1.21.8 only) observe TNT before it explodes, `tnt_redstone_ignition`, `tnt_obsidian`, `tnt_drops`, `tnt_void`, `tnt_entity_order`, `tnt_knockback`, `tnt_items`, `tnt_chain_edge`, `tnt_water` and `tnt_power` its explosions, and the open `tnt_reload_pending` a pending tick across an unload; `arrow_flight`, `arrow_tnt` and `arrow_life` observe arrows and snowballs, and the open `arrow_reload` arrows across an unload; `minecart_ground`, `minecart_water` and `boat_land` observe vehicles, and the open `boat_motion` boats afloat; `piston_entities` and `slime_entities` observe moving blocks pushing, carrying and launching entities. Passing official recordings are in `tests/fixtures/harness*/`; the others stay open with their first difference listed in `EXPECTED.txt`.

**Chunks and entities.** The official servers remove the entities of a chunk that unloads, and add them again when it loads, outside the stages the harness instruments (`other`). Circuit reports both from its chunk stage under the stage `other`: an entity leaves in the tick its column unloads and comes back in the tick after the column loads again, with a new id; Circuit observes it under its old handle.

**Entity order.** An entity ticks in the entity phase unless something removed it before its turn: Circuit's entity phase runs one root per entity, keyed by its id, so a removal by the root of an entity with a lower id (an item merging into another) means the removed entity did not tick, as on the target versions.

**Recording on the target version.** The reference harness records version 2 on both releases. It names stages from its hooks into the level tick: the chunk tick counts as `random_ticks`, and work between the instrumented stages is `other`. Chunk status changes are `chunks`. It observes entities added to the level inside the area, removals with the target version's reason, the order of entity ticks, and stack changes at stage boundaries. It counts the raw draws of the overworld's level random, which it seeds with 0 when the run starts, and it restarts the entity id counter at tick 0, so that runs repeat and ids (which some entity behavior reads) match Circuit's, which start at 1. From harness 0.10.0 it also sets the game time at tick 0, so that at the end of tick t it is t + 2, as in Circuit's runs (daylight detectors update when it is a multiple of 20); traces of earlier harnesses do not depend on it. It observes entities when they are added to the level, and, from harness 0.7.0, when a chunk loads an entity it saved, which it recognizes by its UUID. Chunks near the world origin are spawn chunks on the official servers; scenarios about chunk levels stay away from them (`unload_reload` and `item_chunk_edge` use x ≥ 320). Every trace is recorded twice on fresh worlds; the two recordings must be identical.

## Workflow

```bash
xmake run circuit-server --record-fixture "$PWD/tests/fixtures/skeletons/vertical_slice.ctrace" \
  --output "$PWD/tests/fixtures/reference/vertical_slice.ctrace"
xmake run circuit-server --check-fixture "$PWD/tests/fixtures/reference/vertical_slice.ctrace"
xmake run evidence_tests
```

| Directory | Contents |
| --- | --- |
| `tests/fixtures/skeletons/` | Scenario skeletons |
| `tests/fixtures/reference/` | Reviewed reference recordings of the skeletons |
| `tests/fixtures/harness/`, `harness-1.21.4/` | Target-version traces recorded from the skeletons (or documented fixtures) of the same name; all pass |
| `tests/fixtures/open/`, `open-1.21.4/` | Target-version traces Circuit does not match yet, with `EXPECTED.txt` listing each one's outcome and first mismatch |
| `tests/fixtures/open-skeletons/` | Skeletons of open traces with content Circuit cannot represent yet, such as TNT. Each has a trace in every open directory, or a line in that directory's `NOT_APPLICABLE.txt` with the harness's reason for refusing it |
| `tests/fixtures/documented/` | Hand-written fixtures from public documentation |
| `tests/fixtures/workloads/` | Profiling workloads generated by `tools/generate_workloads.py`: setup actions only |

**Comparing two recordings.** `circuit-server --diff-fixtures <first> <second> [--tolerance <blocks>]` reports the first observation on which two recordings of the same scenario differ, with both line numbers: an official recording against Circuit's, two runs, two versions, or sequential against parallel execution. Provenance is not compared, block-state properties may come in any order, and a differing scenario is reported as such. `tools/scenario.py` runs a skeleton with Circuit (1 worker and several), on each reference harness given (twice), and prints a report of every comparison.

`evidence_tests` re-records every skeleton and fails if the result differs from the committed recording, so changes to the rules show up as fixture diffs. It also checks that every target-version trace was recorded from the scenario currently in the repository, and that it passes on the engine and on the oracle. Open traces must fail, or be unsupported, with exactly the first mismatch `EXPECTED.txt` records: a change in Circuit that moves the mismatch, or makes the trace pass, has to update that file, and a trace that passes moves to the harness directory. The [compatibility ledger](../compatibility/README.md) lists open traces as failing or unsupported cases.

An external harness takes the same skeletons and produces `harness_trace` fixtures. The harness is an instrumented target-version server and lives outside this repository. No game code, decompiled source, or mappings enter the repository; only trace files are copied in. Review harness output for third-party content before committing it, and record the harness build identity in the `harness` key. The [development guide](DEVELOPMENT.md) describes the workflow.
