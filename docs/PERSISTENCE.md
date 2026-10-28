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

The migration owns both directories while it runs. It reads the latest durable state, replays its log, and writes a new world directory for the target bundle; the source is only read. The same command moves a world to a newer rule set of the same version (a server refuses a world of another rule set and names this command). Between rule sets it needs a world stopped cleanly, with no log after its last checkpoint: the log could only be replayed by the rules that wrote it. The state carries over, and the new rules apply from its tick on.

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

Players are not connected after a restart: their chunk tickets are removed in the first tick, with every other ticket the target versions do not save (all but forced and portal tickets); the start ticket is added again. Every start counts as the levels' creation for the idle-level rule. Player inventories are simulation state: every change is a logged command, so recovery restores them like the world. Player positions, game modes, health, hunger and recipe books (the recipes known and still highlighted, the books' settings and the recipe advancements done, by id) are saved with each checkpoint and when a player leaves; a crash can lose those since the last checkpoint, but never logged world or inventory changes. A player who left dead joins respawned at the world spawn.

## Failure handling

The server fails closed on storage errors. If a log record cannot be written, the server does not run the tick. It tries to write a checkpoint of the state before the tick, disconnects every player and stops. The same happens when an autosave fails. `circuit-server --serve` then exits with a non-zero status, and the next start recovers the last durable state.

## Checkpoint format

A checkpoint file is `CIRCKPT\0`, a format version (50; versions 1–49 are still read), the bundle and rule-set ids, the block registry id, the seed and tick, the raw and compressed body sizes, the zlib-compressed body, and a SHA-256 digest of everything before it. Block states are stored in the encoding of the named block registry, and a checkpoint of another registry is refused. Older versions are read as follows:
- versions before 50 name no hotbar slot in players' presences, and their maps no holders: the first slot is in the main hand, and no one has explored a map;
- versions before 49 have no maps and no open cartography tables: none was made, none is open;
- versions before 48 have no chiseled bookshelves with rules of their own: they come back as the modeled blocks with empty block entities;
- versions before 47 have no shulker boxes with rules of their own: they come back as the modeled blocks with empty block entities (contents an earlier Anvil import kept in their saved data are not carried over);
- versions before 46 have no ender chests with rules of their own and no ender inventories: ender chests come back as the modeled blocks with empty block entities, and players' ender inventories are empty;
- versions before 45 have no composters with rules of their own: they come back as the modeled blocks;
- versions before 44 have no barrels with rules of their own: they come back as the modeled blocks with empty block entities;
- versions before 43 have no chest viewers and no trapped chests with rules of their own: nobody views a chest, and trapped chests come back as the modeled chests with empty block entities;
- versions before 42 have no lecterns with rules of their own: they come back as the modeled blocks with empty block entities;
- versions before 41 have no open enchanting tables and no enchantment seeds: none is open, and each player's seed is Circuit's own from the player;
- versions before 40 have no open anvils: none is open;
- versions before 39 have no recipes a furnace cooked: none are kept;
- versions before 38 have no entity randoms: each is seeded with its entity's id when first used;
- versions before 37 have no player experience: players have none;
- versions before 36 have no experience orbs: entities have no experience and one orb;
- versions before 35 have no brewing stand block entities: brewing stands were blocks without rules of their own, and they come back as the modeled blocks with empty block entities;
- versions before 34 have no open looms: none is open;
- versions before 33 have no open grindstones: none is open;
- versions before 32 have no crafter block entities: crafters were blocks without rules of their own, and they come back as the modeled blocks with empty block entities;
- versions before 31 have no open smithing tables: none is open;
- versions before 30 have no open stonecutters: none is open;
- versions before 29 have no open crafting tables: none is open;
- versions before 28 have no furnace times, and their furnaces, smokers and blast furnaces were blocks without rules of their own: they come back as the modeled blocks with empty block entities;
- versions before 27 have no random-tick position state: each level's starts at 0;
- versions before 26 have no difficulty: the world is peaceful until the server sets the configured one at its start;
- versions before 25 have no fall distances: every entity starts its count again;
- versions before 24 have no random sequences: each loot table's starts when it is first used, with the default settings;
- versions before 23 have no minecart yaw (0, not flipped);
- versions before 22 have no redstone torch turn-offs;
- versions before 21 have no piston pushes: no entity was pushed in the tick;
- versions before 20 have no minecarts or boats;
- versions before 19 have no arrows;
- versions before 18 have items at full health;
- versions before 17 have no primed TNT;
- versions before 16 have no falling blocks;
- versions before 15 have no entities waiting to come back with a reloaded chunk;
- versions before 14 have no entity tick counts: an item's is its age, which nothing reset before;
- versions before 13 store entity positions and velocities in 1/4096-block units, which are read exactly as doubles, and no level random: each level's is seeded as for a new world, from the seed and the level; their entities are items;
- versions before 12 have no weather: clear, with every timer at 0;
- versions before 11 have no day time: it is 0;
- versions before 10 have no game rules: every rule has its default;
- versions before 9 do not record when saved columns dropped: their pending ticks wait from the checkpoint's tick;
- versions before 8 have untyped tickets: each is read as a forced ticket of level 31 over its radius (its load radius is dropped), and each ticking column as entity ticking, each border column as border without simulation;
- versions before 7 hold the overworld alone, and their tickets are the overworld's;
- versions 1–4 also predate the registry: their block states are a subset of `circuit-blocks-1` and are read as such;
- versions before 6 have item stacks without components.

The body holds:
- the time, the entity id counter, counted over all levels, the tick the levels last started, the game rules away from their defaults (name and value), the day time, the weather (timers, flags and levels), the settings new random sequences start with (salt and flags), and the difficulty;
- per level (overworld, Nether, End; only those with columns, players or an active tick): its counters, the last tick it had players or forced chunks, the state of its level random and of its random-tick position generator, the players in it (position and whether a spectator), columns with their load and simulation levels, the tick a saved column dropped and the tick a reloaded column resumes, palette-packed sections, block entities, entities (their kind, tick count, position and velocity as doubles, an item's stack, age, pickup delay and health, a falling block's state and fall time, primed TNT's fuse, an arrow's being stuck, life and fire, a boat's wood, last state and last vertical move, a minecart's water, the tick and amount of its last push by moving blocks, how far it has fallen, and for an entity of a reloaded chunk the tick it comes back), scheduled ticks, pending block events, the recent turn-offs of its redstone torches (position and tick, for their burnout) and its loot tables' random sequences (id and state, by id);
- chunk tickets: type, level, square radius, the tick their lifetime ends and their level (dimension);
- player inventories: the 46 slots, the carried stack, a drag in progress, an open crafting table's result and grid, an open stonecutter's input, result and chosen recipe, an open smithing table's and grindstone's slots, an open loom's slots and chosen pattern, an open anvil's slots, typed name and cost, an open enchanting table's slots and offers, an open cartography table's slots, the player's enchantment seed once enchanting changed it and the chest, barrel or ender chest the player views, and the player's ender inventory; item stacks carry their damage and their other data components;
- block entities' components (a container's name), a chest's or barrel's viewers, and the saved data of block entities of blocks without rules of their own (empty for one a player placed: an Anvil export writes the official defaults of its type);
- named extras, such as the server's metadata and player positions.

Light is not saved: it is a function of the blocks, and the engine computes it again after a restart.

Decoding checks every length, count and the digest. The engine's restoring constructor then checks the semantics: positions inside known columns, valid block states, and no duplicate ids. The server checks every item component of the checkpoint and the log as it checks a client's, and refuses a world with components of another protocol or outside the registries it synchronizes. A restored engine continues exactly like the one that produced the checkpoint.

The server's metadata records the fingerprint of the [data packs](DATA_PACKS.md#the-worlds-data) the log was written with; a server with other data refuses to replay that log.

Log segments start with `CIRCWAL` and a format byte:
- 17: commands include closed windows that name their block;
- 16 (read only): commands include window buttons of creative players;
- 15 (read only): commands include names typed into anvils and experience changes;
- 14 (read only): summoned experience orbs carry their value, count and age;
- 13 (read only): commands include slot toggles;
- 12 (read only): commands include window buttons;
- 11 (read only): commands include closed windows whose items drop;
- 10 (read only): commands include the difficulty;
- 9 (read only): commands include summoned entities, player uses with where they clicked and items used along the view;
- 8 (read only): spawned items may carry a pickup delay and an age;
- 7 (read only): positions and velocities in commands are doubles;
- 6 (read only): commands include the weather;
- 5 (read only): commands include day times;
- 4 (read only): commands include game rule changes;
- 3 (read only): tickets are typed, and commands include players' presence and server starts;
- 2 (read only): commands name their level; tickets are read as forced ones;
- 1 (read only): overworld commands whose stacks carry components;
- 0 (read only): records written before stacks carried components.

A world directory written by an earlier release (version 6 checkpoints, format 1 segments, `circuit-reference-rules-v7`) is kept as a test fixture (`tests/fixtures/storage/`). `storage_tests` checks that other rules refuse it, and that under its own rules every record is read (tickets as forced ones, commands as the overworld's) and new records use the current formats. The server opens such a world only through `--migrate-world`, which needs a world stopped cleanly (no log after its last checkpoint). A world whose server metadata predates typed tickets (metadata version 3) keeps none of its tickets: they were all spawn and player tickets, and the server adds them again. A world written and stopped by the last `circuit-reference-rules-v9` server (game rules, day time, weather and a forced chunk set) is kept as a second fixture: `server_tests` checks that it is refused, migrates with `--migrate-world`, and opens under the current rules with its state. A world written and stopped by the last `circuit-reference-rules-v10` server, 20 ticks after a player primed TNT in a forced chunk, is a third: `server_tests` checks that it is refused, migrates, and opens with the primed TNT, which explodes when its saved fuse runs out. A world written and stopped by the last `circuit-reference-rules-v11` server, 3 seconds after a player started a flying engine toward an obsidian wall in forced chunks, is a fourth: `server_tests` checks that it is refused, migrates, and opens with the engine in flight, which flies on until the wall stops it. A copy that is stopped and started again 40 ticks later holds the same machine at every tick as one that runs without stopping.

## Anvil import and export

```bash
xmake run circuit-server --import-anvil /path/to/java-world --world "$PWD/worlds/imported"
xmake run circuit-server --export-anvil /path/to/new-java-world --world "$PWD/worlds/imported"
```

Import reads the source world and never writes to it. It creates a new world directory and refuses one that already holds a world. Export reads the latest durable state of a world directory and writes no world file to it. It takes ownership of the directory, so it refuses a world while a server runs on it. It writes a new world to a temporary sibling directory and renames it at the end, and it refuses a target that exists.

**Supported.** A 1.21.8 world (DataVersion 4440), overworld only:

| Kind | Supported content |
| --- | --- |
| Blocks | Every block state of 1.21.8. The blocks with rules of their own are air, stone, glass, obsidian, redstone block, slime block, honey block, lever, buttons, pressure plates, target blocks, daylight detectors, note blocks, rails, coral fans, dispensers, droppers, crafters, furnaces, smokers, blast furnaces, brewing stands, doors, trapdoors, fence gates, redstone torches, redstone dust, repeater, comparator, redstone lamp, observer, pistons and their heads, moving pistons, hopper, chest, water and wheat; every other block keeps its state, and those without rules of their own are inert in the rules. Redstone torches' recent turn-offs are not written to region files: the official servers keep them in memory only |
| Block entities | Chests, hoppers, dispensers, droppers, crafters, furnaces, smokers, blast furnaces and brewing stands with their items, custom name and components, the hopper transfer cooldown, furnaces' times (`cooking_time_spent`, `cooking_total_time`, `lit_time_remaining`, `lit_total_time`; the recipes a furnace used, `RecipesUsed`, are not kept and written empty), crafters' `disabled_slots` and `crafting_ticks_remaining` (`triggered` is written from the block state), brewing stands' `BrewTime` and `Fuel` (the ingredient a brew uses is not saved: a loaded brew carries on with the one in its slot), comparator output signals, daylight detectors and moving pistons. Block entities of blocks without rules of their own (signs, banners, beacons and the rest) are kept as saved and written back unchanged |
| Item components | The damage, and the components whose save form mirrors their network form: `custom_data`, `max_stack_size`, `max_damage`, `unbreakable`, `custom_name`, `item_name`, `item_model`, `lore`, `rarity`, `repair_cost`, `enchantment_glint_override`, `intangible_projectile`, `glider`, `tooltip_style`, `dyed_color`, `map_color`, `map_id`, `map_decorations`, `potion_duration_scale`, `recipes`, `ominous_bottle_amplifier`, `note_block_sound`, `provides_banner_patterns`, `lock`, `container_loot`, `debug_stick_state`, `entity_data`, `bucket_entity_data` and `block_entity_data`; those that name registry entries: `trim`, `banner_patterns`, `base_color`, and, when the configured data packs synchronize enchantments, `enchantments` and `stored_enchantments`; a removed default (`!minecraft:name`) |
| Entities | Every entity with how far it has fallen (`fall_distance`); item entities, with their age, pickup delay, health and fire (`Fire`); experience orbs, with their value, the orbs merged into them (`Count`), age and health (the official 1.21.4 server saves their health as 0, and so does an export to 1.21.4); falling blocks, with their block state and time; empty minecarts and boats (a boat's kind by its id; a minecart's yaw as its `Rotation`, whether it is flipped not saved, as the official servers do; their state before the tick is not saved); arrows and snowballs (`inGround`, `life`, `Fire`; their owner, damage and pickup rule are not kept); primed TNT, with its fuse (`fuse`, `block_state` TNT and `explosion_power` 4 when exported; other display states and powers are unsupported on import) |
| Scheduled ticks | Block and fluid ticks of blocks with rules of their own, lava's fluid ticks among them, and the block ticks of falling blocks, sugar cane, cacti, vines, dripleaves, farmland, fire and frosted ice, with their delay and priority |
| Level data | Time, day time, difficulty (`Difficulty`, normal when absent; the server sets its configured difficulty at every start, as the official dedicated server does), weather (flags and timers; the levels follow the flags, as when the official servers load a world), game rules (rules the version lacks, or has only with experiments, are ignored with a note; invalid values are refused), seed, spawn point and level name. An exported world holds every rule of the version; the official 1.21.8 server read the day time and rules of one |
| Random ticks | The official servers do not save the state random ticks pick positions from (each start gives it a random value); an imported level's starts at 0 |
| Random sequences | `data/random_sequences.dat`: the loot tables' random sequences (id and 128-bit state) and the settings new ones start with (salt, and the flags, written only when false as the official server writes them). The official 1.21.8 server continued the sequences of an exported world and started new ones with its settings |
| Maps | `data/map_<id>.dat` (center, scale, dimension, lock, tracking flags and colors; fields at their defaults absent, as the official servers save them) and `data/idcounts.dat` (the last map id given); the players who held a map, with their steps, are not saved, as on the official servers |

Every block property round-trips unchanged, including dust shapes, lever mounting, repeater locks, cave air and properties the rules only store, such as a chest's `waterlogged`.

**Unsupported content and policies.** Anything else is unsupported content, for example:
- blocks unknown to 1.21.8 or with invalid properties; scheduled ticks of blocks without rules of their own (but falling blocks, sugar cane, cacti, vines, dripleaves, farmland, fire and frosted ice); other entities; and items with components that have no save form in Circuit yet (enchantments, potions, books, containers and the rest are kept on the network and in checkpoints, but not converted to or from region files yet; inline trim or banner pattern definitions likewise);
- containers with a loot table or a lock;
- block entities with custom names, loot tables or locks;
- the Nether and the End;
- chunks that still have an older DataVersion because the game has not re-saved them.

By default, an import that meets unsupported content imports nothing and lists every kind with its count. With `--substitute-unsupported`, the import goes ahead:
- **Blocks** with invalid properties are replaced by a stand-in of the same coarse shape: air for blocks without collision, obsidian for unbreakable blocks, stone for full light-blocking solids and glass for other solids. Unknown blocks become air.
- **Other content** is dropped: block entities, items and entities.

Export refuses a state whose items hold a component without a save form, and lists them; it writes nothing. Export leaves out a block tick of a block without rules of its own whose block has gone, such as the next tick of fire that went out: Circuit's ticks of such blocks do not name the block. The official servers save it; it does nothing unless the same block is back when it is due.

The report lists every replacement. Substitution changes behavior: for example, an enchanted tool or a container with a loot table is dropped. A substituted world is therefore not evidence about the original machine.

**Not kept.** Import keeps none of the following:
- light, heightmaps and biomes;
- structures and pending post-processing;
- player data, raids, scoreboards and the world border (of the saved data, only random sequences and maps are read), and maps' banners and frames;
- the order of creation of block entities and entities in the source world. Circuit orders them by chunk position.

Positions and motion are kept as the doubles they are; motion is limited to 64 blocks per tick on each axis. Item ages are kept, -32768 (never) included; an entity's tick count, which the format does not save, starts again at 0. Export writes the following and leaves light and heightmaps to the game:
- `level.dat`, with a superflat generator that matches the server's floor height;
- `data/random_sequences.dat`;
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
| `anvil_tests` | <ul><li>export and import of the scenario states, with scheduled ticks, item entities and moving pistons, and a falling block in the air; primed TNT exported mid-fuse, imported, exploding on time;</li><li>both policies on hand-made worlds;</li><li>malformed and damaged region files;</li><li>the world tools.</li></ul> |
| `server_tests` | <ul><li>a world saved and restored with a player's position;</li><li>the real server binary killed with `SIGKILL` while a player acts, recovered with every synced change;</li><li>a world of other rules refused.</li></ul> |
| `simulation_tests` | Engines restored from a dump continue exactly, for every scenario at four split points |

The official 1.21.8 server also serves as a black-box check of the Anvil code. It runs outside this repository:
- **Export:** it loads exported worlds with ticking frozen and re-saves them, and the saved blocks, block entities, scheduled ticks and item entities are compared with the export.
- **Import:** worlds it builds are imported and compared.
