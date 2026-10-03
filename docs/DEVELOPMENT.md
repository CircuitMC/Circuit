# Development guide

## Workspace

Run commands from the Circuit repository root. Source, tests, build files, and public guides are tracked on main.

```text
Circuit/
  xmake.lua                 Build entry point
  app/                      Application startup and composition
  include/circuit/          Module interfaces and runtime contracts
  src/runtime/              Executor and synthetic kernel
  src/plugin/               Native shared-library loader
  examples/technical_tools/ Native timing plugin
  modules/java-bridge/      Java SPI and example sources
  src/exploration/          Standalone reasoning models, not linked into the server
  tests/                    Framework and parallel-reasoning checks
  tools/                    Local build/check and snapshot commands
  docs/                     Public guides and explicitly ignored internal notes
  LICENSE                   Unmodified AGPLv3 text
  README.md, TODO.md         Public overview and roadmap
  .local/                   Local archives and publication hook
```

Add `src/gameplay/`, `src/protocol/`, `src/auth/`, and `src/storage/` when their first implementations land; the current headers describe interfaces, not completed services. Empty directories are not implementation milestones.

## Requirements

- Linux x86_64 for the current native plugin loader.
- xmake 3.0 or later and a C++23 compiler with `std::expected`; GCC 13 is verified.
- Standalone Asio 1.34.2; Boost.Asio is not used.
- Bash and Python 3 for local maintenance. `rg` is used by the Java source check.
- JDK 17 or later for the optional Java SPI compilation check.

## Build and run

```bash
export XMAKE_GLOBALDIR="$PWD/.xmake-global"
xmake f -m debug -y
xmake build -y --all
xmake run circuit-server --demo --workers 4 --ticks 20
```

The default dependency path resolves the pinned Asio package through xmake. For offline builds, provide an existing standalone Asio include directory:

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

`--list-versions` lists planned candidates; `--version` returns public build identity. The demo opens no network listener. Starting without `--demo` fails because playable protocol support is not implemented.

## Validation

`bash tools/check.sh` configures debug mode, builds all targets, runs the framework and parallel-model test programs, and compiles the Java SPI/example when `javac` is installed. A missing JDK is reported as a skip. Use focused targets during development, then run the relevant integration checks before marking a milestone complete.

The xmake modes include `debug`, `release`, `asan`, and `tsan`. Sanitizer availability depends on the compiler and host. GCC TSan previously failed before test execution with an unexpected-memory-mapping error on the development host; it has not supplied evidence of race freedom. Neither arithmetic demo tests nor a clean sanitizer run proves vanilla gameplay compatibility.

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
