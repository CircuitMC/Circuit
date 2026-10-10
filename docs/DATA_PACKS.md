# Data packs

Circuit ships no game data. An operator may give a server the official server jar of its version, whose built-in data pack then provides tags and the definitions of data-driven registries. Data-pack directories and archives may follow it.

## Configuration

| Key | Meaning |
| --- | --- |
| `vanilla-server-jar` | The official server jar of the server's version. Circuit refuses a jar whose SHA-1 is not the one the version bundle records (also listed in `compatibility/versions/`), or whose `version.json` names another version, protocol, save version or data pack format |
| `data-pack` | A data-pack directory or `.zip` archive; repeatable, lowest precedence first, after the built-in pack |

```bash
circuit-server --data-report --game-version java-1.21.8 --vanilla-jar /path/to/server.jar
```

`--data-report` loads the packs as a server would. It prints:
- each pack's digest and file count;
- the data fingerprint;
- tag and resource counts;
- what the server sends;
- where block tags disagree with the public registry data.

`--import-anvil` and `--export-anvil` use the packs of the `--config` file.

## Loading

The built-in pack is read from the jar in memory, as it is packed:
- the bundled game jar under `META-INF/versions/`;
- its `data/` directory;
- not the experimental packs it contains.

Other packs need a `pack.mcmeta` whose `pack_format`, or `supported_formats` range, includes the version's data pack format: 81 for 1.21.8, 61 for 1.21.4. Packs with `filter` or `overlays` are refused; they are not supported yet.

Precedence follows the target version:
- a later pack's file replaces an earlier one with the same path;
- tag files merge, unless a later one sets `"replace": true`.

Tags are flattened: references to other tags resolve recursively. Any of the following fails the load, as it fails a reload in the target version:
- a cycle;
- a missing referenced tag;
- an unknown required entry.

Optional entries (`{"id": ..., "required": false}`) that do not exist are skipped.

Circuit loads the tags of the registries it can list:
- block, item, entity type and fluid;
- the registries it synchronizes.

The tags of game events, points of interest, structures, world presets and the 1.21.8 dialogs are noted and skipped.

## Consumers

- **Tags for clients:** clients that share the vanilla pack receive every loaded tag of the registries Circuit numbers. With the official packs that is the same number of tags per registry as the official servers send. Clients then predict mining speeds from the same block tags as the server.
- **Enchantments:** with tags loaded, the enchantment registry is synchronized, because the client's enchantment definitions refer to item and enchantment tags. Item components with enchantments are then accepted, and enchantments convert to and from the Anvil save form.
  - Packs must define exactly the vanilla enchantments; packs that add or remove some are refused, since clients only know the vanilla definitions by name.
- **Block tools:** the `mineable/*` and `needs_*_tool` block tags give the effective tool and harvest tier of blocks without rules of their own. A block that needs a tool of another kind (a sword or shears) keeps that from the public data.
  - With the official packs, 44 blocks of 1.21.4 and 2 of 1.21.8 get another tool or tier than the public data gave. Most are plants, which the official tags put in `mineable/axe`; the crafter needs a stone pickaxe.

Loot tables and recipes are read but not used yet.

## The world's data

The rules read the loaded data, so a world records the fingerprint of the data its log was written with:
- **A log to replay:** a server with other data refuses to open the world, and names both fingerprints.
- **No log:** the data may change, and the world takes the new fingerprint.

Without data packs the fingerprint is `none`.
