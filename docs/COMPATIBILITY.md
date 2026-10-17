# Compatibility

Circuit states compatibility separately for four layers. Passing one layer says nothing about another: a client can connect (protocol) to a world whose blocks it shows correctly (content) while a machine in it behaves differently from the official server (behavior).

This guide describes current support. The [vanilla completion roadmap](VANILLA_COMPLETION.md) defines the full 1.21.4/1.21.8 target, missing systems and future acceptance gates; planned features are not current capabilities.

| Layer | Question it answers | Evidence |
| --- | --- | --- |
| [Protocol](#protocol) | Can a client of this version connect and exchange the packets the server uses? | Documented packet tables, a protocol library (mineflayer) and the real client |
| [Content](#content) | Which blocks, items and saved data does Circuit represent, and how are they named? | Content tables checked against public data; Anvil round trips through the official server |
| [Behavior](#behavior) | Does a mechanism do what the official server does, in the same order? | A compatibility tier per mechanism, backed by harness traces |
| [Extensions](#extensions) | Which plugin and mod interfaces can code be written against? | Circuit's own APIs with versioned ABIs; no third-party plugin API |

## Protocol

| Version | Protocol | Status |
| --- | --- | --- |
| Java Edition 1.21.8 | 772 | Supported: play, status, offline and online login |
| Java Edition 1.21.4 | 769 | Supported: play, status, offline and online login |
| Java Edition 1.20.4 | — | Listed candidate without rules; refused |

One server runs one version. A client of another protocol is refused at login with a message naming the server's version.

**Connection.** Handshake, status (with ping), login with compression and, in online mode, encryption and session verification, configuration (known packs, registries, tags, feature flags) and play are implemented. Limits on frame size, decompressed size, queued bytes, packet rate and login time apply to every connection; see [the server guide](SERVER.md).

**Play packets.** The server understands these serverbound packets: teleport confirmation, movement (position, rotation, status), player actions (digging, dropping items), player commands (sprinting), client status (respawning), use item and use item on a block, held slot, creative inventory slot, container clicks and closing, player input (sneaking), abilities (flying), chat, chat commands, keep-alive and player loaded. Others are read and ignored. It sends the packets these need: login, chunks with light, chunk batches, block updates and acknowledgements, entity spawn, movement, data and removal, item pickup, damage events, death screens, respawning, player info, abilities, health and hunger, window opening, content, slot and closing, the carried item, held slot, spawn position, time, game events, system chat, command tree, keep-alive and disconnect.

**Not supported:** signed chat (chat is relayed as system messages), resource packs, boss bars, scoreboards, titles, sounds and particles, other dimensions, and vehicles. A client that sends a packet for one of these gets no effect.

**Data sources.** Packet ids and field layouts come from cited minecraft.wiki revisions and the MIT-licensed minecraft-data dataset; [the server guide](SERVER.md#protocol-data) lists the revisions and the consistency checks.

## Content

Every block state of both versions is represented, with its official name and properties (the block registry `circuit-blocks-1`, from public data). The reference rules give behavior of their own to the blocks below; the properties they only store are listed as display-only.

| Blocks | Properties the rules use | Display-only properties |
| --- | --- | --- |
| `air`, `cave_air`, `void_air` | — | — |
| `stone`, `glass`, `obsidian`, `redstone_block`, `slime_block`, `honey_block` | — | — |
| `redstone_wire` | `power`, `north`, `south`, `east`, `west` (the stored shape) | — |
| `lever` | `face`, `facing`, `powered` | — |
| `repeater` | `delay`, `facing`, `locked`, `powered` | — |
| `comparator` | `facing`, `mode`, `powered` | — |
| `redstone_lamp` | `lit` | — |
| `observer` | `facing`, `powered` | — |
| `piston`, `sticky_piston`, `piston_head`, `moving_piston` | `facing`, `extended`, `type` | `short` |
| `hopper` | `enabled`, `facing` | — |
| `chest` | `facing`, `type` | `waterlogged` (stored, not simulated) |
| `water` | `level` | — |
| `wheat` | `age` | — |
| `redstone_torch`, `redstone_wall_torch` | `facing`, `lit` | — |
| `*_button` (every kind) | `face`, `facing`, `powered` | — |
| `*_pressure_plate` (every kind, the weighted ones too) | `powered`, `power` | — |
| `target` | `power` | — |
| `daylight_detector` | `inverted`, `power` | — |
| `note_block` | `instrument`, `note`, `powered` | — |
| `rail` | `shape` | `waterlogged` (stored) |
| `powered_rail`, `activator_rail`, `detector_rail` | `powered`, `shape` | `waterlogged` (stored) |
| `*_door`, `*_trapdoor`, `*_fence_gate` (every kind) | `facing`, `half`, `hinge`, `in_wall`, `open`, `powered` | a trapdoor's `waterlogged` (stored, not simulated) |

**Other blocks** keep every property and are inert: they are solid for items when they have a collision shape, conduct redstone power when opaque, support dust and diodes on faces their collision shape covers (or any face when opaque), cannot be moved by pistons, break at their documented hardness with their documented tool, and do nothing else. With the official data pack they drop from their loot tables, except where a table draws random numbers (see [data packs](DATA_PACKS.md)). Players collide with their exact collision shapes. A world may contain them, and the client shows them, but no machine that depends on their behavior works yet.

**Items.** The rule set knows every item of both versions (1,415), with its stack size and durability, from PrismarineJS minecraft-data (MIT) at the pinned commit. Any item can be held, stored in containers, moved through hoppers, thrown and picked up; items without rules are otherwise inert. Placing works for the blocks above, and a water bucket places water. Each version's protocol uses its own item ids for the same names.

**Item components.** Item stacks keep every data component of their version: names, lore, enchantments, potions, books, containers and the rest. Each component type of both protocols (96 in 1.21.8, 67 in 1.21.4) is read from the network, checked and kept in a canonical form, so equal stacks compare equal and stack together whatever order a client sent; schemas come from PrismarineJS minecraft-data at the pinned commit. Components travel through containers, hoppers, item entities, checkpoints and logs unchanged. The rules use three of them: damage, max stack size and max damage; the others have no behavior yet. A component that refers to a registry the server does not synchronize is refused: a creative player cannot take such a stack, and the server sends the inventory again. Trim materials and patterns, banner patterns, goat-horn instruments and jukebox songs are always synchronized; enchantments only when the server loads the official jar's data pack, whose tags their definitions need (see [data packs](DATA_PACKS.md)); the data component predicates of block predicates never are yet.

**Saved worlds.** Anvil worlds of Java Edition 1.21.8 (DataVersion 4440) import and export with every block state, as long as their block entities, entities and items are the ones described here and in [persistence](PERSISTENCE.md#anvil-import-and-export), in the overworld. Other content is refused by default, or substituted or dropped under an explicit policy with a report; see [persistence](PERSISTENCE.md#anvil-import-and-export). A world directory belongs to one version bundle and rule set; `--migrate-world` moves it to the other version when both run the same rules.

**Block entities.** Containers keep their custom name (the window title) and components. The block entities of blocks without rules of their own (signs, banners, furnaces and the rest) keep their saved data through checkpoints and Anvil interchange, but nothing interprets it and clients do not receive it: a sign shows no text.

**Not represented:** mobs and other entities beyond items, falling blocks, primed TNT and players, biomes other than plains, lighting (everything is lit by the sky), and world generation beyond a flat floor.

### Technical machines

Block and item registration do not provide behavior. Coral fans exist as inert blocks (pistons destroy them as the official servers do), and their items cannot be placed by the current rules. Rails take their shapes and pass power, and minecarts ride them; detector rails sense nothing and minecarts do not collide with each other. Common building blocks such as cobblestone, dirt and oak logs are inert too; sand, gravel and concrete powder fall, and TNT is primed and explodes (below). Survival inventories and container screens therefore do not constitute complete vanilla survival support.

| Mechanism or machine | Current support |
| --- | --- |
| Piston quasi-connectivity and activation after an update | Recorded target-version scenarios pass; this does not cover every BUD layout. |
| Sticky-piston block dropping on a short pulse | The one-game-tick pulse scenario passes recorded traces for both versions. |
| Slime structures, the 12-block push limit and observers watching piston writes | Implemented with rule, trace and cross-chunk simulation checks. Pistons remain L2 because item pushing, drops and some update ordering have gaps. |
| TNT ignition, fuse, motion and explosions | Redstone and explosions prime TNT (placed by commands, players or pistons); primed TNT's motion and fuse and its explosions (blocks, drops, chain reactions, item damage and knockback) match recorded traces of both versions. Burning arrows prime TNT; fire, flint and steel and dispensers do not, and explosions do not hurt players. |
| TNT duplication machines | Not supported. Primed TNT, explosions, rails and minecarts on them exist, but coral fans are inert, minecarts do not collide, and TNT duplication itself is not modeled. No TNT-duplication reference fixture exists. |
| Honey-based flying machines | Honey blocks, their separation from slime and the entities they carry match recorded traces of both versions, including a small slime-honey flying machine. Larger flying-machine designs have no acceptance evidence yet. |
| Redstone torches, buttons, pressure plates and target blocks | Torch inverters, towers and burnout, buttons pressed by players and arrows, pressure plates sensing items and arrows (weighted ones counting them) and target blocks grading hits match recorded traces of both versions, including the update order of the dust they drive. Mobs and players do not press plates yet, and tripwire is not modeled. |
| Daylight detectors | Normal and inverted detectors follow the sky light, the time of day, rain and thunder every 20 game ticks, and invert when used, as in recorded traces of both versions over a day, with and without the daylight cycle. Sculk sensors, lightning rods and the other sensors are not modeled. |
| Doors, trapdoors and fence gates | Opened and closed by power and by hand, with their halves, shapes and sound draws, as in recorded traces of both versions. Bells, copper bulbs and other consumers are not modeled yet. |
| Rails | Rails, powered, activator and detector rails take their shapes from their neighbors, curve, rise, switch at junctions and pass power, and empty minecarts ride them (slopes, curves, braking and boosting), as in recorded traces of both versions. Detector rails sense nothing, activator rails do nothing to minecarts, and minecarts do not collide with each other. |
| Note blocks | Their instruments from the block below or a head above, playing by power and by use as block events with the sound's draw, silenced by a block above, and retuned when pistons move them, as in recorded traces of both versions. Player heads with a sound of their own play nothing. |
| Dispensers and droppers | Not implemented. Machines that need them are not supported. |
| Portal chunk loaders, sand duplication and bedrock-breaking machines | No compatibility claim. Dimensions/portals and the required block/interaction coverage are absent; falling blocks exist but are not duplicated. |

Importing an unsupported machine with substitution does not preserve its mechanism. Default Anvil import rejects unrepresentable content; explicit substitution can replace blocks or discard entities and reports those losses.

## Behavior

Each mechanism of the rule set declares a compatibility tier:

| Tier | Meaning |
| --- | --- |
| L0 | State, event order and random-number use match the target version, checked against traces recorded on the official server |
| L1 | Exact within a region; only invariants across regions |
| L2 | Circuit's own definition; known or uncompared differences are listed |

`circuit-server --compatibility` prints the table of the selected bundle, with each row's tier, evidence and gaps. [The simulation guide](SIMULATION.md#status) explains the rows and the evidence behind them. Both supported versions run the same rule set: the traces recorded on 1.21.4 are identical to those recorded on 1.21.8.

Behavior the server adds on top of the rules, such as player placement, digging time and container windows, is checked against the official server with the client harnesses; [the server guide](SERVER.md#design) describes what is validated. Player inventories, survival use and the click modes of container windows are part of the rule set; their results were compared with the official servers of both versions in a 48-step window scenario. With the official data pack, 2x2 crafting was compared in a 48-step scenario and block drops in 23 cases per version (see [data packs](DATA_PACKS.md)).

Differential tests compare the parallel runtime with a sequential executor at several worker counts and region layouts. Grouped player-action regressions also compare dependent placements and uses across batch boundaries. These checks cover their scenarios; because the executors share gameplay rules, independent target-version evidence is required to establish vanilla behavior.

## Extensions

| Interface | Version | Compatibility promise |
| --- | --- | --- |
| Native modules (`include/circuit/plugin/api.hpp`) | API 2, ABI tag `circuit-v2;compiler;standard library;...` | Modules must be built with a compatible toolchain and standard library; a mismatched API version or ABI tag is refused at load. See [the plugin API](PLUGIN_API.md#abi). |
| Java plugins (`org.circuitmc.spi`) | Bridge protocol 1, Java 17 | Plugins run in a separate JVM and use only the SPI; the bridge refuses another protocol version at the handshake. See [the Java bridge](../modules/java-bridge/README.md). |
| Bukkit, Spigot, Paper | — | Not supported. See [the evaluation](BUKKIT_EVALUATION.md). |

Modules cannot add blocks, items or rules: content and behavior are fixed by the rule set, so extensions never change the behavior layer.
