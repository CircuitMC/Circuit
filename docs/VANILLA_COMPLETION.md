# Vanilla completion roadmap

Status: in progress. Baseline reviewed on 2026-10-08 at source commit `107738a`, with `circuit-reference-rules-v7`. Delivered so far: C0 (version manifests, the feature/evidence ledger, circuit-trace v2 with entity, lifecycle and random-draw observations recorded on both official servers, and first-difference reports) C1.1 (a representation of every block state of both versions), C1.2 (item and block-entity components kept losslessly) C1.3 (version bundles that own their content schema, registries, game rules, pack formats and the evidence for their shared rules) and C1.4 (data packs from the operator's official jar: tags, enchantments, block tools, every recipe checked, 2x2 crafting and deterministic block loot). C1 is complete. C2 is complete under `circuit-reference-rules-v10`: C2.1 (levels with separate state, named by commands, traces and storage), C2.2 (typed chunk tickets with load and simulation levels, what each level runs, idle levels, each version's random-tick range, `/forceload`, the chunk stage after scheduled ticks, pending work across unload and reload, pausing when empty), C2.3 (block and sky light of every state, sent to clients and read by crops; the heightmaps clients get) and C2.4 (game rules, the day time and the weather as world state, with `/gamerule`, `/time` and `/weather`), each compared with both official servers; the C2 gate is recorded with the remaining deviations. C3 is in progress: C3.1 (entity positions and velocities in doubles with an explicit kind, collision against every state's boxes, each level's random sequence with the draws of block drops and piston sounds, and item motion identical with the official traces) and C3.2a (item merging, hopper collection and the scatter of removed containers) C3.2b (items in still, flowing and falling water) and C3.2c (entities leaving and coming back with their chunks), C3.2d (every state's collision boxes checked against both official servers, players moving on them) C3.3a (sand, gravel and concrete powder falling) and C3.4 (TNT primed by redstone and primed TNT's fuse and motion) C3.5 (TNT's explosions: blocks, drops, chain reactions, damage and knockback) and C3.3b (arrows and snowballs, burning arrows priming TNT), each compared with both official servers.

Parts I and II delivered a working simulation and integration foundation for a limited gameplay subset. The next deliverable is a server that implements the server-side features of **Java Edition 1.21.4 and 1.21.8**, with one authoritative version selected at startup. Passing the original milestones does not establish full vanilla compatibility.

## Definition of completion

The target includes all stable-release blocks, states, items and components; entity types and behavior; dimensions, generation and lighting; survival and creative interactions; technical gameplay; commands and data packs; persistence; multiplayer protocol; and dedicated-server operation. Adventure, spectator, hardcore, difficulties, gamerules and supported world presets are part of the target. Features available only through commands still count.

The normal release profile and each optional built-in experimental profile must have separate inventories and results. Experimental behavior must not leak into the default profile. Experimental profiles remain visible backlog until implemented and tested; an unqualified all-features claim requires their disposition too.

The server must produce the state and messages the unmodified client needs, including sounds, particles, maps, dialogs and metadata where applicable. Reimplementing the client renderer, launcher, Realms hosting service, Marketplace, Bedrock networking or third-party Java mod/plugin APIs is outside this dedicated-server compatibility target. Circuit's own native and Java extension APIs remain separate capabilities.

Completion requires both **implemented behavior** and **version-specific evidence**. A name in a registry, a correctly encoded packet, a passing Circuit-versus-Circuit comparison, or a playable scenario is insufficient by itself. Known gameplay differences remain open compatibility work; they cannot be hidden by marking their parent milestone complete.

## Measured baseline

The numerical inventories below come from the checked-in protocol tables, generated from [PrismarineJS minecraft-data at the pinned commit](https://github.com/PrismarineJS/minecraft-data/tree/095709f3ad62b043069141dc127fc80ef6cd10fa). They describe protocol names and states, not implemented rules.

| Inventory | Java 1.21.4 | Java 1.21.8 | Current Circuit behavior |
| --- | ---: | ---: | --- |
| Block names | 1,095 | 1,105 | All represented by the block registry `circuit-blocks-1` (1,106 blocks, one per property schema); 22 have rules of their own, the others are inert |
| Block-state IDs | 27,866 | 27,946 | All map to one internal state and back; only the 22 modeled blocks have behavior |
| Item entries, including air | 1,385 | 1,416 | Union registry of 1,415 non-air items; storage/transfer is broader than item use |
| Entity-type names | 149 | 151 | Item entities in the engine; players handled by the server; most entity families absent |
| Placement mappings | — | — | 17 item cases, including water buckets and wheat seeds |
| Existing recorded reference scenarios | 30 | 30 | Narrow block/container observations, not a census of vanilla features |

The [vanilla ledger](VANILLA_LEDGER.md) records the same inventories, and more, feature by feature: for each version, every mechanic of families V01–V36 and every enumerated block, item, entity type, effect, enchantment, attribute, damage type, biome, particle, command and packet has an implementation state, an evidence state and its cases. Its data and validator are in [`compatibility/`](../compatibility/README.md); `tools/check.sh` fails when they disagree with the source or with each other. The version manifests there were verified against the official server and client jars.

Every block state of both versions has an internal representation (C1.1): blocks with rules of their own keep a packed state, and every other block is a state of the block registry `circuit-blocks-1`, inert in the rules. Dimensions currently contain only the overworld. Entities have an explicit kind (only items so far), double-precision positions and velocities and collision against every state's collision boxes; each level has a `java.util.Random`-compatible level random. Both bundles currently share one rule implementation. These are architectural expansion points, not merely missing tests.

The 2026-10-08 debug run passed all 15 test programs, including previous review regressions. Existing traces support the documented subset. There is no defensible overall completion percentage: equal weight for a decorative block, world generation and mob AI would produce a misleading number. The remaining work includes multiple major systems as well as deeper validation of existing systems.

## Version baselines and differences

The target releases include the entire earlier game, not just their release additions. For example, both include the Trial Chambers, Crafter, trial spawners, vaults, Breeze, Bogged and Mace from [Java 1.21](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21). These also need behavior and tests.

| Release boundary | Server-relevant scope to inventory | Consequence for the plan |
| --- | --- | --- |
| [1.21.4](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-4) | Pale Garden, pale-oak/resin families, Eyeblossoms and the Creaking/heart interaction; data-pack format 61 and resource-pack format 46 | These belong to both target baselines. Generation, AI, block updates, loot and client events must agree. |
| [1.21.5](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-5), inherited by 1.21.8 | Animal variants, new plants, component/entity-data changes, chat changes, data-driven tests and `tntExplodes` | Keep separate data/behavior adapters. The TNT gamerule must not be assumed to exist in 1.21.4. |
| [1.21.6](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-6), inherited by 1.21.8 | Happy Ghast/Dried Ghast/harnesses, leash and saddle interactions, locator waypoints/dialogs, projectile and splash-potion changes | Add version-specific entity, interaction, networking and command cases. These are not ID-table substitutions. |
| [1.21.7](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-7), inherited by 1.21.8 | New painting and music-disc loot; data-pack format 81 and resource-pack format 64 | Registry presence, loot and client presentation need separate checks. |
| [1.21.8](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-8) | Graphics hotfix release | Pin this exact server/client baseline. Client graphics fixes do not imply new server mechanics, and do not remove earlier differences. |

The checked-in 1.21.8 numerical tables add ten block names and 31 item names relative to 1.21.4. Entity names add `happy_ghast`, `splash_potion` and `lingering_potion`, and remove `potion`. Numeric IDs are local to each bundle. A shared implementation is appropriate only where evidence establishes shared semantics.

These release notes seed a version-difference inventory; they are not a full behavior specification. Every listed change and bug fix needs classification as server behavior, wire/storage schema, data, client-only, or unresolved. Facts inferred from a small scenario must remain scoped to that scenario.

## Capability gap inventory

**Partial** means some behavior exists, with the stated gaps. **Missing** means the audited tree lacks an implementation for the family. Every row needs finer per-version cases before it can be accepted as complete. A milestone owns delivery, while other milestones may provide prerequisites.

| ID | Capability family | Current gap | Delivery |
| --- | --- | --- | --- |
| V01 | Version manifests and evidence | Partial: packet/content IDs exist; full feature, schema and behavioral-difference inventory does not | C0 |
| V02 | Block registry and state properties | Partial: every block, property and collision shape represented; behavior for 22 blocks, the others inert with data-derived opacity, face coverage, hardness and tools | C1, C5 |
| V03 | Item/entity components and registries | Partial: every item component of both protocols is read, checked and kept; only damage, max stack size and max damage have behavior; components of unsynchronized registries are refused; entity components missing | C1, C5, C6 |
| V04 | Data and resource-pack negotiation | Partial: the official jar's data pack and further data packs load with the target version's precedence and checks; tags and the synchronized registries are sent; worldgen and damage-type registries from packs, reload, client pack negotiation beyond the vanilla pack and resource packs missing | C1, C9, C10 |
| V05 | Dimensions, tickets and chunk lifecycle | Partial: overworld, levels with separate state for the Nether and the End (not generated or reachable), typed tickets with level propagation, load and simulation levels and what each runs, idle levels, the server's start and player tickets, `/forceload`, pending work across unload and reload; teleport, portal and ender pearl tickets, chunk-loading delays, and generation and rules of the other dimensions missing | C2, C7 |
| V06 | Lighting and environmental state | Partial: block and sky light of every state computed and compared with both official servers, sent to clients and read by crops; the heightmaps clients get; the day time and rain and thunder with their commands and client events, compared with both official servers; lightning, snow and ice, other effects of rain and everything that depends on the time of day missing | C2, C3, C7 |
| V07 | RNG and global event order | Partial: deterministic Circuit order; random-tick streams, entity order and remaining update behavior differ or lack target evidence | C0, C2, C4 |
| V08 | Terrain and biomes | Missing: same-seed terrain/noise, caves, aquifers, carvers, biome allocation and world presets | C7 |
| V09 | Structures and decoration | Missing: structures, jigsaw placement, features, ores, trees, archaeology and structure loot | C7, C5 |
| V10 | Portals and dimension travel | Missing: portal search/creation, scaling, arrival, respawn anchors, End gateways and load effects | C7 |
| V11 | Common building blocks | Partial: stone/glass and a few technical blocks; dirt, cobblestone, wood, stairs, slabs, doors, fences and most families missing | C5 |
| V12 | Fluids, fire and falling blocks | Partial: limited water; sand, gravel and concrete powder fall and land as on the official servers; lava, waterlogging, buckets, fire, snow/ice, anvils and other gravity blocks missing or incomplete | C3, C5 |
| V13 | Crops, growth and ecology | Partial: simplified wheat; farmland, light/hydration, bone meal, trees, leaves and other crops/plants missing | C5 |
| V14 | Redstone sources and signal consumers | Partial: lever/wire/diodes/observer/lamp; torches, buttons, plates, targets, daylight detectors, note blocks, copper bulbs and other components missing | C4 |
| V15 | Pistons and moving structures | Partial: push/pull/slime and short-pulse cases; honey, exact entity interaction and uncovered update sequences missing | C3, C4 |
| V16 | Automation and storage blocks | Partial: chests/hoppers; dispensers, droppers, Crafter, shulker boxes and item-component-aware automation missing | C4, C5 |
| V17 | TNT and explosions | Partial: TNT primed by redstone (placed, moved by pistons or updated; tntExplodes) and by explosions, primed TNT's launch, fuse and motion, and its explosions (blocks, drops, chain reactions, item damage, knockback) identical with the official traces; fire, players, projectiles, dispensers, player damage and other explosion sources missing | C3, C4 |
| V18 | Technical machines and vanilla quirks | Partial component evidence only; TNT duplication, complete flying machines, loaders, gravity-block machines and selected bedrock-breaking cases have no acceptance evidence | C4, C7 |
| V19 | General entity lifecycle | Partial: an explicit entity kind (items, falling blocks and primed TNT so far) with double-precision state, tick counts, and entities leaving and coming back with their chunks as on the official servers; UUIDs, attributes, components, passengers, leashes and tracking need expansion | C3 |
| V20 | Item entities and experience | Partial: item motion and collision, currents and floating in water, merging, despawning, hopper collection, block drops and container scatter identical with the official traces; experience orbs missing | C3, C6 |
| V21 | Projectiles, vehicles and mounts | Partial: summoned arrows and snowballs identical with the official traces (flight, sticking, falling, life, fire, TNT); entity hits, shooting, tridents, potions, pearls, fireworks, wind charges, boats, minecarts and riding missing | C3, C6 |
| V22 | Movement and player modes | Partial: walking/collision/creative/survival/spectator; complete swimming, crawling, climbing, elytra, vehicles, adventure and hardcore behavior missing | C6 |
| V23 | Combat, attributes and effects | Partial: fall/void damage, basic health/hunger; combat, shields, armor, enchantments, potions, other damage causes and knockback missing | C6 |
| V24 | Food, equipment and item use | Partial: placement and tool wear; eating, drinking, bows, fishing, buckets, shears, cooldowns and most use actions missing | C5, C6 |
| V25 | Menus, recipes and production | Partial: player/chest/hopper clicks; 2x2 crafting with data-pack recipes; deterministic block loot tables; crafting table, special recipes, recipe book, furnace family, brewing, enchanting, anvil, smithing, loom, stonecutter and other menus missing | C5 |
| V26 | Mob AI and navigation | Missing: goals/brains/sensors, pathfinding, breeding, taming and species-specific behavior | C8 |
| V27 | Spawning and farms | Missing: natural spawning, caps, distances, despawn, spawners/trial spawners and biome/dimension rules | C8 |
| V28 | Villages, raids and trading | Missing: POIs, professions, schedules, gossip, reputation, restocking, raids and wandering traders | C8 |
| V29 | Bosses and progression | Missing: dragon, wither, fight persistence, End progression, trial/ominous systems and related loot | C8 |
| V30 | Data-pack execution | Partial: recipes and block loot tables are read and checked, and their deterministic subset is used; functions/macros, predicates, the random loot functions, dynamic enchantments, advancements and programmable tests missing | C1, C5, C9 |
| V31 | Commands and administration | Partial: a few chat commands and `/gamerule` (game rules are world state; only randomTickSpeed and spawnChunkRadius have behavior); complete grammar/selectors, permissions, command blocks, scoreboard, teams, bans, whitelist and dedicated-server controls missing | C9 |
| V32 | Client-visible world state | Partial: chunks/players/items/windows; block-entity data, animations, equipment, sounds, particles, maps, signs, books, displays and boss bars incomplete | C10 |
| V33 | Communication and connection lifecycle | Partial: login/auth/compression/encryption/basic chat; signed chat, full configuration/reconfiguration, resource packs and remaining play packets incomplete | C10 |
| V34 | Save compatibility | Partial: crash-safe internal subset, 1.21.8 Anvil subset; full data/players/POI/entities/maps/raids/scoreboards, both DataVersions and migrations missing | Every phase, C11 |
| V35 | Version-specific gameplay and optional profiles | Mostly missing: actual behavior for the release-specific content and optional bundled features | Every phase, C12 |
| V36 | Integration, endurance and operating limits | Partial: focused regressions and synthetic profiles; full survival, machine, world-generation, multiplayer and upgrade qualification missing | C12 |

## Delivery sequence

All new milestones are open. Implement and validate both target versions in every milestone. A small shared vertical slice can be delivered before the rest of its family, but the family remains open. Dependencies describe technical prerequisites; they do not require separate branches or concurrent implementations.

| Milestone | Deliverable | Main prerequisites | Acceptance gate |
| --- | --- | --- | --- |
| **C0 — Baselines and evidence** | Exact release manifests, per-version feature ledger, expanded entity/RNG/lifecycle observations and reproducible reference runs | Existing harnesses and tests | Every family has an owner and known/unknown status; new observations reproduce on reference reruns; known differences are visible |
| **C1 — Complete data model** | Extensible state IDs, component-aware stacks/entities/block entities, versioned schemas, registry/tag/pack foundations | C0 | Every target registry entry is represented or explicitly still missing; no silent component loss; old saves/replays have a tested migration/refusal policy |
| **C2 — World foundations** | Dimension-scoped ownership, ticket levels, time/weather foundations, lighting and chunk lifecycle | C1 | Unload/reload, lighting and ticking transitions match per-version cases; identical coordinates in different dimensions stay isolated |
| **C3 — Entities and TNT** | General entity lifecycle and motion, item parity, non-living entities, baseline TNT/explosions | C0–C2; required block primitives | Entity observations, ignition paths, fuse/motion/explosion results and save/restart behavior match both targets |
| **C4 — Technical gameplay** | Missing redstone/automation components, rails/honey/coral, exact piston interactions and machine fixtures | C3; required C1/C2 primitives | Selected real machine layouts match vanilla over time, across chunks and after restarts; no special-case simulated outputs |
| **C5 — Blocks, items and production** | All block families, environmental interactions, loot, crafting/processing and functional menus/items | C1–C3; C4 automation integration | Family inventories close with placement/use/update/drop/menu and persistence evidence, including components |
| **C6 — Complete player gameplay** | Modes, movement, combat, equipment, effects, experience, food and progression interactions | C3, C5 | Real-client scenarios cover valid actions and corrections without rejecting valid vanilla motion or creating resources |
| **C7 — Worlds and dimensions** | Same-seed generation, all biomes/features/structures, presets, portals and travel | C2, C5; entity interfaces from C3 | Generated and imported worlds retain semantic state; seed and portal scenarios match each version at chunk boundaries |
| **C8 — Living world** | All mob families, AI/navigation, spawning/farms, villages/raids, bosses and release-specific creatures | C3, C5–C7 | Species/farm/progression cases match controlled reference runs; caps, RNG and order are verified |
| **C9 — Commands and data packs** | Command grammar/permissions, functions, selectors, advancements, scoreboards, gamerules and server operations | C1 plus the affected gameplay families | Version-specific command/data-pack suites reproduce effects, results, persistence and reload behavior |
| **C10 — Complete client integration** | Remaining packets, metadata, UI, audio/visual events, signed chat, packs, maps and dialogs/waypoints | Continuous integration with C1–C9 | Unmodified clients exercise every server-visible feature; packet presence alone does not pass |
| **C11 — Complete world interchange** | Both target save formats, full metadata/entities/players/POI, upgrades, backups and recovery tools | Storage support added in each earlier milestone | Semantic round trips on both official servers, no silent data loss, recovery through every supported migration |
| **C12 — Release qualification** | Full version/profile matrices, machine/farm and survival campaigns, soak/load and failure testing | C0–C11 | No mandatory missing/unknown/deviating feature; performance is measured with the same accepted semantics |

TNT is an early vertical slice through C0–C4, not a reason to omit the remaining game. Complete datapack execution and save interoperability are release requirements even when a normal survival session does not exercise them.

## Evidence and acceptance policy

Maintain implementation status separately from evidence status for each `(version, profile, feature, behavior case)`. Record sources, relevant schemas, fixtures, known gaps and the first failing observation. Equivalent behavior may share code; expected differences must have separate assertions.

Each feature's acceptance package must include:

1. Target-version expectations from documented contracts and controlled official-server observations, with exact inputs, initial world, gamerules, pack stack and timing.
2. A local reference rule and a parallel-runtime comparison, including 1/2/4/8/16 workers, adverse schedules, chunk edges/corners and ownership changes where relevant.
3. Player/protocol tests for publicly reachable actions, including delayed/batched input and correction behavior; real-client confirmation for display or interaction semantics.
4. Save/reload and interruption coverage for new state, plus explicit handling of old formats and cross-version data.
5. Normal boundary and invalid-input checks, the relevant sanitizers, and a coverage statement naming cases not exercised.

The current sequential oracle shares gameplay rules with the parallel engine. It is valuable for scheduling equivalence but cannot independently validate those shared rules. The external reference and hand-derived expectations remain required.

Trace v2 must observe entities, RNG streams/draw order where measurable, dimensions/ticket transitions, components, menus and significant intermediate events. Stable entity identities may be mapped across processes; gameplay differences must not be normalized away. Uncontrolled random behavior needs repeatable instrumentation or a declared statistical comparison, not a false exact-equality claim.

Technical-gameplay compatibility includes specified vanilla quirks such as TNT duplication when present in the selected version. Compare a named design and its causal behavior; do not substitute periodic spawns or assume every duplication technique works. Resource invariants must account for allowed rule-created entities while still detecting unintended loss or duplication.

Strict compatibility may serialize causally connected work. Preserve the vanilla order first, then demonstrate safe parallelism. Changes to global RNG, ticket levels, dimensions and entity ownership need explicit review; keeping worker threads busy is not an acceptance criterion.

## Release gates and progress reporting

Report progress per family and per target version: inventoried, represented, behavioral implementation, official-reference comparison, client integration, persistence and parallel qualification. Do not combine these into one unweighted percentage.

Before an all-features release:

- The stable-release inventory is closed for both versions, including administrative and command-only features. Optional profiles have explicit outcomes and are not conflated with default behavior.
- All required scenarios pass for each version. No unknown or declared gameplay deviation remains inside an exact-compatibility claim; exceptions must narrow the published claim.
- TNT/technical builds, survival progression, entity-heavy farms, fresh exploration and imported long-lived worlds survive restart and prolonged multiplayer use.
- Remaining L2 rows are resolved or the release is still labelled partial. The existing 60 trace files alone cannot satisfy this gate.
- All new state participates in persistence, replay/diagnostics and version handling. Lossy import is never the path used to demonstrate full compatibility.
- Performance and resource limits are published for accepted behavior, with tick-time percentiles, memory, I/O and network costs; connected machines may remain serial.

The existing foundation is reusable. The remaining work is a full compatibility program spanning data models, missing systems and evidence, rather than a final round of testing on an otherwise complete vanilla server.
