# Network server

## Status

Circuit runs a playable world for Java Edition 1.21.8 (protocol 772) or 1.21.4 (protocol 769) on top of the headless engine. One server runs one version, chosen with `game-version`. The world is a flat stone floor at y = −61. The real client of either version can join in offline and online mode, build with the reference blocks, use levers, repeaters and comparators, and watch redstone, pistons, observers, hoppers, water and item entities run under the reference rules. Several players see each other and chat. Native modules and Java plugins can extend it through controlled services; see [the plugin API](PLUGIN_API.md).

Players play in creative or survival mode. In survival, placing uses up items, breaking takes the block's dig time and drops what the tool harvests, items are thrown and picked up, and chests, double chests and hoppers open with every click mode. Movement is checked against block shapes; players get hungry, take fall and void damage, die, drop their inventory and respawn. Inventories are simulation state; see [Survival and inventories](#survival-and-inventories).

With a world directory, the world is saved with checkpoints and an input log and survives crashes; Java Edition 1.21.8 worlds can be imported and exported. See [persistence](PERSISTENCE.md). Mobs, eating and crafting are not implemented; see [Not supported](#not-supported).

## Running

```bash
xmake run circuit-server --serve --offline --port 25565 --world "$PWD/worlds/main"
xmake run circuit-server --serve --config "$PWD/server.conf"
```

`--offline` sets `online-mode=false`, and `--world` sets `world-directory`. The server stops on SIGINT or SIGTERM. When it stops, it saves the world, disconnects every player with a reason and gives their connections up to two seconds to drain. It also stops by itself, with a non-zero exit status, when the world cannot be written.

The configuration file holds `key=value` lines; `#` starts a comment. Unknown keys and invalid values are errors.

| Key | Default | Meaning |
| --- | --- | --- |
| `bind-address`, `port` | `0.0.0.0`, `25565` | Listening address |
| `game-version` | `java-1.21.8` | Version bundle: `java-1.21.8` or `java-1.21.4`. Clients of another protocol are refused at login |
| `online-mode` | `true` | Require session verification. See [Identity](#identity) |
| `allow-flight`, `difficulty` | `false`, `easy` | Whether survival players may hover; `peaceful`, `easy`, `normal` or `hard` (how far players starve, and peaceful regeneration) |
| `session-server` | `https://sessionserver.mojang.com` | Session service for online mode; plain `http` only on loopback |
| `auth-timeout-ms` | `5000` | Deadline for one verification |
| `max-players`, `max-connections` | `20`, `256` | Players in the game; open sockets of any kind |
| `view-distance`, `simulation-distance` | `6`, `4` | Chunks loaded and sent around a player; chunks simulated around a player (at most 8) |
| `pause-when-empty-seconds` | `60` | Seconds without players after which the world stops ticking until a player joins; `0` or less never pauses |
| `compression-threshold` | `256` | Packets of at least this size are compressed; `-1` disables compression |
| `network-threads`, `workers` | `2`, `4` | Asio threads for connections; engine worker threads |
| `login-timeout`, `keep-alive-timeout` | `30`, `30` | Seconds |
| `max-queued-bytes` | 16 MiB | Unsent data per connection before it is closed as too slow |
| `motd`, `seed`, `floor-y` | | Status text; seed and flat floor height of a new world (a saved world keeps its own) |
| `world-directory`, `autosave-ticks`, `log-sync-ticks` | none, `6000`, `20` | Saved world and its checkpoint and log-sync intervals; see [persistence](PERSISTENCE.md) |
| `plugin`, `plugin-permissions.<id>` | none | A native module to load (repeatable) and its permissions; see [the plugin API](PLUGIN_API.md) |
| `java-bridge`, `java`, `java-plugin-class`, `java-plugins`, `java-permissions`, `java-max-lag-ticks` | none, `java`, none, none, `world.read`, `200` | The Java bridge; see [its guide](../modules/java-bridge/README.md) |

Setting the environment variable `CIRCUIT_TRACE_PACKETS=1` logs every serverbound play packet.

## Design

**Threads.** Each connection runs on an Asio strand. Its protocol state machine covers handshake, status, login, configuration and play, with framing, compression and encryption.

The game runs on one thread at 20 ticks per second, using the engine's tick clock:
- **Inputs:** network threads only enqueue joins, decoded intents and departures. The game applies them at the start of the next tick, in arrival order.
- **Tick:** the game turns the inputs into engine commands and advances the engine once.
- **Output:** it then publishes block changes, entity movement and chunk views to each player.

Players never touch engine state directly.

**Authority.** Every player action is validated before it becomes a command:
- **Movement:** finite coordinates inside the world border; at most 10 blocks per packet; only into delivered chunks; checked against block shapes (see [Movement and vitals](#movement-and-vitals)). Refused moves are corrected with a teleport, and non-finite ones are kicked.
- **Reach:** blocks within the interaction range plus 1 of the eyes, measured to the nearest point of the block: 6 blocks in creative (range 5), 5.5 in survival (range 4.5). An open container stays usable up to 4 blocks further.
- **Inventory:** creative slots 0–45 with known items, at most the stack's size, with components that pass the checks of [item components](COMPATIBILITY.md#content); a refused stack is logged and the whole window is sent again. Clicks only in the window the player has open.

**Block actions.** Clicks and creative breaking become the commands `PlayerUse` and `PlayerBreak`. The rules resolve them when they are applied, in admission order, against the state the earlier commands left. A player's actions therefore mean the same however they fall into ticks: a block placed and then built on, or a lever placed and then used, gives the same world whether the packets arrive in one tick or several. Admission checks only what the world does not decide:
- the held item's block state, oriented by the player and the clicked face;
- which of the two possible positions, the clicked block or its neighbor, is within reach and free of players.

When the command is applied:
- **Use:** a lever, repeater or comparator is used, unless the player sneaks while holding an item.
- **Placement:**
  - the block replaces the clicked block if that is air or water, otherwise it goes next to it;
  - the target must be air or water;
  - dust, repeaters and comparators need a solid block below, and levers a solid block to attach to;
  - a chest joins a neighboring single chest as the official server does: next to one of the same facing, clockwise first, unless the player sneaks; when sneaking, the chest whose side was clicked, taking its facing.
- **Breaking:** any block but air and moving pistons; a piston head takes its base along.

A refused action is answered with the current state of the affected blocks, or, for a command refused when applied, restored by the client when the action is acknowledged. Every block action is acknowledged with its sequence number.

**World delivery.**
- **Chunks:** encoded from copy-on-write column snapshots: paletted block sections, plains biome, heightmaps and full sky light, with no lighting engine yet. At most 32 chunks per player per tick, nearest first.
- **View and simulation:** as in the target versions, each player has two chunk tickets. A loading ticket of level 31 over the view distance the client asked for (at most `view-distance`) loads those chunks, and two rings beyond them at levels 32 and 33, without simulating them. A simulation ticket of level 31 − `simulation-distance` at the player's chunk simulates what is loaded: entities within the simulation distance, blocks one chunk further. Every chunk of the view is sent, and chunks one beyond it stay shown while loaded. The spawn chunks keep a start ticket of level 30 (spawnChunkRadius 2). See [chunk levels](SIMULATION.md#reference-rules-v9).
- **Block states:** every displayed property is rule state, including dust shapes, lever mounting and repeater locks, so clients see the state the rules hold.
- **Updates and entities:** block changes in a tick become block updates for every player who has the chunk. Item entities are spawned, moved and removed from the engine trace.

## Survival and inventories

`/forceload add <x> <z> [<x> <z>]`, `/forceload remove <x> <z> [<x> <z>]`, `/forceload remove all` and `/forceload query [<x> <z>]` mark chunks of the overworld as force-loaded, as in the target versions: block coordinates (`~` for the player's own) name the chunks, at most 256 per command, and feedback uses the official translation keys. Each marked chunk is a forced ticket of level 31 that persists in the world. `/gamemode survival|creative|spectator` switches modes. Inventories belong to the simulation: every change is a command the rules apply (see [the simulation guide](SIMULATION.md#tick-structure)), so it is logged, replayed and checked for item conservation like the world.

- **Placement:** uses up the item; a water bucket leaves an empty bucket.
- **Digging:** the server follows the target version's destroy progress for the block, the held tool, the ground and water. A finish is accepted once the progress since the start reaches 70 % (network delay); one that comes too early completes when the progress reaches the whole block. A finish for another block, or without a start, is refused.
- **Drops and tools:** a broken block drops what the tool harvests, and a container drops its contents. With the official data pack, blocks without rules of their own drop from their loot tables when these draw no random numbers (silk touch included). Tools wear by one per block and break at their durability.
- **Crafting:** with the official data pack, the player's 2x2 grid crafts with the pack's shaped, shapeless and transmute recipes, with every click mode; the external `craft-loot.js` scenario gives the same windows as both official servers.
- **Items:** `Q` throws one item from the selected slot and `Ctrl+Q` the stack, from the eyes along the look. Players pick up item entities whose box touches theirs, grown by one block sideways and half a block up and down, once their pickup delay has run out; the client shows the pickup.
- **Windows:** using a chest, a double chest or a hopper opens a `generic_9x3`, `generic_9x6` or `hopper` window, unless a solid block lies above the chest. All seven click modes work in it and in the player's own window. Windows close when the container is removed or changes size, or the player moves out of range; closing returns the carried stack and the crafting grid.
- **Synchronization:** as the target version does, the server keeps the client's view of its window. It takes the slots and carried stack the client predicted for a click as that view, and after each tick sends only the slots that differ. A click made on a stale state id, or one whose prediction cannot be read, gets the whole window. Protocol 772 predicts stacks with hashed components, which the server does not compute: a predicted slot that holds components is sent again. Creative slot changes are not echoed.

The clicks were calibrated against the official 1.21.8 and 1.21.4 servers. The same 48-step mineflayer scenario gives identical windows, carried stacks and inventories on both servers and on Circuit. It covers every click mode, double chests, hoppers, the own window, thrown items and pickups. A second scenario, `craft-loot.js`, crafts in the own window (48 steps) and breaks 23 blocks with various tools on both official servers and on Circuit with the official data pack. The crafting windows are identical. The drops are identical wherever the block's loot table draws no random numbers.

### Movement and vitals

The server follows the target version's movement handling for every player but spectators:
- **Collision:** a claimed move is replayed from the last accepted position against the collision shapes of every block state (compiled from minecraft-data at the pinned commit by `tools/compile_collision_shapes.py`). The vertical axis moves first, then the horizontal axis with the larger movement, and a player on the ground steps up to 0.6 blocks.
- **Refused moves:** in survival, a move that ends more than 0.25 blocks away horizontally from the replayed one is refused, unless the player was already stuck in a block. In any mode, a move into a block the player was not already in is refused. Vertical differences are not compared, as in the target version.
- **Floating:** a survival player who neither sinks nor stands on or next to anything for 80 ticks is kicked with the client's "Flying is not enabled" message, unless `allow-flight` is set.
- **Falls:** the distance fallen since the ground, without the move that lands, deals one point per block beyond 3. Water and flying reset it; a slime block cancels the damage unless the player sneaks.
- **Hunger:** food, saturation and exhaustion follow the documented rules: jumps (0.05, sprinting 0.2), sprinting on the ground (0.1 per metre), moving through water (0.01 per metre), breaking blocks (0.005) and healing tire players in survival. Healing comes from saturation with full food (saturation/6 every 10 ticks) or from food 18 or more (one point every 80 ticks), in every game mode; without food players starve down to the difficulty's limit. Eating is not modeled, so food only goes down.
- **Damage:** fall and starvation damage only survival players; the void (more than 64 blocks below the world) deals 4 points to every player but spectators. A hurt cooldown of 20 ticks lets only stronger damage through in its last 10 ticks.
- **Death and respawn:** at zero health the client shows the death screen with the target version's message (a translation key), everyone reads it, and the inventory drops (a `PlayerDeath` command; items dropped below the world are lost). Asking to respawn brings the player back at the world spawn with full health and hunger.

The client is told health, food and saturation when they change, as the target version does. Two black-box scenarios (`vitals.js` and `collision.js` in the external client tests) give identical results on the official 1.21.8 and 1.21.4 servers and on Circuit: a wall correction, fall damage, regeneration with draining saturation, a deadly fall with its drops, respawning, void damage against regeneration, the death messages, the floating kick, and moves against chests, repeaters, a hopper's bowl and full blocks.

## Identity

`online-mode=true` performs the documented key exchange: a 1024-bit RSA key, AES-128/CFB8 in both directions, and the login hash. The server then asks the session service whether the player joined.
- **Verified:** the profile id, name and properties (skins) come from the service.
- **Default deny:** when the service does not confirm the join, cannot be reached in time, or answers malformed data, the login is refused.
- **TLS:** certificates and host names are verified.

`online-mode=false` admits offline identities: the name-based UUID of `OfflinePlayer:<name>`, labelled unverified. The log states which kind each player has. Offline mode does not exempt an operator from the Minecraft terms.

## Protocol data

Numeric protocol data is compiled from public documentation, and every table names its source.

| Data | Source | Tool |
| --- | --- | --- |
| Packet ids | minecraft.wiki "Java Edition protocol/Packets", revision 3258009 (1.21.8) and 2938097 (1.21.4) | `tools/extract_packet_ids.py` |
| Block states, items, entity types | PrismarineJS minecraft-data (MIT) at commit `095709f3`, which the wiki's protocol FAQ points to | `tools/compile_content_tables.py` |
| Registry entries, their data and the biome tags 1.21.4 needs | minecraft.wiki pages cited in `src/server/registries.cpp` | by hand |
| Item component schema and static registry sizes | PrismarineJS minecraft-data `protocol.json` and registry tables at the same commit | `tools/compile_components.py` |
| Game rules | minecraft.wiki "Game rule" revision 3099622 | by hand, in `src/contracts/version.cpp` |
| Field layouts | minecraft.wiki protocol pages at the same revisions | by hand, in `src/server/wire.cpp` |

Two consistency checks guard these tables:
- **Packet ids:** where the two public sources name the same packet (83 packets), their ids agree. Per state and direction they list the same number of packets for 1.21.8.
- **1.21.4 packet count:** minecraft-data lists two more serverbound configuration packets for 1.21.4, `custom_report_details` and `server_links`. These are clientbound packets in the wiki's tables, so the server follows the wiki. The eight ids both sources share agree.
- **Block states:** every block's state count matches its properties, and every state id round-trips through its notation.
- **Registries and game rules:** every synchronized registry has the same entries as the official server's configuration, and each bundle's game rules are the official server's `/gamerule` list (`tests/fixtures/registries/`, observed 2026-10-10). The check found the four element paintings missing from the painting table, now added. The official servers also send registries Circuit does not: enchantments (their definitions need item and enchantment tags), and in 1.21.8 dialogs and the game-test registries.

**1.21.4 differences.** The 1.21.4 wire protocol differs from 1.21.8 in what the server uses:
- **Chunk data:** heightmaps are an NBT compound, and paletted containers carry their data-array length.
- **Registries:** the cat, chicken, cow, frog, pig and wolf-sound variants are not synchronized, "Dennis" is not yet a painting, and the jukebox songs lack Tears and Lava Chicken.
- **Tags:** its wolf variants need the `is_badlands`, `is_jungle` and `is_savanna` biome tags, which the server sends.

The real client exercises the rest.

Each version bundle names its registry set (`circuit-server --compatibility` prints it). Clients that report the vanilla data pack receive registry entry names only: 17 registries in 1.21.8 and 11 in 1.21.4, including trim materials and patterns, banner patterns, goat-horn instruments and jukebox songs, whose definitions refer to no tags. With the official jar's data pack (`vanilla-server-jar`, see [data packs](DATA_PACKS.md)) they also receive the enchantments and every tag of the registries Circuit numbers. Clients that do not, such as protocol libraries, receive a documented minimal set with data: overworld, plains and the `chat` chat type.

## Not supported

- Mobs, combat between players, armor protection, and experience.
- The crafting table, special recipes (repairing, cloning, fireworks, dyeing armor), the recipe book, and crafting without the official data pack.
- Item use other than placement (eating, buckets of other fluids, tools on blocks) and item cooldowns.
- Respawning picks the exact world spawn; the target version picks a random position around it. Death drops spread in a fixed pattern instead of at random.
- Swimming and crawling poses, climbing, elytra flight and vehicles: their movement is checked like walking.
- Lighting: everything is fully lit by sky light. World generation is a flat floor.
- Saving is limited to what [persistence](PERSISTENCE.md) describes. Checkpoints are written in the background; the game thread takes a snapshot and syncs the start of a new log segment.
- Signed chat: chat is relayed as system messages. Commands are `/help`, `/players`, `/tp`, `/gamemode survival|creative|spectator`, `/forceload` and `/tps`, with no command-tree suggestions and no permission levels.
- Display gaps: block entity data is not sent to clients, so moving pistons are not animated. Other blocks and entities are out of scope.
- **Other versions:** 1.21.8 and 1.21.4 only. Anvil import and export are defined for 1.21.8 worlds only. A world directory belongs to one bundle; `--migrate-world` copies it to another bundle when both run the same rules.

## Testing

| Check | What it covers |
| --- | --- |
| `protocol_tests` | Data types against documented examples, NBT, framing and compression limits, packet and content tables, fuzzed input |
| `server_tests` | An in-process server and a small client built on Circuit's codec. It covers: status; offline and online login (encryption and a mock session service); default deny; version, name and frame errors; connection, rate and login-time limits; movement and reach; placement, with player actions grouped into ticks in different ways and chest pairing; survival placement, digging time, drops, pickups and item conservation; container windows and their incremental synchronization; movement against block shapes with step-up, the floating kick, falls, regeneration, death, respawn and the void; view loading without ticking; a lever lighting a lamp; a client that stops reading; one owner per world; graceful stop; saving and restoring a world and a player; the server binary killed and recovered. |

Two clients outside this repository give independent evidence:
- **Protocol library:** mineflayer suites run against both Circuit and the vanilla server: building and redstone, chest placement, container windows, vitals and collision.
- **Real client:** a Fabric client mod drives the real 1.21.8 or 1.21.4 client from step scripts and checks the blocks and entities the client shows.

The [verification guide](DEVELOPMENT.md#client-testing) describes how to run them.
