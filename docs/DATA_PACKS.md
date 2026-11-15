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
- where block tags disagree with the public registry data;
- how many loot tables and recipes the rules use, and why not the others.

`--import-anvil` and `--export-anvil` use the packs of the `--config` file.

## Loading

The built-in pack is read from the jar in memory, as it is packed:
- the bundled game jar under `META-INF/versions/`;
- its `data/` directory;
- not the experimental packs it contains;
- and its English language file (`assets/minecraft/lang/en_us.json`), for the names items show (an anvil compares a typed name with them).

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
  - Each enchantment's `slots` and its `minecraft:attributes` effects give players attribute modifiers in those slots; its `minecraft:damage_protection` effects (an added level-based value) protect them against the damage their requirements match, and its `minecraft:damage_immunity` effects keep it off; its `minecraft:damage` and `minecraft:knockback` effects (added values) add to their attacks, and its `minecraft:post_attack` effects of the type `ignite`, from the attacker's enchantment onto the victim, set the players they attack on fire. A requirement may test the damage type against damage type tags (`damage_source_properties` with `tags`, and for attacks `is_direct`, alone), the type of the entity hurt (`this`), the attacker or the direct attacker (`entity_properties` with a `type` alone: an entity type, a list of them or an entity type tag), and combine such tests with `all_of`, `any_of` and `inverted`. A definition whose attribute effect names an attribute, operation, slot group or level-based value form Circuit does not know, or a modifier id outside the `minecraft` namespace, whose damage protection, damage or knockback effect is not an added value, whose ignite is not the attacker's onto the victim, or whose requirement has another form or names a tag no pack defines, refuses the packs. The damage type tags `bypasses_effects` and `bypasses_enchantments` give the damage no enchantment protects against. Other post-attack effects (thorns' damage and wear, bane of arthropods' slowness, channeling, wind burst) and its other effects are not acted on yet.
  - Anvils use each enchantment's `anvil_cost`, `max_level`, `supported_items` and `exclusive_set`, and grindstones its `min_cost`; the item tags `*_tool_materials` and `repairs_*` give what repairs an item at an anvil.
  - Lecterns take the books of the item tag `lectern_books`.
  - Beacons count the blocks of the block tag `beacon_base_blocks` in their pyramids and take the items of the item tag `beacon_payment_items` as payments (without tags, the five blocks and five items minecraft.wiki names).
  - Enchanting tables offer the enchantments of the tag `in_enchanting_table`, in its order, by their `weight`, `max_level`, `min_cost`, `max_cost`, `primary_items` (else `supported_items`) and `exclusive_set`; the block tags `enchantment_power_provider` and `enchantment_power_transmitter` give the bookshelves that power them and the blocks that let the power through.
- **Block tools:** the `mineable/*` and `needs_*_tool` block tags give the effective tool and harvest tier of blocks without rules of their own. A block that needs a tool of another kind (a sword or shears) keeps that from the public data.
  - With the official packs, 44 blocks of 1.21.4 and 2 of 1.21.8 get another tool or tier than the public data gave. Most are plants, which the official tags put in `mineable/axe`; the crafter needs a stone pickaxe.
- **Block loot:** blocks without rules of their own drop from their loot tables (`data/<namespace>/loot_table/blocks/`) when the tool harvests them; wheat drops from its table too. A wall torch, sign, hanging sign, banner or head uses the table of the standing one. Circuit compiles (`gameplay/loot.hpp`):
  - pools with a constant number of rolls, without bonus rolls;
  - item, alternatives, group, sequence and empty entries;
  - the conditions `survives_explosion`, `block_state_property`, `match_tool` (items, item tags and enchantment levels), `random_chance`, `table_bonus` (a chance for each enchantment level), `location_check` (the block at an offset, as the other half of a tall plant), `entity_properties` of the block's breaker without a predicate (a player, or an explosion's primed TNT, broke it: snow and chorus flowers), `any_of`, `all_of` and `inverted`;
  - the functions `set_count` with constant, uniform and binomial numbers, `limit_count`, `explosion_decay`, and `apply_bonus` with the `ore_drops`, `uniform_bonus_count` and `binomial_with_bonus_count` formulas.

  Random numbers come from the table's random sequence (`random_sequence`), as on the official servers: one Xoroshiro128++ generator per sequence id, started from the world seed and the MD5 of the id when first used (minecraft.wiki "Random sequence"), and drawn in the order the table lists its parts. The sequences are world state: checkpoints keep them, and an Anvil import or export reads and writes `data/random_sequences.dat`.

  A table with any other part is not used, and its block drops nothing; the report counts them by reason. With the official packs, 887 of the 952 block tables of 1.21.8 are used, and 879 of 944 in 1.21.4. The others copy components (`copy_components`, as shulker boxes and banners do) or block states (`copy_state`), or name dynamic contents. A compiled table whose roll chooses between weighted entries drops nothing either; no block table of either version has one. Other modeled blocks keep their fixed drops.
- **Recipes:** every recipe is checked against the target version's format (minecraft.wiki "Recipe") and item registry:
  - crafting (shaped, shapeless, transmute and the special types);
  - smelting, blasting, smoking and campfire cooking;
  - stonecutting;
  - smithing.

  As in the target version, a recipe that fails is skipped; the report lists why. Transmute results are item ids in 1.21.4 and stack templates in 1.21.8, and 1.21.8 trims name their pattern. All 1,407 recipes of 1.21.8 and 1,370 of 1.21.4 are valid. The shaped, shapeless and transmute recipes are used by the player's 2x2 crafting grid (see [simulation](SIMULATION.md)): 984 in 1.21.8 and 948 in 1.21.4. The twelve special recipes are made too (repairing, armor dye, banner copying, shield decoration, firework stars, firework rockets, star fades, book and map cloning, map extending, tipped arrows and decorated pots). Recipes whose results carry components (suspicious stews) are valid but make nothing yet.

Both consumers were compared with the official servers of both versions. In a 48-step crafting scenario the windows were identical. Of 23 blocks broken with various tools, the 19 whose tables are deterministic dropped the same items. Three recorded traces break grasses, ferns, dead bushes, sweet berry bushes, wheat and other plants with random loot, and two more have a player break ores, amethyst clusters, glowstone, melons, sea lanterns, every kind of leaves, gravel and gilded blackstone with tools of fortune 0 to 3, silk touch, shears or a bare hand: with the official packs, every drop and every draw matches both versions, and the sequences the official servers saved equal Circuit's. The observations are kept as test fixtures in `tests/fixtures/crafting/` and `tests/fixtures/loot/`; `server_tests` replays them when the official jars are given (`CIRCUIT_VANILLA_JAR_1_21_8`, `CIRCUIT_VANILLA_JAR_1_21_4`).

## The world's data

The rules read the loaded data, so a world records the fingerprint of the data its log was written with:
- **A log to replay:** a server with other data refuses to open the world, and names both fingerprints.
- **No log:** the data may change, and the world takes the new fingerprint.

Without data packs the fingerprint is `none`.
