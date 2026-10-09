# Persistence

## Status

The network server saves its world in a world directory: checkpoints of the engine state plus a write-ahead log of the inputs admitted after them. After a crash, the server restarts from the latest durable checkpoint and replays the logged inputs, so it continues exactly where the durable history ends.

Java Edition 1.21.8 worlds in the Anvil format can be imported into a world directory and exported from one; a `java-1.21.4` server refuses Anvil interchange. Import and export cover every block state, and the block entities, entities and items Circuit models. Other content is rejected unless the substitution policy is chosen explicitly; see [Anvil import and export](#anvil-import-and-export).

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
| `session.lock` | Ownership of the directory; holds the owner's process id |

Each checkpoint has a new generation number, so no file that the durable manifest names is ever rewritten.

### Ownership

One process at a time owns a world directory. The owner holds an exclusive `flock` on `session.lock` before it reads or writes any world file, and keeps it until it stops; the kernel releases it when the process exits for any reason, so a crash never leaves the world locked. Any other owner is refused before it touches a file, with an error that names the holder:
- **Servers:** a second server on the same directory, in the same or another process, does not start.
- **Tools:** import, export and migration take ownership of every directory they use, so they refuse a world that a running server owns. Stop the server first, or export a copy (see [backups](RELEASE.md#upgrades-migration-and-recovery)).

A server binds its port before it opens its world, so a server that cannot listen fails without changing the world.

### Migrating between versions

A world directory records its bundle and rule set, and a server of another bundle refuses it. To move a world to another version:

```bash
xmake run circuit-server --migrate-world "$PWD/worlds/main-1.21.4" --world "$PWD/worlds/main" --game-version java-1.21.4
```

The migration owns both directories while it runs. It reads the latest durable state, replays its log, and writes a new world directory for the target bundle; the source is only read. It is refused when the two bundles run different rule sets, because the saved state would not mean the same.

## Durability protocol

Each tick, the server logs the tick's input before the engine applies it. Every `log-sync-ticks` ticks the record is synced before the tick runs. A change that players saw is therefore either in the durable log or after the last sync.

A checkpoint has two phases.

**Begin, on the game thread, between ticks:**
1. Take a copy-on-write snapshot of the engine. This costs time in the number of columns, not of blocks.
2. Sync the current log segment, so that the current checkpoint plus the log reach the new checkpoint.
3. Create the new, empty log segment for the ticks after this one, and sync the directory.
4. Replace the manifest with one that still names the current checkpoint but adds the new segment. Logging continues in the new segment right away.

**Finish, on a writer thread, while the game runs on:**
5. Encode the snapshot and write it to a temporary file, sync it, rename it and sync the directory. Sections that did not change since the last checkpoint reuse their earlier encoding.
6. Replace the manifest with one that names the new checkpoint and keeps the previous one, with the segments the fallback needs.
7. Remove files the new manifest no longer names.

Each manifest replacement writes a temporary file, syncs it, renames it over `manifest` and syncs the directory. Every manifest names complete files, and recovery never sees a mixture of two.
- **Crash between the phases:** recovery continues from the current checkpoint through the new segment.
- **Crash after step 6:** recovery starts from the new checkpoint.

The server writes a checkpoint at startup, every `autosave-ticks` ticks and when it stops; the first and the last are written synchronously. One checkpoint is written at a time. If the previous one is still being written when the next is due, that autosave is skipped and the log keeps every input meanwhile. [The storage evaluation](STORAGE_EVALUATION.md) has the measurements behind this design and the choice of compression.

## Recovery

At startup the server reads the manifest:
1. **Checkpoint:** it loads the current checkpoint and checks its digest. If the current checkpoint is unreadable or damaged, it uses the previous one and replays the longer log; the log names this fallback.
2. **Log replay:** it replays the log records after the checkpoint in order, up to the first incomplete or damaged record. A torn record at the end of a segment is expected after a crash and is discarded, with a log note. A later segment that a checkpoint started at the last complete record continues the history.
3. **Fresh segment:** it writes a checkpoint of the recovered state before accepting players, so that new records never follow a discarded tail.

Recovery fails, and the server does not start, when:
- another process owns the directory;
- the manifest names a world saved by another bundle or rule set;
- no named checkpoint is readable;
- a log record has a valid checksum but cannot be decoded, which means the log is corrupt rather than torn.

An input that the engine cannot apply during replay ends the history before it. The server logs the tick and continues from the last good one.

Files from which recovery discarded data are not deleted: a damaged checkpoint, or a segment with a discarded tail. Once no manifest needs them, they are renamed to `damaged-<name>` and kept for diagnosis. Delete them when they are no longer needed.

Players are not connected after a restart: their chunk tickets are removed in the first tick. Player inventories are simulation state: every change is a logged command, so recovery restores them like the world. Player positions, game modes, health and hunger are saved with each checkpoint and when a player leaves; a crash can lose those since the last checkpoint, but never logged world or inventory changes. A player who left dead joins respawned at the world spawn.

## Failure handling

The server fails closed on storage errors. If a log record cannot be written, the server does not run the tick. It tries to write a checkpoint of the state before the tick, disconnects every player and stops. The same happens when an autosave fails. `circuit-server --serve` then exits with a non-zero status, and the next start recovers the last durable state.

## Checkpoint format

A checkpoint file is `CIRCKPT\0`, a format version (6; versions 1–5 are still read), the bundle and rule-set ids, the block registry id, the seed and tick, the raw and compressed body sizes, the zlib-compressed body, and a SHA-256 digest of everything before it. Block states are stored in the encoding of the named block registry, and a checkpoint of another registry is refused. Versions 1–4 predate the registry: their block states are a subset of `circuit-blocks-1` and are read as such. Versions before 6 have item stacks without components. The body holds:
- columns with palette-packed sections;
- block entities, entities (with their pickup delay), scheduled ticks and pending block events;
- chunk tickets and the engine counters;
- player inventories: the 46 slots, the carried stack and a drag in progress; item stacks carry their damage and their other data components;
- block entities' components (a container's name) and the saved data of block entities of blocks without rules of their own;
- named extras, such as the server's metadata and player positions.

Decoding checks every length, count and the digest. The engine's restoring constructor then checks the semantics: positions inside known columns, valid block states, and no duplicate ids. The server checks every item component of the checkpoint and the log as it checks a client's, and refuses a world with components of another protocol or outside the registries it synchronizes. A restored engine continues exactly like the one that produced the checkpoint.

Log segments start with `CIRCWAL` and a format byte: 1 for records whose stacks carry components, 0 (read only) for records written before.

## Anvil import and export

```bash
xmake run circuit-server --import-anvil /path/to/java-world --world "$PWD/worlds/imported"
xmake run circuit-server --export-anvil /path/to/new-java-world --world "$PWD/worlds/imported"
```

Import reads the source world and never writes to it. It creates a new world directory and refuses one that already holds a world. Export reads the latest durable state of a world directory and writes no world file to it. It takes ownership of the directory, so it refuses a world while a server runs on it. It writes a new world to a temporary sibling directory and renames it at the end, and it refuses a target that exists.

**Supported.** A 1.21.8 world (DataVersion 4440), overworld only:

| Kind | Supported content |
| --- | --- |
| Blocks | Every block state of 1.21.8. The blocks with rules of their own are air, stone, glass, obsidian, redstone block, slime block, lever, redstone dust, repeater, comparator, redstone lamp, observer, pistons and their heads, moving pistons, hopper, chest, water and wheat; every other block keeps its state and is inert in the rules |
| Block entities | Chests and hoppers with their items, custom name and components, the hopper transfer cooldown, comparator output signals and moving pistons. Block entities of blocks without rules of their own (signs, banners, furnaces and the rest) are kept as saved and written back unchanged |
| Item components | The damage, and the components whose save form mirrors their network form: `custom_data`, `max_stack_size`, `max_damage`, `unbreakable`, `custom_name`, `item_name`, `item_model`, `lore`, `rarity`, `repair_cost`, `enchantment_glint_override`, `intangible_projectile`, `glider`, `tooltip_style`, `dyed_color`, `map_color`, `map_id`, `map_decorations`, `potion_duration_scale`, `recipes`, `ominous_bottle_amplifier`, `note_block_sound`, `provides_banner_patterns`, `lock`, `container_loot`, `debug_stick_state`, `entity_data`, `bucket_entity_data` and `block_entity_data`; those that name registry entries: `trim`, `banner_patterns` and `base_color`; a removed default (`!minecraft:name`) |
| Entities | Item entities, with their pickup delay |
| Scheduled ticks | Block and fluid ticks of blocks with rules of their own, with their delay and priority |
| Level data | Time, seed, spawn point and level name |

Every block property round-trips unchanged, including dust shapes, lever mounting, repeater locks, cave air and properties the rules only store, such as a chest's `waterlogged`.

**Unsupported content and policies.** Anything else is unsupported content, for example:
- blocks unknown to 1.21.8 or with invalid properties; scheduled ticks of blocks without rules of their own; other entities; and items with components that have no save form in Circuit yet (enchantments, potions, books, containers and the rest are kept on the network and in checkpoints, but not converted to or from region files yet; inline trim or banner pattern definitions likewise);
- containers with a loot table or a lock;
- block entities with custom names, loot tables or locks;
- the Nether and the End;
- chunks that still have an older DataVersion because the game has not re-saved them.

By default, an import that meets unsupported content imports nothing and lists every kind with its count. With `--substitute-unsupported`, the import goes ahead:
- **Blocks** with invalid properties are replaced by a stand-in of the same coarse shape: air for blocks without collision, obsidian for unbreakable blocks, stone for full light-blocking solids and glass for other solids. Unknown blocks become air.
- **Other content** is dropped: block entities, items and entities.

Export refuses a state whose items hold a component without a save form, and lists them; it writes nothing.

The report lists every replacement. Substitution changes behavior: for example, an enchanted tool or a container with a loot table is dropped. A substituted world is therefore not evidence about the original machine.

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
