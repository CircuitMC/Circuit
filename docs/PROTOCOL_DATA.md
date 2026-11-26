# Protocol data

The sources of Circuit's numeric protocol data, the checks that guard them, and the wire differences between the two target versions. The packet layouts are in `src/server/wire.cpp` and the tables in `src/protocol/`.

Numeric protocol data is compiled from public documentation, and every table names its source.

| Data | Source | Tool |
| --- | --- | --- |
| Packet ids | minecraft.wiki "Java Edition protocol/Packets", revision 3258009 (1.21.8) and 2938097 (1.21.4) | `tools/extract_packet_ids.py` |
| Block states, items, entity types | PrismarineJS minecraft-data (MIT) at commit `095709f3`, which the wiki's protocol FAQ points to | `tools/compile_content_tables.py` |
| Registry entries, their data and the biome tags 1.21.4 needs | minecraft.wiki pages cited in `src/server/registries.cpp` | by hand |
| Item component schema and static registry sizes | PrismarineJS minecraft-data `protocol.json` and registry tables at the same commit | `tools/compile_components.py` |
| Map colors of block states | What held maps drew on both official servers for every state Circuit's probe placed (`tests/fixtures/maps/colors-*.txt`), with PrismarineJS minecraft-data's collision shapes at the same commit for the waterlogged rule | `tools/compile_map_colors.py` |
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

The real client exercised the rest with the first generation's server.
