# Network server

## Status

Circuit runs a playable creative-mode world for Java Edition 1.21.8 (protocol 772) on top of the headless engine. The world is a flat stone floor at y = −61. The real 1.21.8 client can join in offline and online mode, build with the reference blocks, use levers, repeaters and comparators, and watch redstone, pistons, observers, hoppers, water and item entities run under the reference rules. Several players see each other and chat.

Persistence, survival gameplay and containers are not implemented yet; see [Not supported](#not-supported).

## Running

```bash
xmake run circuit-server --serve --offline --port 25565
xmake run circuit-server --serve --config "$PWD/server.conf"
```

`--offline` sets `online-mode=false`. The server stops on SIGINT or SIGTERM. When it stops, it disconnects every player with a reason and gives their connections up to two seconds to drain.

The configuration file holds `key=value` lines; `#` starts a comment. Unknown keys and invalid values are errors.

| Key | Default | Meaning |
| --- | --- | --- |
| `bind-address`, `port` | `0.0.0.0`, `25565` | Listening address |
| `game-version` | `java-1.21.8` | Version bundle; only bundles with a play protocol can serve |
| `online-mode` | `true` | Require session verification. See [Identity](#identity) |
| `session-server` | `https://sessionserver.mojang.com` | Session service for online mode; plain `http` only on loopback |
| `auth-timeout-ms` | `5000` | Deadline for one verification |
| `max-players`, `max-connections` | `20`, `256` | Players in the game; open sockets of any kind |
| `view-distance`, `simulation-distance` | `6`, `4` | Chunks sent around a player; ticking radius of the player's chunk ticket |
| `compression-threshold` | `256` | Packets of at least this size are compressed; `-1` disables compression |
| `network-threads`, `workers` | `2`, `4` | Asio threads for connections; engine worker threads |
| `login-timeout`, `keep-alive-timeout` | `30`, `30` | Seconds |
| `max-queued-bytes` | 16 MiB | Unsent data per connection before it is closed as too slow |
| `motd`, `seed`, `floor-y` | | Status text, world seed, height of the flat floor |

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
| Registry entries and their data | minecraft.wiki pages cited in `src/server/registries.cpp` | by hand |
| Field layouts | minecraft.wiki protocol pages at the same revisions | by hand, in `src/server/wire.cpp` |

Two consistency checks guard these tables:
- **Packet ids:** where the two public sources name the same packet (83 packets), their ids agree, and per state and direction they list the same number of packets for 1.21.8.
- **Block states:** every block's state count matches its properties, and every state id round-trips through its notation.

The real client exercises the rest.

Clients that report the vanilla data pack receive registry entry names only. Clients that do not, such as protocol libraries, receive a documented minimal set with data: overworld, plains and the `chat` chat type.

## Not supported

- Survival gameplay: health, hunger, damage, mining times and drops from players, mobs, and crafting.
- Container screens: chests and hoppers cannot be opened. Dropping items from the inventory is not supported.
- Lighting: everything is fully lit by sky light. World generation is a flat floor. Persistence comes with M5.
- Signed chat: chat is relayed as system messages. Commands are `/help`, `/players`, `/tp`, `/gamemode creative|spectator` and `/tps`, with no command-tree suggestions.
- Display gaps: block entity data is not sent to clients, so moving pistons are not animated. Other blocks and entities are out of scope.
- Other versions: 1.21.4 support is planned in M8.

## Testing

| Check | What it covers |
| --- | --- |
| `protocol_tests` | Data types against documented examples, NBT, framing and compression limits, packet and content tables, fuzzed input |
| `server_tests` | An in-process server and a small client built on Circuit's codec. It covers: status; offline and online login (encryption and a mock session service); default deny; version, name and frame errors; connection, rate and login-time limits; movement and reach; placement; a lever lighting a lamp; graceful stop. |

Two clients outside this repository give independent evidence:
- **Protocol library:** a mineflayer suite, run against both Circuit and the vanilla server.
- **Real client:** a Fabric client mod drives the real 1.21.8 client from step scripts and checks the blocks and entities the client shows.

The [verification guide](DEVELOPMENT.md#client-testing) describes how to run them.
