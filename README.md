# Circuit

Circuit is an experimental Minecraft Java Edition server written in C++23, built with xmake and standalone Asio. It explores chunk-level parallel simulation with explicit ownership and cross-chunk coordination.

## Development priorities

**Part I: simulation and technical gameplay.** M1, M2, M3, and M7 establish contracts, an independent reference world, parallel execution, and version-specific technical behavior. This first deliverable is headless; networking and storage are not prerequisites for validating its simulation.

**Part II: integration and additional capabilities.** M4, M5, M6, and M8 add client access, persistence, native and Java extensions, technical tools, optimization, and additional versions.

See [the roadmap](TODO.md) for milestones and [the executable reasoning models](docs/PARALLEL_MODELS.md) for the concurrency cases guiding Part I.

## Status

The current framework provides an Asio executor, synthetic simulation fixtures, Linux native shared-library plugins, numerical validation helpers, and module interfaces. It includes 13 abstract concurrency scenarios. Minecraft client login and playable worlds are not yet supported; model passes do not establish vanilla compatibility.

## Build

Requires Linux, xmake 3.0+, a C++23 compiler with `std::expected` (GCC 13 verified), and standalone Asio 1.34.2. JDK 17+ is optional for the Java SPI check.

```bash
xmake f -m debug -y
xmake build -y --all
xmake run circuit-server --demo --workers 4 --ticks 20
xmake run parallel_models
```

For offline dependency configuration and all validation commands, see [the development guide](docs/DEVELOPMENT.md). API and usage documents are indexed in [docs](docs/README.md).

## License

Source is licensed under [AGPL-3.0-only](LICENSE). AGPL-compliant commercial use is permitted. Authorized rights holders may offer an alternative commercial agreement; see [the licensing policy](LICENSING.md).

Circuit is an independent project and is not affiliated with Mojang or Microsoft.
