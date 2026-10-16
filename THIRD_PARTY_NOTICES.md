# Third-party notices

Circuit is licensed under AGPL-3.0-only (see `LICENSE`). It uses or includes material from the following third parties, which keep their own terms.

| Component | Use | License |
| --- | --- | --- |
| Standalone Asio 1.34.2 | Networking and executors (headers, not included in this repository) | Boost Software License 1.0 |
| zlib | Packet, checkpoint and Anvil region compression (system library) | zlib License |
| OpenSSL 3 | Protocol encryption, TLS for session verification (system library) | Apache License 2.0 |
| PrismarineJS minecraft-data, commit `095709f3ad62b043069141dc127fc80ef6cd10fa` | Block-state, item and entity-type tables and coarse block shapes in `src/protocol/content_*.cpp` (`tools/compile_content_tables.py`); the block registry in `src/gameplay/block_registry_data.cpp` (`tools/compile_block_registry.py`); the item registry in `src/gameplay/item_registry.cpp` (`tools/compile_item_registry.py`); the item component schema in `src/protocol/component_schema.cpp` (`tools/compile_components.py`, from `protocol.json` and the registry tables); 1.21.4 biome list cross-check | MIT (below) |

Packet ids, registry entries, tags and protocol field layouts were compiled from the public protocol documentation on minecraft.wiki, at the revisions cited in the source files and in `docs/SERVER.md`; the Anvil, chunk, entity, block entity and level formats likewise, at the revisions cited in `docs/PERSISTENCE.md`. Circuit takes identifiers and numeric facts from those pages and cites them. It does not copy their prose; the page texts remain under their own license.

The storage benchmark (`tools/bench/storage_bench.cpp`) loads the system's zstd (BSD) and LZ4 (BSD) libraries at run time when they are present, to compare compressors; Circuit does not link or distribute them.

Minecraft is a trademark of Mojang AB. Circuit is not affiliated with Mojang or Microsoft and contains no game code or assets.

## PrismarineJS minecraft-data (MIT)

The minecraft-data README states its license as MIT. The repository has no separate license file and names no copyright holder; the notice below names its contributors.

```text
Copyright (c) the PrismarineJS minecraft-data contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
