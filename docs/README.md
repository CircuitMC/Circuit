# Project documentation

| Document | Purpose |
| --- | --- |
| [Development guide](DEVELOPMENT.md) | Build, validation, repository layout, and contribution boundaries |
| [Headless simulation core](SIMULATION.md) | Contracts, reference rules, region-parallel runtime, verification, and limitations |
| [Compatibility](COMPATIBILITY.md) | Protocol, content, behavior and extension compatibility, stated separately |
| [Vanilla completion](VANILLA_COMPLETION.md) | Full 1.21.4/1.21.8 target, measured gaps, version differences, C0–C12 milestones and acceptance gates |
| [Vanilla ledger](VANILLA_LEDGER.md) | Generated per-version status and evidence of every family, mechanic and enumerated resource |
| [Network server](SERVER.md) | Running and configuring the server, identity, protocol data sources, limits and unsupported features |
| [Persistence](PERSISTENCE.md) | World directories, the durability and recovery protocol, and Anvil import and export |
| [Data packs](DATA_PACKS.md) | Loading the official jar's built-in pack and other data packs: precedence, tags, enchantments, block tools and the world's data fingerprint |
| [Storage evaluation](STORAGE_EVALUATION.md) | Measured checkpoint sizes and costs, compression, background checkpoints and deltas |
| [Performance](PERFORMANCE.md) | Tick-time percentiles, critical paths and coordination costs, and the optimizations made |
| [Trace fixture format](TRACE_FORMAT.md) | circuit-trace v1 fixtures, content mapping, recording and evaluation |
| [Parallel reasoning models](PARALLEL_MODELS.md) | Executable concurrency cases, assumptions, and open questions |
| [Native plugin API](PLUGIN_API.md) | Modules, host services, permissions, load order, technical tools and ABI constraints |
| [Java bridge](../modules/java-bridge/README.md) | Java plugins in a separate JVM: SPI, configuration, isolation and protocol |
| [Bukkit evaluation](BUKKIT_EVALUATION.md) | Why Circuit does not claim Bukkit, Spigot or Paper compatibility |
| [Releases and operation](RELEASE.md) | Release checklist and provenance, supported scope, operating limits, upgrades and recovery |
| [Licensing policy](../LICENSING.md) | AGPLv3, alternative licensing, and third-party rights |
| [Roadmap](../TODO.md) | Parts I/II foundation and Part III full vanilla compatibility |

Source and general guides are published on main. Key internal execution/design notes and local verification logs are intentionally excluded from Git. Public build and validation instructions do not depend on those documents.
