# Circuit

Circuit is an experimental Minecraft Java Edition server written in C++23, built with xmake and standalone Asio. It explores chunk-level parallel simulation with explicit ownership and cross-chunk coordination.

## Development priorities

**Part I: simulation and technical gameplay.** M1, M2, M3, and M7 establish contracts, an independent reference world, parallel execution, and version-specific technical behavior. This first deliverable is headless; networking and storage are not prerequisites for validating its simulation.

**Part II: integration and additional capabilities.** M4, M5, M6, and M8 add client access, persistence, native and Java extensions, technical tools, optimization, and additional versions.

See [the roadmap](TODO.md) for milestones and [the executable reasoning models](docs/PARALLEL_MODELS.md) for the concurrency cases guiding Part I.

## Status

The headless Part I core is implemented for Circuit's own reference rules: shared contracts, redstone, pistons, hoppers, water, crops, item entities and chunk tickets, an independent sequential oracle, and a region-parallel runtime that matches the oracle exactly across worker counts, region layouts and seeded schedules. These rules are written from documented behavior and are not yet verified against Java Edition 1.21.8; no mechanism claims exact vanilla compatibility. See [the simulation core](docs/SIMULATION.md).

The framework also provides Linux native shared-library plugins, numerical validation helpers, interface scaffolds, and 13 abstract concurrency models. Minecraft client login, playable worlds and persistence are not yet supported.

## Build

Requires Linux, xmake 3.0+, a C++23 compiler with `std::expected` (GCC 13 verified), and standalone Asio 1.34.2. JDK 17+ is optional for the Java SPI check.

```bash
xmake f -m debug -y
xmake build -y --all
xmake run circuit-server --simulate --ticks 300 --workers 4
xmake run simulation_tests
```

For offline dependency configuration and all validation commands, see [the development guide](docs/DEVELOPMENT.md). API and usage documents are indexed in [docs](docs/README.md).

## License

Source is licensed under [AGPL-3.0-only](LICENSE). AGPL-compliant commercial use is permitted. Authorized rights holders may offer an alternative commercial agreement; see [the licensing policy](LICENSING.md).

Circuit is an independent project and is not affiliated with Mojang or Microsoft.
