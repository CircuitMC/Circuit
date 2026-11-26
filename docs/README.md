# Project documentation

| Document | Purpose |
| --- | --- |
| [Development guide](DEVELOPMENT.md) | Build, validation, gates, worktrees, behavior sources and contribution boundaries |
| [Reference rules and oracle](SIMULATION.md) | Contracts, reference rules, the sequential oracle, verification and limitations |
| [Compatibility](COMPATIBILITY.md) | Protocol, content and behavior compatibility, stated separately |
| [Vanilla completion](VANILLA_COMPLETION.md) | Full 1.21.4/1.21.8 target, measured gaps and version differences |
| [Vanilla ledger](VANILLA_LEDGER.md) | Generated per-version status and evidence of every family, mechanic and enumerated resource |
| [Baselines](BASELINES.md) | Tick times of the official server, Paper and Folia on Circuit's workloads, measured from outside |
| [Persistence](PERSISTENCE.md) | World directories, the durability and recovery protocol, and Anvil import and export |
| [Protocol data](PROTOCOL_DATA.md) | Sources of packet ids, content tables, registries and layouts, their checks, and 1.21.4 differences |
| [Data packs](DATA_PACKS.md) | Loading the official jar's built-in pack and other data packs: precedence, tags, enchantments and block tools |
| [Trace fixture format](TRACE_FORMAT.md) | circuit-trace fixtures, content mapping, recording and evaluation |
| [Gates](../tools/gate/README.md) | Fail-closed checks for lanes, merges and stages |
| [Licensing policy](../LICENSING.md) | AGPLv3, the separate commercial license, and third-party rights |
| [Roadmap](../TODO.md) | The phases of the core restructuring |

Source and general guides are published on main. Internal execution and design notes and local verification logs are intentionally excluded from Git. Public build and validation instructions do not depend on those documents.
