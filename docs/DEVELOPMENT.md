# Development guide

## Workspace

Run commands from the Circuit repository root. Source, tests, build files, and public guides are tracked on main.

```text
Circuit/
  xmake.lua                 Build entry point
  app/                      Application startup and composition
  include/circuit/          Module interfaces and runtime contracts
  src/contracts/            Version bundles, compatibility table, trace and replay records
  src/gameplay/             Reference rules (circuit-reference-rules-v7)
  src/runtime/              Region-parallel engine, job runners, executor, synthetic kernel
  src/evidence/             circuit-trace fixture format, content mapping, recording and evaluation
  src/protocol/             Wire codec: data types, NBT, framing, packet and content tables
  src/auth/                 Identity: offline identities, encryption, session verification
  src/storage/              Checkpoints, the input log and world directories
  src/server/               Network server: sessions, version adapter, the authoritative game loop,
                            Anvil import and export
  src/plugin/               Native shared-library loader and load order
  examples/technical_tools/ Technical tools as a native plugin
  modules/java-bridge/      Java SPI, bridge runtime and example plugin
  src/exploration/          Standalone reasoning models, not linked into the server
  tests/                    Framework, rule, differential and parallel-reasoning checks
  tests/oracle/             Independent sequential executor; never linked into the server
  tests/fixtures/           circuit-trace skeletons, reference recordings, target-version traces,
                            documented fixtures and profiling workloads
  tools/                    Local build/check, snapshot, publication guard and workload generator
  docs/                     Public guides and explicitly ignored internal notes
  LICENSE                   Unmodified AGPLv3 text
  README.md, TODO.md         Public overview and roadmap
  .local/                   Local archives and publication hook
```

Empty directories are not implementation milestones. The simulation core is described in [the simulation guide](SIMULATION.md), persistence in [its guide](PERSISTENCE.md).

## Requirements

- Linux x86_64 for the current native plugin loader.
- xmake 3.0 or later and a C++23 compiler with `std::expected`; GCC 13 is verified. Clang 18 with libstdc++ 13 does not provide `std::expected`; use GCC, Clang 19 or later, or libc++.
- Standalone Asio 1.34.2; Boost.Asio is not used.
- Bash, Python 3 and standard POSIX utilities for local maintenance.
- JDK 17 or later for the optional Java SPI compilation check.

## Build and run

```bash
export XMAKE_GLOBALDIR="$PWD/.xmake-global"
xmake f -m debug -y
xmake build -y --all
xmake run circuit-server --simulate --ticks 300 --workers 4
xmake run circuit-server --demo --workers 4 --ticks 20
```

`--simulate` runs the headless core; `--demo` runs the earlier synthetic scheduler fixture. The default dependency path resolves the pinned Asio package through xmake. For offline builds, provide an existing standalone Asio include directory:

```bash
export CIRCUIT_ASIO_INCLUDE=/path/to/asio/include
bash tools/check.sh
```

After configuration, run the plugin demo:

```bash
export XMAKE_GLOBALDIR="$PWD/.xmake-global"
xmake run circuit-server --demo --game-version java-1.21.8 --workers 4 --ticks 20 \
  --plugin "$PWD/build/linux/x86_64/debug/libtechnical_tools.so"
```

`--list-versions` lists the version candidates with their protocol and play support; `--version` returns the public build identity. Neither mode opens a network listener. `--serve` runs the network server; see [the network server](SERVER.md).

## Validation

`bash tools/check.sh` configures debug mode, builds all targets, and runs the framework, contract, rule, differential simulation, control and parallel-model test programs. It also compiles the Java SPI/example when `javac` is installed; a missing JDK is reported as a skip. Use focused targets during development, then run the relevant integration checks before marking a milestone complete:

```bash
xmake run contract_tests    # identities, coordinates, bundles, compatibility claims, replay limits
xmake run rules_tests       # reference rules against hand-derived expectations
xmake run simulation_tests  # runtime against the oracle across workers, layouts and seeds
xmake run control_tests     # seeded schedules, tick clock, digest
xmake run evidence_tests    # trace fixtures, content mapping, recordings, target-version traces
xmake run protocol_tests    # wire codec, NBT, framing limits, packet and content tables
xmake run storage_tests     # checkpoints, the input log, crash and I/O-error injection
xmake run anvil_tests       # Anvil import and export, unsupported-content policies
xmake run server_tests      # in-process server: login, encryption, limits, player actions, saving
xmake run extension_tests   # native modules, permissions, technical tools, the Java bridge (needs a JDK)
```

`evidence_tests` re-records every skeleton in `tests/fixtures/skeletons/` and compares the result with `tests/fixtures/reference/`. When a rule change is intended, re-record with `--record-fixture`, review the diff, and commit both. See [the trace fixture format](TRACE_FORMAT.md).

The xmake modes include `debug`, `release`, `asan`, `ubsan`, and `tsan`. The `ubsan` mode stops at the first diagnostic, so a completed test program had no undefined-behavior report. Run `plugin_tests` with the paths of `libtechnical_tools.so` and `libplugin_fixture.so` from the same build directory; `xmake run` passes them automatically. On kernels with high mmap randomization, ThreadSanitizer stops before tests run with an unexpected-memory-mapping error. Disable ASLR for the test process only; do not change host settings:

```bash
export XMAKE_GLOBALDIR="$PWD/.xmake-global"
xmake f -m tsan --toolchain=gcc -y
xmake build runtime_tests simulation_tests
setarch "$(uname -m)" -R ./build/linux/x86_64/tsan/runtime_tests
setarch "$(uname -m)" -R ./build/linux/x86_64/tsan/simulation_tests
```

GCC warns that ThreadSanitizer does not model the `std::atomic_thread_fence` that Asio uses. A clean run is evidence about the engine's own accesses, not a proof that Asio internals are race-free. Add `--asio_include=/path/to/asio/include` to the configure step for offline builds, and run `bash tools/check.sh` afterwards to restore the debug configuration. Neither sanitizer runs nor differential tests prove vanilla gameplay compatibility.

## Behavior sources

Implement Minecraft behavior from specifications, observed traces, and test fixtures. Do not copy, translate, or paraphrase decompiled, deobfuscated, or mapped game source into this repository. Reference traces come from an instrumented target-version server maintained outside this repository. Committed fixtures record their source and target version and are reviewed for third-party content. This is a project contribution rule, not legal advice.

### Target-version harness

The harness is a separate project. Running it requires accepting the Minecraft EULA, and the harness may use the target version's official mappings internally; neither the server, the mappings nor harness code enters this repository. For each skeleton, a harness must:

- start the unmodified target-version server on a fresh void world with a fixed seed, and set `max-chained-neighbor-updates` from the fixture's `neighbor-update-limit`;
- keep the area's chunks plus one chunk of margin force-loaded, as described in [the trace format](TRACE_FORMAT.md);
- apply the actions of tick `t` before the server runs tick `t`, as player inputs would be;
- record every block state change inside the area at the moment it is written, including intermediate states, and read probes after the tick ends;
- write a `harness_trace` fixture with its own build identity in the `harness` key.

Copy only the resulting trace files into `tests/fixtures/harness/` (1.21.8), or into `tests/fixtures/harness-1.21.4/` for a 1.21.4 build of the harness, under the name of the skeleton they came from. Review them before committing. Each trace names the game version of the server that recorded it. `evidence_tests` then requires every trace to pass on the engine and the oracle, under the bundle of its version.

The harness can also time the target version's tick phases for a workload. Compare that profile with Circuit's own on a release build:

```bash
python3 tools/generate_workloads.py
xmake f -m release -y
xmake build circuit-server
xmake run circuit-server --profile-fixture "$PWD/tests/fixtures/workloads/dust_clocks.ctrace" --ticks 600 --workers 8
```

The profile reports tick-time percentiles, rule work, the critical path and coordination costs; [performance](PERFORMANCE.md) explains them. `storage_bench` and `chunk_bench` (`tools/bench/`) measure checkpoint costs and chunk-packet encoding on a release build.

Run `bash tools/check.sh` afterwards to restore the debug configuration. `xmake run circuit-server --compatibility` prints the compatibility table that these traces support. Change a claimed tier only with the evidence that `contract_tests` requires: an L0 claim needs target-version traces and no known gap, and an L2 claim lists its gaps.

## Client testing

The server is also checked with two clients that live outside this repository. Neither is a build dependency.

**Protocol library.** Mineflayer scripts (Node.js) cover:
- creative placement and breaking, dust powering a lamp, a piston push and a piston destroying dust into an item entity, player visibility and chat (`suite.js`, two bots);
- chest placement and pairing (`chests.js`);
- container windows (`windows.js`): every click mode in a chest, the own window, a double chest and a hopper, thrown items and pickups. Each step prints the window, carried stack and inventory the server reports;
- vitals (`vitals.js`) and collision (`collision.js`): scripted moves with mineflayer's physics off, printing the corrections, health, hunger, deaths and kicks the server sends.

Calibrate each script against an offline-mode vanilla server of the same version with a fresh flat world, with the bots listed as operators. Then run it against Circuit (set `MC_VERSION=1.21.4` and `game-version=java-1.21.4` for the second version) and compare the outputs:

```bash
xmake run circuit-server --serve --offline --port 25580 &
node suite.js 25580 -61
```

**Real client.** A Fabric client mod for the 1.21.8 client, with a 1.21.4 build of the same mod, runs a step script through Loom's development launch. The client joins the server with quick play, and the script places blocks, uses them, and checks the block states and entities the client shows. It also takes screenshots and writes a report. Scripts are plain text:

```text
wait_world
give 0 minecraft:lever
use 2 -61 2 up
use 2 -60 2 up
expect 3 -60 2 minecraft:redstone_lamp[lit=true] 40
```

Movement steps (`hold <key> <ticks>`, `expect_pos`, `expect_health`, `expect_dead`, `respawn`) let a script walk, jump and sprint with the client's own physics; `survival.txt` uses them to check that the server accepts real movement and applies falls, death and respawning.

The real client needs a display. In development it logs in with an offline name; online mode needs a real account and is a manual check. Set `pauseOnLostFocus:false` in the client options so that a window without focus keeps running.

Both clients use the official server and client under the Minecraft EULA. Like the trace harness, they stay outside this repository.

## Source and documentation boundaries

Commit ordinary source, tests, examples, build files, license material, and public guides normally. Part I (M1, M2, M3, M7) has priority; Part II covers networking, persistence, extensions, and optimization.

The ignore file names the internal documents kept local: agent instructions, execution plan, architecture, verification log, `docs/internal/`, and the incomplete commercial agreement. Normal build output, local tool state, archives, runtime worlds, logs, and credentials are also excluded. This does not prevent publishing source or normal documentation.

Before a source commit or push, inspect the actual file list and diff:

```bash
git status --short
git diff --cached --check
git diff --cached --stat
python3 tools/check_public_tree.py --staged
```

`tools/check_public_tree.py` checks the current commit by default. In `--pre-push` mode it checks new reachable commit trees against the current ignore policy. This is an accidental-publication guard, not a secret scanner. Ignore rules do not remove material already in Git history.

## Local document backups

Normal source history is preserved in Git. Optional local snapshots also protect ignored working documents:

```bash
python3 tools/snapshot.py
```

Snapshots include a SHA-256 manifest and are stored under `.local/snapshots/`. They may contain internal documents, so keep them private. They are not off-machine backups and must not be committed or pushed.

All project documentation and source comments use English. Public documents describe project behavior, scope, and usage; private execution notes are not required to build or run the repository.
