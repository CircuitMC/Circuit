# Development guide

## Workspace

Run commands from the Circuit repository root. Source, tests, build files, and public guides are tracked on main.

```text
Circuit/
  xmake.lua                 Build entry point
  include/circuit/          Module interfaces and contracts
  src/contracts/            Version bundles, compatibility table, trace and replay records
  src/gameplay/             Reference rules (circuit-reference-rules-v12) behind RuleContext
  src/evidence/             circuit-trace fixture format, content mapping, recording and evaluation
  src/protocol/             Wire codec: data types, NBT, framing, packet and content tables
  src/auth/                 Login cryptography, offline identities, the JSON reader
  src/data/                 Data packs: the official jar's built-in pack, directories and archives
  src/storage/              Checkpoints, the input log and world directories
  src/server/               Kept parts of the first generation's server: data compilers, registries,
                            packet layouts, block palette, Anvil import and export
  tests/                    Test programs, one directory each
  tests/oracle/             Sequential executor in vanilla order; the specification of the runtime
  tests/fixtures/           circuit-trace skeletons, reference recordings, target-version traces,
                            documented fixtures and profiling workloads
  tools/                    Checks, gates, worktree slots, generators, the fixture tool, the ledger
  compatibility/            Ledger sources and work packages
  docs/                     Public guides and explicitly ignored internal notes
  LICENSE                   Unmodified AGPLv3 text
  README.md, TODO.md        Public overview and roadmap
  .local/                   Local archives and publication hook
```

The first generation's runtime, network server, player code and extensions were removed in the restructuring of November 2026; they remain reachable at the tag `pre-refactor`. The rules and the oracle are described in [the simulation guide](SIMULATION.md), persistence in [its guide](PERSISTENCE.md).

## Requirements

- Linux x86_64.
- xmake 3.0 or later and a C++23 compiler with `std::expected`; GCC 13 is verified. Clang 18 with libstdc++ 13 does not provide `std::expected`; use GCC, Clang 19 or later, or libc++.
- zlib and OpenSSL 3 development files.
- Bash, Python 3 and standard POSIX utilities for local maintenance.
- PACE, the network library, comes as the xmake package `pace` at the exact release `xmake.lua` names (0.6.0), with TLS on and HTTP and WebSocket off. Its OpenSSL dependency is resolved to the system library, so the process has one OpenSSL. In the `asan`, `ubsan` and `tsan` modes PACE is built with the same sanitizer, as a separate package build. Several worktrees share one package cache (`XMAKE_GLOBALDIR`); configure each mode once, one at a time, before lanes build in parallel.

## Build

```bash
export XMAKE_GLOBALDIR="$PWD/.xmake-global"
xmake f -m debug -y
xmake build -y --all
```

There is no server binary in this state. `circuit-fixture` records, checks and compares trace fixtures on the oracle:

```bash
xmake run circuit-fixture --check-fixture "$PWD/tests/fixtures/harness/hopper_chain.ctrace"
xmake run circuit-fixture --record-fixture "$PWD/tests/fixtures/skeletons/hopper_chain.ctrace" --output /tmp/hopper.ctrace
xmake run circuit-fixture --compatibility --game-version java-1.21.4
```

`--vanilla-jar <jar>` (and `--data-pack <dir>`) runs the rules with the data packs, as scenarios of loot tables need.

## Validation

`bash tools/check.sh` configures debug mode, builds all targets and runs the test programs one after another. The test programs are the xmake targets whose names end in `_tests`; every one of them must be listed in `GATE_PROGRAMS` (`tools/gate/lib.sh`), and every check and gate fails when the two differ. It validates the [compatibility ledger](../compatibility/README.md) and tests its validator, so a status change in the source data must come with a regenerated ledger (`python3 tools/compat_ledger.py generate`). The programs:

```bash
xmake run contract_tests    # identities, coordinates, bundles, compatibility claims, replay limits
xmake run net_tests         # the network layer; first, the PACE package as Circuit consumes it
xmake run rules_tests       # reference rules on the oracle against hand-derived expectations
xmake run simulation_tests  # the engine under test against the oracle: 41 scenarios, restores, 64 random seeds
xmake run evidence_tests    # trace fixtures, content mapping, recordings, target-version traces
xmake run protocol_tests    # wire codec, NBT, framing limits, packet and content tables
xmake run data_tests        # data pack loading, precedence and archives
xmake run content_tests     # data compilers, registries, block palette, login cryptography
xmake run storage_tests     # checkpoints, the input log, crash and I/O-error injection
xmake run anvil_tests       # Anvil import and export, unsupported-content policies
```

`storage_tests`, `anvil_tests` and `simulation_tests` run their worlds on the engine under test, behind the engine facade (`include/circuit/runtime/engine.hpp`); `tests/support/engine_under_test.hpp` picks it, by `CIRCUIT_ENGINE` (`oracle`, the default and so far the only one). `simulation_tests` compares that engine with the oracle tick by tick: traces, states, change feeds and restores. Cases that read the official jars skip without them, unless `CIRCUIT_REQUIRE_JARS=1` makes them fail.

`evidence_tests` re-records every skeleton in `tests/fixtures/skeletons/` and compares the result with `tests/fixtures/reference/`. When a rule change is intended, re-record with `circuit-fixture --record-fixture`, review the diff, and commit both. See [the trace fixture format](TRACE_FORMAT.md).

The xmake modes include `debug`, `release`, `asan`, `ubsan`, and `tsan`. The `ubsan` mode stops at the first diagnostic, so a completed test program had no undefined-behavior report. On kernels with high mmap randomization, ThreadSanitizer stops before tests run with an unexpected-memory-mapping error. Disable ASLR for the test process only; do not change host settings:

```bash
xmake f -m tsan --toolchain=gcc -y
xmake build storage_tests
setarch "$(uname -m)" -R ./build/linux/x86_64/tsan/storage_tests
```

Run `bash tools/check.sh` afterwards to restore the debug configuration. Neither sanitizer runs nor differential tests prove vanilla gameplay compatibility.

### Gates

`tools/check.sh` has two further modes that end with one line per step and a non-zero exit status when any step fails or does not run:

```bash
bash tools/check.sh --lane   # incremental debug build with zero warnings, every test program side by side
bash tools/check.sh --full   # the same plus the ledger checks
```

Both require the official server jars (`CIRCUIT_VANILLA_JAR_1_21_8`, `CIRCUIT_VANILLA_JAR_1_21_4`), so the cases that read them fail instead of being skipped. `--jobs` and `--test-jobs` set the compile jobs and the number of test programs run at a time (6 each by default); `--rebuild` compiles every file again so that every file can warn. `evidence_tests`, `simulation_tests` and `storage_tests`, which take minutes, run as several shards each (`CIRCUIT_TEST_SHARD`).

The tiers built on this are in `tools/gate/` and described in [its guide](../tools/gate/README.md):

| Tier | When | Command |
| --- | --- | --- |
| T1 | every step, in a lane's worktree | `tools/gate/t1.sh [scenario...]`: the lane check plus the named client scenarios (once a server exists again) |
| T2 | every merge into `main` | `tools/gate/t2.sh --tree <checkout> --work <directory>`: copies of one commit built clean in debug with zero warnings and under ASan, UBSan and TSan, every program in each |
| T3 | nightly on `main` | T2 plus every client scenario |
| T4 | end of a stage | T3 plus the stage's exit conditions |

A sanitizer run that stops before its tests is a failed step. Client scenarios compare a probe's output with exactly one pinned official reference per scenario and version; `tools/gate/scenarios.py` reads the pins from a manifest and takes the probe and reference directories as arguments, because both stay outside this repository.

## Worktrees

Parallel lanes work in up to four linked worktrees next to the main checkout; the main checkout integrates. `tools/worktree.sh` manages them:

```bash
bash tools/worktree.sh add 2 net        # ../Circuit-wt-2 on branch lane/net, configured for debug
bash tools/worktree.sh list             # slots, branches, state, port ranges, hook status
bash tools/worktree.sh env 2            # the slot's exports
bash tools/worktree.sh retarget 2 world # switch a clean slot to another lane branch
bash tools/worktree.sh remove 2         # refused when the slot is dirty or has unpushed commits
```

`add` creates `lane/<lane>` from `main` or attaches the branch when it exists, and refuses a fifth worktree. It writes a git-ignored `.circuit-worktree.env` into the worktree, which `tools/check.sh --lane` and the gates read:

| Variable | Meaning |
| --- | --- |
| `CIRCUIT_MAIN` | the main checkout; documents that are not tracked are read from there and never copied |
| `CIRCUIT_SLOT`, `CIRCUIT_LANE` | the slot number and the lane |
| `CIRCUIT_PORT_BASE` | `25600 + 20 * slot`; the slot owns twenty ports from there |
| `CIRCUIT_WORK` | the slot's work directory for worlds, copied binaries and gate output |
| `XMAKE_GLOBALDIR` | one xmake global directory shared by all slots; install packages into it one at a time, before lanes build |
| `CIRCUIT_VANILLA_JAR_1_21_8`, `CIRCUIT_VANILLA_JAR_1_21_4` | taken from the environment, or found next to the main checkout |

Each worktree has its own `build/`, so configurations and binaries never collide. Use at most six compile jobs per worktree when several build at once.

Two hooks apply to every worktree once `core.hooksPath` is an absolute path: `pre-push` runs the publication guard of the pushing checkout and rejects a push of `main` from a linked worktree, and `reference-transaction` lets only the main checkout move the local `main`. Their sources are in `tools/hooks/`; install them from the main checkout:

```bash
bash tools/worktree.sh install-hooks --set-path   # --set-path points core.hooksPath at .local/public-hooks
```

## Behavior sources

Implement Minecraft behavior from specifications, observed traces, and test fixtures. Do not copy, translate, or paraphrase decompiled, deobfuscated, or mapped game source into this repository, and do not read game, Paper or Folia source in sessions that write Circuit code. Numeric parameters may come from game source only as data files with provenance prepared in the separate specification room. Reference traces come from an instrumented target-version server maintained outside this repository. Committed fixtures record their source and target version and are reviewed for third-party content. This is a project contribution rule, not legal advice.

### Target-version harness

The harness is a separate project. Running it requires accepting the Minecraft EULA, and the harness may use the target version's official mappings internally; neither the server, the mappings nor harness code enters this repository. For each skeleton, a harness must:

- start the unmodified target-version server on a fresh void world with a fixed seed, and set `max-chained-neighbor-updates` from the fixture's `neighbor-update-limit`;
- keep the area's chunks plus one chunk of margin force-loaded, as described in [the trace format](TRACE_FORMAT.md);
- apply the actions of tick `t` before the server runs tick `t`, as player inputs would be;
- record every block state change inside the area at the moment it is written, including intermediate states, and read probes after the tick ends;
- write a `harness_trace` fixture with its own build identity in the `harness` key.

Copy only the resulting trace files into `tests/fixtures/harness/` (1.21.8), or into `tests/fixtures/harness-1.21.4/` for a 1.21.4 build of the harness, under the name of the skeleton they came from. Review them before committing. Each trace names the game version of the server that recorded it. `evidence_tests` then requires every trace to pass on the oracle, under the bundle of its version; the runtime must match the oracle.

Version-2 traces (entities, stages, random draws) are recorded the same way. Record each one twice on fresh worlds and keep it only when both recordings are identical (the harness's `run_repeat.sh`). A trace Circuit does not match yet goes into `tests/fixtures/open/` or `open-1.21.4/`, with its outcome and first mismatch added to that directory's `EXPECTED.txt`, and its status into the [compatibility ledger](../compatibility/README.md). It moves to the harness directory once Circuit passes it. A skeleton whose content does not exist in a version (such as a game rule added later) is refused by that version's harness; note the refusal in that directory's `NOT_APPLICABLE.txt`.

To see every comparison for one scenario at once, run it everywhere:

```bash
tools/scenario.py tests/fixtures/skeletons/item_drop_piston.ctrace --harness ../circuit-harness --harness ../circuit-harness-1.21.4
```

The report states whether each official recording repeats, Circuit's outcome and first difference against each version, and whether the versions differ. It is written to `.local/scenarios/<name>/report.md`.

The harness can also time the target version's tick phases for a workload (`python3 tools/generate_workloads.py` writes the workloads). The runtime's own profile and benchmark driver return with the new runtime; see [the benchmark notes](../tools/bench/README.md).

Change a claimed tier only with the evidence that `contract_tests` requires: an L0 claim needs target-version traces and no known gap, and an L2 claim lists its gaps. `circuit-fixture --compatibility` prints the table.

## Client testing

The first generation's server was checked with two clients that live outside this repository, and the new server will be checked with them again once it exists. Neither is a build dependency.

**Protocol library.** Mineflayer scripts (Node.js) cover:
- creative placement and breaking, dust powering a lamp, a piston push and a piston destroying dust into an item entity, player visibility and chat (`suite.js`, two bots);
- chest placement and pairing (`chests.js`);
- container windows (`windows.js`): every click mode in a chest, the own window, a double chest and a hopper, thrown items and pickups. Each step prints the window, carried stack and inventory the server reports;
- vitals (`vitals.js`) and collision (`collision.js`): scripted moves with mineflayer's physics off, printing the corrections, health, hunger, deaths and kicks the server sends.

Calibrate each script against an offline-mode vanilla server of the same version with a fresh flat world, with the bots listed as operators. Then run it against Circuit (set `MC_VERSION=1.21.4` and `game-version=java-1.21.4` for the second version) and compare the outputs:

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

Commit ordinary source, tests, build files, license material, and public guides normally. The [roadmap](../TODO.md) gives the order of the restructuring.

The ignore file names the internal documents kept local: agent instructions, execution plans, architecture, verification log, `docs/internal/`, and the incomplete commercial agreement. Normal build output, local tool state, archives, runtime worlds, logs, and credentials are also excluded. This does not prevent publishing source or normal documentation.

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

Project documentation and source comments use English; core documents may also have a Chinese version. Public documents describe project behavior, scope, and usage; private execution notes are not required to build or run the repository.
