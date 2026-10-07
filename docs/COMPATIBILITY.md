# Compatibility

Circuit states compatibility separately for four layers. Passing one layer says nothing about another: a client can connect (protocol) to a world whose blocks it shows correctly (content) while a machine in it behaves differently from the official server (behavior).

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

The reference rules model a fixed set of blocks and items. Their names and state properties are the official ones.

| Blocks | Properties the rules use | Display-only properties |
| --- | --- | --- |
| `stone`, `glass`, `obsidian`, `redstone_block`, `slime_block` | — | — |
| `redstone_wire` | `power`, `north`, `south`, `east`, `west` (the stored shape) | — |
| `lever` | `face`, `facing`, `powered` | — |
| `repeater` | `delay`, `facing`, `locked`, `powered` | — |
| `comparator` | `facing`, `mode`, `powered` | — |
| `redstone_lamp` | `lit` | — |
| `observer` | `facing`, `powered` | — |
| `piston`, `sticky_piston`, `piston_head`, `moving_piston` | `facing`, `extended`, `type` | `short` |
| `hopper` | `enabled`, `facing` | — |
| `chest` | `facing`, `type` | `waterlogged` (always false) |
| `water` | `level` | — |
| `wheat` | `age` | — |

**Items.** The rule set knows every item of both versions (1,415), with its stack size and durability, from PrismarineJS minecraft-data (MIT) at the pinned commit. Any item can be held, stored in containers, moved through hoppers, thrown and picked up; items without rules are otherwise inert. Placing works for the blocks above, and a water bucket places water. Item stacks carry one data component, the damage of items with durability; other components are not represented. Each version's protocol uses its own item ids for the same names.

**Saved worlds.** Anvil worlds of Java Edition 1.21.8 (DataVersion 4440) import and export as long as they hold only this content, in the overworld, with no item component other than damage. Other content is refused by default, or substituted or dropped under an explicit policy with a report; see [persistence](PERSISTENCE.md#anvil-import-and-export). A world directory belongs to one version bundle and rule set; `--migrate-world` moves it to the other version when both run the same rules.

**Not represented:** other blocks, item components other than damage, block entity data beyond containers and comparators, mobs and other entities beyond items and players, biomes other than plains, lighting (everything is lit by the sky), and world generation beyond a flat floor.

## Behavior

Each mechanism of the rule set declares a compatibility tier:

| Tier | Meaning |
| --- | --- |
| L0 | State, event order and random-number use match the target version, checked against traces recorded on the official server |
| L1 | Exact within a region; only invariants across regions |
| L2 | Circuit's own definition; known or uncompared differences are listed |

`circuit-server --compatibility` prints the table of the selected bundle, with each row's tier, evidence and gaps. [The simulation guide](SIMULATION.md#status) explains the rows and the evidence behind them. Both supported versions run the same rule set: the traces recorded on 1.21.4 are identical to those recorded on 1.21.8.

Behavior the server adds on top of the rules, such as player placement, digging time and container windows, is checked against the official server with the client harnesses; [the server guide](SERVER.md#design) describes what is validated. Player inventories, survival use and the click modes of container windows are part of the rule set; their results were compared with the official servers of both versions in a 48-step window scenario.

The parallel runtime never changes behavior: every test compares it with a sequential oracle at several worker counts and region layouts, and the server's results do not depend on how player actions fall into ticks.

## Extensions

| Interface | Version | Compatibility promise |
| --- | --- | --- |
| Native modules (`include/circuit/plugin/api.hpp`) | API 2, ABI tag `circuit-v2;compiler;standard library;...` | Modules must be built with a compatible toolchain and standard library; a mismatched API version or ABI tag is refused at load. See [the plugin API](PLUGIN_API.md#abi). |
| Java plugins (`org.circuitmc.spi`) | Bridge protocol 1, Java 17 | Plugins run in a separate JVM and use only the SPI; the bridge refuses another protocol version at the handshake. See [the Java bridge](../modules/java-bridge/README.md). |
| Bukkit, Spigot, Paper | — | Not supported. See [the evaluation](BUKKIT_EVALUATION.md). |

Modules cannot add blocks, items or rules: content and behavior are fixed by the rule set, so extensions never change the behavior layer.
