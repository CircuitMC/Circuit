# Native plugin and mod API

## Implemented surface

The current framework supports Linux shared libraries, C++23 inheritance interfaces, API/ABI checks, and controlled lifetime management. `IHost` provides logging and tick metrics by value. It does not expose writable world pointers or a complete command/event API.

Native modules are trusted process code. Library static initializers execute before descriptor validation, so ABI and capability checks do not make untrusted libraries safe.

## Implementing a module

Derive from `circuit::plugin::IPlugin` and implement the nonthrowing callbacks:

- `bool on_load(IHost&) noexcept`: acquire host capabilities and initialize the module.
- `void on_tick() noexcept`: observe a committed synthetic tick on the control thread.
- `void on_unload() noexcept`: stop and join all module-owned workers before returning.

Export `circuit_plugin_descriptor`, `circuit_plugin_create`, and `circuit_plugin_destroy` with the signatures in `include/circuit/plugin/api.hpp`. Descriptor data and its strings remain valid and immutable while the library is loaded. A failed factory returns null.

The module's destroy function releases its objects; the host does not perform cross-library `delete`. If `on_load` returns false, the loader still calls `on_unload`, then destroy, then closes the library. Cleanup must handle partially completed initialization.

`PluginLoader::load(path, host, phase)` returns `std::expected<LoadedPlugin, LoadError>`. The host must outlive every loaded plugin. Loading, callbacks, and unloading must be serialized. Temporary callback strings cannot be retained beyond their valid lifetime.

`LoadedPlugin` is move-only and unloads through RAII. Before a production loader permits unloading, all subscriptions, queued callbacks, and outstanding code references must be drained.

## ABI and registration phase

Descriptors contain structure size, API version, native ABI tag, module ID, and category. The current tag checks compiler version, standard-library identity, common libstdc++ ABI options, language mode, and pointer width. It detects common mismatches, not every incompatible compiler flag.

Host and modules must use compatible toolchains, targets, standard libraries, and SDK builds. C-linkage factories do not provide a compiler-independent C++ ABI. A future stable C function-table layer may wrap the inheritance API if required.

`PluginKind::Plugin` can be loaded during registration or running. `PluginKind::Mod` is accepted only during `LoadPhase::Registration`. Actual registry mutation, module dependency ordering, duplicate-ID checks, and production registration closure remain planned.

## Technical tools example

`examples/technical_tools/technical_tools.cpp` logs the most recent and mean simulation duration every 20 committed ticks. It exercises loading, callbacks, and host metrics without direct world access. These values do not represent a complete Minecraft tick benchmark.

The next capabilities are controlled freeze/step commands, chunk and entity diagnostics, event subscriptions, item-flow counters, and redstone traces. Implement each against the runtime ownership and permission boundaries before exposing it to extensions.

## Validation

`tests/plugin_tests.cpp` builds both a fixture library (with `CIRCUIT_PLUGIN_TEST_FIXTURE`) and a test executable. `xmake run plugin_tests` supplies the two library paths automatically.

Tests cover actual loading, metric sampling, move/lifetime behavior, module-side destruction, API/ABI rejection, factory failure, initialization cleanup, and mod phase restrictions. Fixture environment variables affect only test behavior; they are not production plugin controls.
