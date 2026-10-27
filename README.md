# Circuit

Circuit is an experimental Minecraft Java Edition server written in C++23, built with xmake and standalone Asio. It explores chunk-level parallel simulation with explicit ownership and cross-chunk coordination.

## Development priorities

**Part I: simulation and technical gameplay.** M1, M2, M3, and M7 establish contracts, a sequential reference executor, parallel execution, and version-specific technical behavior. This first deliverable is headless; networking and storage are not prerequisites for validating its simulation.

**Part II: integration and additional capabilities.** M4, M5, M6, and M8 add client access, persistence, native and Java extensions, technical tools, optimization, and additional versions.

See [the roadmap](TODO.md) for milestones and [the executable reasoning models](docs/PARALLEL_MODELS.md) for the concurrency cases guiding Part I.

## Status

The headless Part I core is complete for its declared scope. It provides:

- shared contracts;
- reference rules for redstone (including redstone torches, buttons, pressure plates, target blocks, daylight detectors, note blocks, rails, coral fans, doors, trapdoors, fence gates, comparators and observers), pistons with slime and honey, flying engines, a named TNT duplicator, hoppers, double chests, dispensers and droppers, crafters, furnaces, smokers and blast furnaces, brewing stands, water, crops, fire, snow and ice, item entities, experience orbs, falling blocks, TNT and explosions, arrows, minecarts and boats, chunk tickets, light, and player inventories with survival use and container menus;
- a sequential scheduling oracle that shares the gameplay rules;
- a region-parallel runtime that matches the oracle across tested worker counts, region layouts and seeded schedules.

The rules are checked against traces recorded on unmodified Java Edition servers: 225 on 1.21.8 and 220 on 1.21.4 (every scenario but three that need a 1.21.8 game rule and one of 1.21.8's fire, which acts only near players; thirty-one of them need the official data packs, for random loot, recipes, brewing, experience or books), with identical results. Seventeen mechanisms claim exact behavior, including scheduled-tick order, block-event order, the update-chain limit, power through solid blocks, dust, repeaters, comparators, observers, hoppers, double chests, water and lava. The others declare their deviations, such as the random parts of piston drops. See [the simulation core](docs/SIMULATION.md) for the compatibility table and coverage limits.

Full vanilla compatibility remains a separate deliverable. The [completion roadmap](docs/VANILLA_COMPLETION.md) inventories the missing systems and version differences, including TNT/technical machines, full content, world generation, mobs, data packs and complete world interchange. Parts I and II describe the implemented foundation; Part III tracks completion against both versions.

The network server runs a playable creative and survival world for the Java Edition 1.21.8 or 1.21.4 client on top of the same engine, one version per server. It supports offline identities and online mode (encrypted, verified by the session service, refused when verification fails), and the real client is tested against it. In survival, blocks take their dig time and drop what the tool harvests, items are used up, thrown and picked up, chests (trapped chests powering by their viewers), ender chests, shulker boxes keeping their contents, barrels, composters, chiseled bookshelves, hoppers, furnaces, crafters, crafting tables, stonecutters, smithing tables, grindstones, looms, anvils, enchanting tables and lecterns open with every click mode, and movement, falls, hunger, death and respawning follow the target version; these were compared with the official servers of both versions. With the official server jar's data pack, the 2x2 grid and crafting tables craft, furnaces cook, and blocks drop from their loot tables, random rolls and fortune included. Mobs and eating are not supported; see [the network server](docs/SERVER.md).

Worlds are saved as checkpoints plus a write-ahead input log and recover every synced change after a crash. Java Edition 1.21.8 worlds can be imported and exported with every block state; blocks without rules of their own are inert, and other unsupported content is refused unless an explicit substitution policy is chosen. See [persistence](docs/PERSISTENCE.md).

Extensions see the world through controlled services: queries of the published state, commands admitted like player input, tick reports and permissions per module. There are two kinds:
- **Native modules:** mods and plugins, loaded in dependency order.
- **Java plugins:** these run in a separate JVM behind an authenticated socket. A JVM that crashes, stalls or floods the server is stopped while the server keeps ticking.

The technical tools (tick freeze and step, inspection, redstone traces, item counters) are themselves a native plugin. Circuit does not implement the Bukkit API; see [the evaluation](docs/BUKKIT_EVALUATION.md).

Checkpoints are encoded and written in the background from copy-on-write snapshots, and chunk packets are encoded in parallel. [Performance](docs/PERFORMANCE.md) reports tick-time percentiles, critical paths and coordination costs with the hardware and inputs used. [Releases and operation](docs/RELEASE.md) gives the supported scope, operating limits and recovery procedures.

The framework also provides numerical validation helpers, interface scaffolds and 13 abstract concurrency models.

## Build

Requires Linux, xmake 3.0+, a C++23 compiler with `std::expected` (GCC 13 verified), and standalone Asio 1.34.2. JDK 17+ is optional for the Java SPI check.

```bash
xmake f -m debug -y
xmake build -y --all
xmake run circuit-server --simulate --ticks 300 --workers 4
xmake run circuit-server --serve --offline --port 25565 --world "$PWD/worlds/main"
xmake run simulation_tests
```

For offline dependency configuration and all validation commands, see [the development guide](docs/DEVELOPMENT.md). API and usage documents are indexed in [docs](docs/README.md).

## License

Source is licensed under [AGPL-3.0-only](LICENSE). AGPL-compliant commercial use is permitted. Authorized rights holders may offer an alternative commercial agreement; see [the licensing policy](LICENSING.md).

Circuit is an independent project and is not affiliated with Mojang or Microsoft.
