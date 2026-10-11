# Native plugin and mod API

## Model

Native modules are Linux shared libraries that implement C++23 interfaces from `include/circuit/plugin/api.hpp` (API version 2). The server loads them at startup and calls them on its game thread between ticks, never inside the parallel tick.

Modules see the world as the engine published it after the last tick. They change it only through commands, which the server admits like player input: commands are logged for replay and applied at the start of a later tick. No module gets a pointer into the world.

**Trusted code.** Native modules are trusted process code. Library static initializers run before any check, and a module can do anything the process can. Descriptor, ABI and permission checks catch mistakes; they do not contain malicious code. For untrusted extensions, see the [Java bridge](../modules/java-bridge/README.md), which runs in a separate process, though it is not a sandbox either.

## Writing a module

Derive from `circuit::plugin::IPlugin`:

| Callback | When |
| --- | --- |
| `bool on_load(IHost&) noexcept` | Once at startup. Register commands and subscribe here. Returning false refuses the module. |
| `void on_tick_report(const TickReport&) noexcept` | After each committed tick, once subscribed: the tick's block changes and container slot changes, in order |
| `void on_tick() noexcept` | After each committed tick |
| `void on_unload() noexcept` | At shutdown, in reverse load order. Stop and join any threads the module owns. |

Export `circuit_plugin_descriptor`, `circuit_plugin_create` and `circuit_plugin_destroy` with C linkage. The descriptor declares:
- the API version and native ABI tag;
- the id: lowercase letters, digits, `.`, `_` and `-`, unique per server;
- the kind (`Plugin` or `Mod`);
- dependencies: a null-terminated list of ids;
- supported bundles, for example `"java-1.21.8"`; null means any.

Descriptor data stays valid while the library is loaded. The module's destroy function frees its object, so the host never deletes across libraries.

`examples/technical_tools/technical_tools.cpp` is a complete example.

## Host services

`IHost` answers with `Result<T>`, which is `std::expected<T, Status>`. The status is `denied` (permission missing), `invalid` (bad arguments, unknown block state, outside the world) or `unavailable` (not loaded, not now, or no world in this host).

| Permission | Services |
| --- | --- |
| none | `log`, `tick_metrics`, `tick_status` (tick number, frozen state, recent tick times with p50/p95/p99), `permissions` |
| `world.read` | `time`, `block` (state notation), `block_entity` (kind, slots, comparator signal), `chunk` (loaded, ticking: whether its scheduled ticks run, counts), `items` (item entities in a chunk) |
| `world.write` | `set_block`, `use_block` (each returns the admission number), `cancel` (withdraws this module's command before it is applied) |
| `world.observe` | `subscribe`: tick reports from now on |
| `commands` | `register_command` (a chat command with a handler object owned by the module), `tell` (a message to an online player) |
| `server.control` | `freeze`, `step(n)`: while frozen, ticks do not run; inputs wait, and players can still move |

**Block notation.** Block states use the game's notation restricted to the properties the rules model: `minecraft:repeater[delay=2,facing=west,powered=false]`, or `minecraft:redstone_wire[power=15]` without connections. `set_block` accepts the full notation and ignores display-only properties.

**Permissions.** Granted per module in the server configuration. A module without an entry gets `world.read` and `world.observe`:

```text
plugin=/srv/circuit/plugins/libtechnical_tools.so
plugin-permissions.circuit.technical-tools=world.read,world.observe,server.control,commands
```

## Loading, mods and registration

At startup the server opens every configured library and checks its descriptor:
- the API version and ABI tag;
- the id;
- dependencies and supported bundles.

The server then:
1. orders the mods so that each comes after its dependencies (ties by id), and starts them in the registration phase;
2. closes registration;
3. orders and starts the plugins, which may depend on mods and on each other.

The server refuses to start when:
- an id is duplicated or invalid;
- a dependency is missing;
- the dependencies form a cycle;
- a module does not support the server's bundle;
- a mod depends on a plugin;
- a module's `on_load` fails.

**Mods.** A mod (`PluginKind::Mod`) may register commands only while registration is open. Afterwards the registry is frozen for mods and further registrations return `unavailable`. Command names cannot replace built-in commands or another module's command.

The content registries (blocks, items, rules) are fixed by the rule set and cannot be extended by modules. A mod that needs new content needs a new rule set.

Modules are unloaded in reverse order. A module's commands are removed before its library closes, so no callback can reach unloaded code.

## ABI

The ABI tag (`circuit-v2;compiler;standard library;...`) catches common toolchain mismatches, not every incompatible flag. Host and modules must use compatible toolchains and standard libraries. Values such as `std::string` and `std::vector` cross the boundary. A stable C function table may wrap the interface later if modules must be built separately from the server.

`circuit::plugin::HostWithoutWorld` implements every world service as `unavailable`. Tools and tests use it; `--demo` runs the technical tools on such a host.

## Technical tools

The example plugin `circuit.technical-tools` uses only the services above:

| Command | Effect | Permissions |
| --- | --- | --- |
| `/tick freeze`, `/tick unfreeze`, `/tick step [n]` | Stop, resume or step the simulation | `server.control` |
| `/tick query` | Tick number, frozen state and recent tick times (mean, p50, p95, p99, max) | none |
| `/inspect block <x> <y> <z>` | State and block entity of a block | `world.read` |
| `/inspect chunk <cx> <cz>` | Load level, block entities and entities of a chunk | `world.read` |
| `/trace redstone on\|off` | Each tick, sends the redstone changes to the player | `world.observe` |
| `/counter add\|remove <x> <y> <z>`, `/counter list\|reset` | Counts items entering containers, with hourly rates | `world.read`, `world.observe` |

All commands also need `commands`. Every minute it logs the tick time.

## Validation

- **`plugin_tests`** loads the technical tools on a host without a world and a fixture library. It covers:
  - loading, tick reports, move and lifetime behavior, and module-side destruction;
  - rejection of a bad API version, ABI tag, id or dependency list, a factory or initialization failure, and a mod after registration;
  - descriptor dependencies and versions;
  - load order with missing dependencies, cycles, duplicates and version mismatches.
- **`extension_tests`** runs real servers. It covers:
  - mods loading before the plugins that depend on them, with the registry frozen afterwards;
  - commands, withdrawals, queries and tick reports through a probe plugin;
  - per-module permissions;
  - the technical tools: freeze, step, inspection and traces;
  - the Java bridge (see its README).
