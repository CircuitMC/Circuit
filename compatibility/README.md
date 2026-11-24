# Compatibility ledger data

This directory holds the machine-readable status of Circuit against the full server-side game of Java Edition 1.21.4 and 1.21.8. The [vanilla completion roadmap](../docs/VANILLA_COMPLETION.md) defines the target; the generated [ledger summary](../docs/VANILLA_LEDGER.md) presents this data.

| File | Kind | Contents |
| --- | --- | --- |
| `versions/<bundle>.json` | hand-written | Release, protocol, DataVersion, pack formats, the official server and client artifacts with their hashes, the default and experimental profiles, and the public-data commit |
| `families/<Vnn>/_family.json` | hand-written | A family's name, its milestones and the order of its mechanics |
| `families/<Vnn>/<mechanic>.json` | hand-written | One named mechanic: its milestone and per-version status, cases and gaps |
| `families/_inventories_unknown.json` | hand-written | Inventories not yet collected |
| `resources/<kind>/_kind.json` | hand-written | The default status of a kind of enumerated resource, and the ordered rules assigning each of its names to a family |
| `resources/<kind>/<name>.json` | hand-written | Status of the resource of that name, where it differs from its kind's default |
| `cases/<id>.json` | hand-written | One behavior case other than the reference traces: a client script, a round trip or a planned acceptance case |
| `inventory/<bundle>.json` | generated | Blocks, items, entity types, effects, enchantments, attributes, biomes, particles, commands, damage types and packets, with counts of block states, sound events, recipes and instruments |
| `ledger.json` | generated | One feature per mechanic and per enumerated resource, with a record per version: implementation state, evidence state, cases and gaps |

The hand-written data holds one entry per file. An entry's id is its path: `families/V14/dust.json` is the mechanic `V14.dust`, `resources/block/stone.json` the block `stone`, `resources/packet/play/serverbound/container_click.json` a packet, and `cases/client/windows.json` the case `client/windows`. Branches that add or change different entries therefore merge without conflicts, and two branches adding the same entry conflict on its file. A new mechanic needs only its file: mechanics listed in `_family.json` come first in the listed order, the others follow by id. Files are JSON with two-space indentation as `format` writes them; `check` refuses other layouts and keys given twice. The generated files are written where branches are integrated; a branch validates its own changes with `check --sources-only`.

Each feature has a stable id: `V14.dust` for a mechanic, `block/minecraft:stone` for a resource, `play/serverbound/container_click` under `packet/` for a packet. A record is `implemented`, `partial`, `data_only`, `missing` or `not_applicable` (with a reason), and its evidence is `compared`, `reference_only`, `documented`, `mismatch` or `none`. Compared evidence names a passing case, mismatch evidence a failing one, and reference-only evidence an official observation. The target-version traces are cases `trace/<name>`: passing ones in `tests/fixtures/harness*/`, linked to the blocks they place, and failing or unsupported ones in `tests/fixtures/open*/`.

```bash
python3 tools/compat_ledger.py check      # validate; fails when the generated files are stale
python3 tools/compat_ledger.py check --sources-only   # validate without comparing the generated files
python3 tools/compat_ledger.py generate   # write ledger.json and docs/VANILLA_LEDGER.md
python3 tools/compat_ledger.py format     # rewrite the hand-written files in canonical form
python3 tools/compat_ledger.py inventory --mcdata <dir>   # rebuild the inventories
```

`inventory` reads PrismarineJS minecraft-data (MIT) at the commit the manifests pin: `<dir>/dataPaths.json` and `<dir>/<release>/tables/<table>.json`. Where minecraft-data maps a release to an older table, the inventory is marked approximate. Packets and damage types come from Circuit's own tables, which cite their sources. The manifests' values were read from the official jars after checking their hashes against Mojang's published version metadata; the jars themselves are not part of this repository.
