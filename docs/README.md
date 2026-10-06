# Project documentation

| Document | Purpose |
| --- | --- |
| [Development guide](DEVELOPMENT.md) | Build, validation, repository layout, and contribution boundaries |
| [Headless simulation core](SIMULATION.md) | Contracts, reference rules, region-parallel runtime, verification, and limitations |
| [Network server](SERVER.md) | Running and configuring the server, identity, protocol data sources, limits and unsupported features |
| [Persistence](PERSISTENCE.md) | World directories, the durability and recovery protocol, and Anvil import and export |
| [Trace fixture format](TRACE_FORMAT.md) | circuit-trace v1 fixtures, content mapping, recording and evaluation |
| [Parallel reasoning models](PARALLEL_MODELS.md) | Executable concurrency cases, assumptions, and open questions |
| [Native plugin API](PLUGIN_API.md) | Modules, host services, permissions, load order, technical tools and ABI constraints |
| [Java bridge](../modules/java-bridge/README.md) | Java plugins in a separate JVM: SPI, configuration, isolation and protocol |
| [Bukkit evaluation](BUKKIT_EVALUATION.md) | Why Circuit does not claim Bukkit, Spigot or Paper compatibility |
| [Licensing policy](../LICENSING.md) | AGPLv3, alternative licensing, and third-party rights |
| [Roadmap](../TODO.md) | Part I core simulation and Part II additional capabilities |

Source and general guides are published on main. Key internal execution/design notes and local verification logs are intentionally excluded from Git. Public build and validation instructions do not depend on those documents.
