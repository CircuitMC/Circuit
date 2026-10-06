# Network server

## Status

Circuit runs a playable creative-mode world for Java Edition 1.21.8 (protocol 772) or 1.21.4 (protocol 769) on top of the headless engine. One server runs one version, chosen with `game-version`. The world is a flat stone floor at y = −61. The real client of either version can join in offline and online mode, build with the reference blocks, use levers, repeaters and comparators, and watch redstone, pistons, observers, hoppers, water and item entities run under the reference rules. Several players see each other and chat. Native modules and Java plugins can extend it through controlled services; see [the plugin API](PLUGIN_API.md).

With a world directory, the world is saved with checkpoints and an input log and survives crashes; Java Edition 1.21.8 worlds can be imported and exported. See [persistence](PERSISTENCE.md). Survival gameplay and containers are not implemented yet; see [Not supported](#not-supported).

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
| `session-server` | `https://sessionserver.mojang.com` | Session service for online mode; plain `http` only on loopback |
| `auth-timeout-ms` | `5000` | Deadline for one verification |
| `max-players`, `max-connections` | `20`, `256` | Players in the game; open sockets of any kind |
| `view-distance`, `simulation-distance` | `6`, `4` | Chunks sent around a player; ticking radius of the player's chunk ticket |
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
- **Movement:** finite coordinates inside the world border; at most 10 blocks per packet; only into delivered chunks. Too-fast moves are corrected with a teleport, and non-finite ones are kicked.
- **Reach:** blocks within 6 blocks of the eyes, measured to the nearest point of the block (creative interaction range 5 plus 1).
- **Placement:**
  - the target must be empty or water, outside every player;
  - dust, repeaters and comparators need a solid block below, and levers a solid block to attach to.
- **Inventory:** creative slots 0–45 with known items and at most 99 per stack.

A refused action is answered with the current state of the affected blocks, and every block action is acknowledged with its sequence number.

**World delivery.**
- **Chunks:** encoded from copy-on-write column snapshots: paletted block sections, plains biome, heightmaps and full sky light, with no lighting engine yet. At most 32 chunks per player per tick, nearest first.
- **Display properties:** dust connections and similar properties are derived for display only and never feed back into the rules.
- **Updates and entities:** block changes in a tick become block updates for every player who has the chunk. Item entities are spawned, moved and removed from the engine trace.

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
| Field layouts | minecraft.wiki protocol pages at the same revisions | by hand, in `src/server/wire.cpp` |

Two consistency checks guard these tables:
- **Packet ids:** where the two public sources name the same packet (83 packets), their ids agree. Per state and direction they list the same number of packets for 1.21.8.
- **1.21.4 packet count:** minecraft-data lists two more serverbound configuration packets for 1.21.4, `custom_report_details` and `server_links`. These are clientbound packets in the wiki's tables, so the server follows the wiki. The eight ids both sources share agree.
- **Block states:** every block's state count matches its properties, and every state id round-trips through its notation.

**1.21.4 differences.** The 1.21.4 wire protocol differs from 1.21.8 in what the server uses:
- **Chunk data:** heightmaps are an NBT compound, and paletted containers carry their data-array length.
- **Registries:** the cat, chicken, cow, frog, pig and wolf-sound variants are not synchronized, and "Dennis" is not yet a painting.
- **Tags:** its wolf variants need the `is_badlands`, `is_jungle` and `is_savanna` biome tags, which the server sends.

The real client exercises the rest.

Clients that report the vanilla data pack receive registry entry names only. Clients that do not, such as protocol libraries, receive a documented minimal set with data: overworld, plains and the `chat` chat type.

## Not supported

- Survival gameplay: health, hunger, damage, mining times and drops from players, mobs, and crafting.
- Container screens: chests and hoppers cannot be opened. Dropping items from the inventory is not supported.
- Lighting: everything is fully lit by sky light. World generation is a flat floor.
- Saving is limited to what [persistence](PERSISTENCE.md) describes. Checkpoints are written in the background; the game thread takes a snapshot and syncs the start of a new log segment.
- Signed chat: chat is relayed as system messages. Commands are `/help`, `/players`, `/tp`, `/gamemode creative|spectator` and `/tps`, with no command-tree suggestions.
- Display gaps: block entity data is not sent to clients, so moving pistons are not animated. Other blocks and entities are out of scope.
- **Other versions:** 1.21.8 and 1.21.4 only. Anvil import and export are defined for 1.21.8 worlds only. A world directory belongs to one bundle; `--migrate-world` copies it to another bundle when both run the same rules.

## Testing

| Check | What it covers |
| --- | --- |
| `protocol_tests` | Data types against documented examples, NBT, framing and compression limits, packet and content tables, fuzzed input |
| `server_tests` | An in-process server and a small client built on Circuit's codec. It covers: status; offline and online login (encryption and a mock session service); default deny; version, name and frame errors; connection, rate and login-time limits; movement and reach; placement; a lever lighting a lamp; graceful stop; saving and restoring a world and a player; the server binary killed and recovered. |

Two clients outside this repository give independent evidence:
- **Protocol library:** a mineflayer suite, run against both Circuit and the vanilla server.
- **Real client:** a Fabric client mod drives the real 1.21.8 or 1.21.4 client from step scripts and checks the blocks and entities the client shows.

The [verification guide](DEVELOPMENT.md#client-testing) describes how to run them.
