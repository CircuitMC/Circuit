# Persistence

## Status

The network server saves its world in a world directory: checkpoints of the engine state plus a write-ahead log of the inputs admitted after them. After a crash, the server restarts from the latest durable checkpoint and replays the logged inputs, so it continues exactly where the durable history ends.

Java Edition 1.21.8 worlds in the Anvil format can be imported into a world directory and exported from one; a `java-1.21.4` server refuses Anvil interchange. Import and export cover the content the reference rules model. Other content is rejected unless the substitution policy is chosen explicitly; see [Anvil import and export](#anvil-import-and-export).

## World directory

Set `world-directory` in the server configuration, or pass `--world <dir>`:

```bash
xmake run circuit-server --serve --offline --world "$PWD/worlds/main"
```

Without a world directory the world lives in memory only, and the server says so at startup.

| Key | Default | Meaning |
| --- | --- | --- |
| `world-directory` | none | Directory of the saved world |
| `autosave-ticks` | `6000` | Ticks between checkpoints (5 minutes) |
| `log-sync-ticks` | `20` | Input log records are synced to disk every this many ticks; `1` syncs every tick |

A crash loses at most the inputs of the ticks since the last log sync. Every input that was synced is recovered, including the ticks after the last checkpoint.

The directory holds:

| File | Content |
| --- | --- |
| `manifest` | The current checkpoint, the previous one and the log segments after the previous one, with SHA-256 digests of the checkpoints |
| `ckpt-<tick>-<generation>.bin` | A checkpoint: the canonical engine state at the end of `<tick>` |
| `wal-<tick>-<generation>.log` | Log segment: one record per tick after `<tick>`, each with its length and CRC-32 |

Each checkpoint has a new generation number, so no file that the durable manifest names is ever rewritten.

### Migrating between versions

A world directory records its bundle and rule set, and a server of another bundle refuses it. To move a world to another version:

```bash
xmake run circuit-server --migrate-world "$PWD/worlds/main-1.21.4" --world "$PWD/worlds/main" --game-version java-1.21.4
```

The migration reads the latest durable state, replays its log, and writes a new world directory for the target bundle; the source is only read. It is refused when the two bundles run different rule sets, because the saved state would not mean the same.

## Durability protocol

Each tick, the server logs the tick's input before the engine applies it. Every `log-sync-ticks` ticks the record is synced before the tick runs. A change that players saw is therefore either in the durable log or after the last sync.

A checkpoint goes through these steps:
1. Sync the current log segment, so that the previous checkpoint plus the log reach the new checkpoint.
2. Write the checkpoint to a temporary file, sync it and rename it.
3. Create the new, empty log segment and sync the directory.
4. Write the new manifest to a temporary file, sync it, rename it over `manifest` and sync the directory.
5. Remove files the new manifest no longer names.

The new checkpoint becomes current at the rename in step 4. A crash before that point leaves the old manifest, which still names complete files; a crash after it leaves the new one. Recovery never sees a mixture of the two.

Checkpoints are written on the game thread. The server writes one at startup, every `autosave-ticks` ticks and when it stops.

## Recovery

At startup the server reads the manifest:
1. **Checkpoint:** it loads the current checkpoint and checks its digest. If the current checkpoint is unreadable or damaged, it uses the previous one and replays the longer log; the log names this fallback.
2. **Log replay:** it replays the log records after the checkpoint in order, up to the first incomplete or damaged record. A torn record at the end of a segment is expected after a crash and is discarded, with a log note. A later segment that a checkpoint started at the last complete record continues the history.
3. **Fresh segment:** it writes a checkpoint of the recovered state before accepting players, so that new records never follow a discarded tail.

Recovery fails, and the server does not start, when:
- the manifest names a world saved by another bundle or rule set;
- no named checkpoint is readable;
- a log record has a valid checksum but cannot be decoded, which means the log is corrupt rather than torn.

An input that the engine cannot apply during replay ends the history before it. The server logs the tick and continues from the last good one.

Files from which recovery discarded data are not deleted: a damaged checkpoint, or a segment with a discarded tail. Once no manifest needs them, they are renamed to `damaged-<name>` and kept for diagnosis. Delete them when they are no longer needed.

Players are not connected after a restart: their chunk tickets are removed in the first tick. Player positions, game modes and inventories are saved with each checkpoint and when a player leaves. A crash can lose player data since the last checkpoint, but never logged world changes.

## Failure handling

The server fails closed on storage errors. If a log record cannot be written, the server does not run the tick. It tries to write a checkpoint of the state before the tick, disconnects every player and stops. The same happens when an autosave fails. `circuit-server --serve` then exits with a non-zero status, and the next start recovers the last durable state.

## Checkpoint format

A checkpoint file is `CIRCKPT\0`, a format version, the bundle and rule-set ids, the seed and tick, the raw and compressed body sizes, the zlib-compressed body, and a SHA-256 digest of everything before it. The body holds:
- columns with palette-packed sections;
- block entities, entities, scheduled ticks and pending block events;
- chunk tickets and the engine counters;
- named extras, such as the server's metadata and player data.

Decoding checks every length, count and the digest. The engine's restoring constructor then checks the semantics: positions inside known columns, valid block states, and no duplicate ids. A restored engine continues exactly like the one that produced the checkpoint.

## Anvil import and export

```bash
xmake run circuit-server --import-anvil /path/to/java-world --world "$PWD/worlds/imported"
xmake run circuit-server --export-anvil /path/to/new-java-world --world "$PWD/worlds/imported"
```

Import reads the source world and never writes to it. It creates a new world directory and refuses one that already holds a world. Export reads the latest durable state of a world directory and never writes to it, so it can run while the server uses the directory; if the server replaces the checkpoint during the export, the export fails and can be repeated. It writes a new world to a temporary sibling directory and renames it at the end, and it refuses a target that exists.

**Supported.** A 1.21.8 world (DataVersion 4440), overworld only:

| Kind | Supported content |
| --- | --- |
| Blocks | The blocks the reference rules model: air, stone, glass, obsidian, redstone block, slime block, lever, redstone dust, repeater, comparator, redstone lamp, observer, pistons and their heads, moving pistons, hopper, chest, water and wheat |
| Block entities | Chests and hoppers with their items (the reference items), the hopper transfer cooldown, comparator output signals and moving pistons |
| Entities | Item entities |
| Scheduled ticks | Block and fluid ticks of supported blocks, with their delay and priority |
| Level data | Time, seed, spawn point and level name |

Display properties that the rules do not model, such as dust connections and lever mounting, are derived again on export.

**Unsupported content and policies.** Anything else is unsupported content, for example:
- other blocks, block entities and entities, and items with data components;
- block entities with custom names, loot tables or locks;
- the Nether and the End;
- chunks that still have an older DataVersion because the game has not re-saved them.

By default, an import that meets unsupported content imports nothing and lists every kind with its count. With `--substitute-unsupported`, the import goes ahead:
- **Blocks** are replaced by a stand-in of the same coarse shape: air for blocks without collision, obsidian for unbreakable blocks, stone for full light-blocking solids and glass for other solids.
- **Other content** is dropped: block entities, items and entities.

The report lists every replacement. Substitution changes behavior: for example, a torch becomes air and a slab becomes glass. A substituted world is therefore not evidence about the original machine.

**Not kept.** Import keeps none of the following:
- light, heightmaps and biomes;
- structures and pending post-processing;
- player data, maps, raids, scoreboards and the world border;
- the order of creation of block entities and entities in the source world. Circuit orders them by chunk position.

Positions and motion are stored in 1/4096-block steps. Item ages that never expire age normally. Export writes the following and leaves light and heightmaps to the game:
- `level.dat`, with a superflat generator that matches the server's floor height;
- region files, written with zlib compression, and entity region files;
- `minecraft:plains` as the biome of every section.

Unsupported compression schemes (LZ4 and custom) are refused with an error.

**Sources.** The formats come from minecraft.wiki revisions of August 2025 that describe 1.21.8:

| Page | Revision |
| --- | --- |
| Region file format | 3126113 |
| Chunk format | 3100914 |
| Entity format | 3118862 |
| Block entity format | 3131994 |
| Java Edition level format | 3110638 |
| Data component format | 3135677 |

Block data sections come from the Chest (3135926), Hopper (3131269), Redstone Comparator (3132038) and Piston/Technical components (3102833) pages. The coarse block shapes used for substitution come from the minecraft-data block table, like the protocol content tables; see [the network server](SERVER.md#protocol-data).

## Testing

| Check | What it covers |
| --- | --- |
| `storage_tests` | Includes crash-injection checks: <ul><li>checkpoint round trips of every scenario state, rejection of every flipped byte and truncation, and fuzzed bodies with valid digests;</li><li>the world directory: recovery, previous-checkpoint fallback, torn and damaged log tails;</li><li>a crash at every file operation of three logging policies, with and without torn unsynced data, each recovered and continued;</li><li>a failure at every operation, including short writes.</li></ul> |
| `anvil_tests` | <ul><li>export and import of 54 scenario states, with scheduled ticks, item entities and moving pistons;</li><li>both policies on hand-made worlds;</li><li>malformed and damaged region files;</li><li>the world tools.</li></ul> |
| `server_tests` | <ul><li>a world saved and restored with a player's position;</li><li>the real server binary killed with `SIGKILL` while a player acts, recovered with every synced change;</li><li>a world of other rules refused.</li></ul> |
| `simulation_tests` | Engines restored from a dump continue exactly, for every scenario at four split points |

The official 1.21.8 server also serves as a black-box check of the Anvil code. It runs outside this repository:
- **Export:** it loads exported worlds with ticking frozen and re-saves them, and the saved blocks, block entities, scheduled ticks and item entities are compared with the export.
- **Import:** worlds it builds are imported and compared.
