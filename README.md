# Circuit

Circuit is an experimental Minecraft Java Edition server written in C++23, built with xmake and standalone Asio. It explores chunk-level parallel simulation with explicit ownership and cross-chunk coordination.

## Development priorities

**Part I: simulation and technical gameplay.** M1, M2, M3, and M7 establish contracts, an independent reference world, parallel execution, and version-specific technical behavior. This first deliverable is headless; networking and storage are not prerequisites for validating its simulation.

**Part II: integration and additional capabilities.** M4, M5, M6, and M8 add client access, persistence, native and Java extensions, technical tools, optimization, and additional versions.

See [the roadmap](TODO.md) for milestones and [the executable reasoning models](docs/PARALLEL_MODELS.md) for the concurrency cases guiding Part I.

## Status

The headless Part I core is complete for its declared scope. It provides:

- shared contracts;
- reference rules for redstone (including comparators and observers), pistons, hoppers and double chests, water, crops, item entities and chunk tickets;
- an independent sequential oracle;
- a region-parallel runtime that matches the oracle exactly across worker counts, region layouts and seeded schedules.

The rules are checked against 20 traces recorded on an unmodified Java Edition 1.21.8 server. Eight mechanisms claim exact behavior, including scheduled-tick order, block-event order, the update-chain limit, observers and double chests. The others declare their deviations, such as no power through solid blocks. See [the simulation core](docs/SIMULATION.md) for the compatibility table and coverage limits.

The network server runs a playable creative-mode world for the Java Edition 1.21.8 client on top of the same engine. It supports offline identities and online mode (encrypted, verified by the session service, refused when verification fails), and the real client is tested against it. Survival gameplay and container screens are not yet supported; see [the network server](docs/SERVER.md).

Worlds are saved as checkpoints plus a write-ahead input log and recover every synced change after a crash. Java Edition 1.21.8 worlds can be imported and exported for the content the rules model; other content is refused unless an explicit substitution policy is chosen. See [persistence](docs/PERSISTENCE.md).

The framework also provides Linux native shared-library plugins, numerical validation helpers, interface scaffolds, and 13 abstract concurrency models.

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
