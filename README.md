# Circuit

Circuit is an experimental Minecraft Java Edition server written in C++23 and built with xmake. Its goal is a server that is faster than the official one on a single thread and that scales across cores: first as independent islands of the world, like Folia, and then with several threads inside one island.

## Status: core restructuring

The project is being restructured around a new runtime. The first generation (tagged `pre-refactor`) reached a playable creative and survival server for 1.21.8 and 1.21.4, but its region-parallel engine did not scale with threads and its host-side player code could not grow into mobs, islands or large player counts. That runtime, the network server, the player code and the extension system have been removed; the parts below were kept because tests pin their behavior.

What the tree contains now:

- **Contracts** (`src/contracts`): identities, logical time, version bundles, normalized commands, traces and replay records.
- **Reference rules** (`src/gameplay`): redstone, pistons and moving blocks, containers, hoppers, dispensers and droppers, crafters, furnaces, brewing, fluids, fire, plants and random ticks, light, falling blocks, TNT and explosions, items, experience orbs, arrows, minecarts and boats, chunk tickets, player inventories and menus. The rules are plain functions over `RuleContext` and know nothing about threads or storage.
- **The oracle** (`tests/oracle`): a sequential executor that applies the rules in the vanilla order. It is the specification of the single-threaded runtime.
- **Evidence** (`src/evidence`, `tests/fixtures`): the circuit-trace format and runner. The rules match 225 traces recorded on unmodified 1.21.8 servers and 220 on 1.21.4 servers; 31 of them need the official data packs.
- **Protocol** (`src/protocol`): codec, NBT, framing with limits, item components and packet tables for protocols 772 (1.21.8) and 769 (1.21.4).
- **Content data** (`src/data`, `src/server`): data packs from the official jar, loot tables, recipes, the recipe book, synchronized registries, packet layouts and the block palette.
- **Storage** (`src/storage`, `src/server/anvil.cpp`): crash-safe world directories with checkpoints and a write-ahead log, and 1.21.8 Anvil import and export.
- **Tooling**: the compatibility ledger, data generators, worktree slots and fail-closed gates.

There is no runnable server in this state. See [the roadmap](TODO.md) for the order in which the runtime, networking and player view return.

## Direction

1. **A single-threaded runtime in vanilla order.** The rules run on a new world store built for speed (hashed column index, paletted sections, active sets, time-ordered tick queues) and must match the oracle exactly. Target: faster than Paper on one thread.
2. **Islands.** Groups of chunks that cannot affect each other tick independently on their own clocks, on all cores, without a global tick barrier. Target: faster than Folia.
3. **Threads inside an island.** Each chunk is a unit of work with an estimated cost. An island whose estimated work does not fit in a tick is divided into connected sub-islands, each with its own execution order, and a thread that falls behind hands the outer chunks of its share to idle threads.
4. **Networking on PACE**, the project owner's networking library, with coroutine sessions, batched writes and several reactors for large player counts.
5. **Entities and players in the engine**, with a living-entity base shared by players and mobs, then the player view and gameplay breadth.

Every mode keeps the vanilla order where it can. Visible differences of island mode are documented, never silent.

## Build

Requires Linux, xmake 3.0+ and a C++23 compiler with `std::expected` (GCC 13 verified), zlib and OpenSSL.

```bash
xmake f -m debug -y
xmake build -y --all
xmake run rules_tests
tools/check.sh --lane
```

[The development guide](docs/DEVELOPMENT.md) lists the test programs, the gates and the official jars some cases need.

## License

Source is licensed under [AGPL-3.0-only](LICENSE). AGPL-compliant commercial use is permitted. A separate commercial license is available from the rights holders; see [the licensing policy](LICENSING.md).

Circuit is an independent project and is not affiliated with Mojang or Microsoft.
