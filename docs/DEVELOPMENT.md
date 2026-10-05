# Development guide

## Workspace

Run commands from the Circuit repository root. Source, tests, build files, and public guides are tracked on main.

```text
Circuit/
  xmake.lua                 Build entry point
  app/                      Application startup and composition
  include/circuit/          Module interfaces and runtime contracts
  src/contracts/            Version bundles, compatibility table, trace and replay records
  src/gameplay/             Reference rules (circuit-reference-rules-v0)
  src/runtime/              Region-parallel engine, job runners, executor, synthetic kernel
  src/evidence/             circuit-trace fixture format, content mapping, recording and evaluation
  src/plugin/               Native shared-library loader
  examples/technical_tools/ Native timing plugin
  modules/java-bridge/      Java SPI and example sources
  src/exploration/          Standalone reasoning models, not linked into the server
  tests/                    Framework, rule, differential and parallel-reasoning checks
  tests/oracle/             Independent sequential executor; never linked into the server
  tests/fixtures/           circuit-trace skeletons, reference recordings and documented fixtures
  tools/                    Local build/check and snapshot commands
  docs/                     Public guides and explicitly ignored internal notes
  LICENSE                   Unmodified AGPLv3 text
  README.md, TODO.md         Public overview and roadmap
  .local/                   Local archives and publication hook
```

Add `src/protocol/`, `src/auth/`, and `src/storage/` when their first implementations land; those headers describe interfaces, not completed services. Empty directories are not implementation milestones. The simulation core is described in [the simulation guide](SIMULATION.md).

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

`--list-versions` lists planned candidates; `--version` returns public build identity. Neither mode opens a network listener. Starting without `--simulate` or `--demo` fails because playable protocol support is not implemented.

## Validation

`bash tools/check.sh` configures debug mode, builds all targets, and runs the framework, contract, rule, differential simulation, control and parallel-model test programs. It also compiles the Java SPI/example when `javac` is installed; a missing JDK is reported as a skip. Use focused targets during development, then run the relevant integration checks before marking a milestone complete:

```bash
xmake run contract_tests    # identities, coordinates, bundles, replay limits
xmake run rules_tests       # reference rules against hand-derived expectations
xmake run simulation_tests  # runtime against the oracle across workers, layouts and seeds
xmake run control_tests     # seeded schedules, tick clock, digest
xmake run evidence_tests    # trace fixtures, content mapping, committed fixtures
```

`evidence_tests` re-records every skeleton in `tests/fixtures/skeletons/` and compares the result with `tests/fixtures/reference/`. When a rule change is intended, re-record with `--record-fixture`, review the diff, and commit both. See [the trace fixture format](TRACE_FORMAT.md).

The xmake modes include `debug`, `release`, `asan`, and `tsan`. On kernels with high mmap randomization, ThreadSanitizer stops before tests run with an unexpected-memory-mapping error. Disable ASLR for the test process only; do not change host settings:

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
