# Gates

A gate is a list of steps. Each step ends as `PASS`, `FAIL` or `NOT-RUN`, the gate prints one line per step, and its exit status is 0 only when every planned step is `PASS`. A step that could not start is `NOT-RUN` and fails the gate; nothing is skipped silently.

| Tier | When | Command |
| --- | --- | --- |
| T1 | every step, in the lane's worktree | `tools/gate/t1.sh [scenario...]` |
| T2 | every merge into `main` | `tools/gate/t2.sh --tree <checkout> --work <directory>` |
| T3 | nightly on `main` | T2 plus `tools/gate/scenarios.py run --all` |
| T4 | end of a stage | T3 plus that stage's exit conditions |

T3 and T4 are combinations of the commands below; they have no script of their own.

## What every tier checks

- **Official jars required.** `CIRCUIT_VANILLA_JAR_1_21_8` and `CIRCUIT_VANILLA_JAR_1_21_4` must name readable files, and the test programs run with `CIRCUIT_REQUIRE_JARS=1`, so a case that needs a jar fails instead of being skipped.
- **Zero warnings.** A line with `warning:` in the debug build log fails the gate. An incremental build shows only the warnings of the files it compiled; `--rebuild` compiles all of them again, and T2 always builds its debug tree from nothing with the compiler cache off.
- **Every test program, side by side.** At most `--test-jobs` programs run at a time (default 6), longest first. A program passes when it exits 0, its log has a completion line (`N passed, 0 failed` with N above zero, or a line with `passed` for the programs without cases), no case reports `SKIPPED`, and no sanitizer report is in the log. A sanitizer that stops before the tests run has no completion line and a non-zero exit status, so it fails.
- **Compiler crashes.** When a build stops with an internal compiler error, it is continued from where it stopped, at most twice, and the build step says how often that happened. The log keeps every attempt, so the warning count still covers every file. No other failure is retried: a test program that dies is a failed step.
- **Timing.** Every step line carries its seconds; the programs line gives the side-by-side time and the sum of the single times.

`CIRCUIT_GATE_SHARDS="evidence_tests=4 storage_tests=2"` runs a program as several shards (`CIRCUIT_TEST_SHARD=i/n`) when the program supports it. `CIRCUIT_GATE_TEST_TIMEOUT` and `CIRCUIT_GATE_BUILD_TIMEOUT` bound a program and a build in seconds (defaults 7200 and 3600).

## T1: `t1.sh`

Runs `tools/check.sh --lane` in the tree the script belongs to, then the client scenarios named on the command line (or selected with `--tag`) against a copy of the `circuit-server` it just built. The copy matters: a later build rewrites the build output, and a probe must not have its server replaced under it.

Scenario settings come from the environment. A worktree made by `tools/worktree.sh` provides the port base, the work directory and the jars in `.circuit-worktree.env`; the evidence directories are named by `CIRCUIT_SCENARIO_MANIFEST`, `CIRCUIT_PROBE_DIR` and `CIRCUIT_REFERENCE_DIR`. When scenarios are named and a setting is missing, the scenario step is `NOT-RUN` and the gate fails. The server listens on the slot's port base plus 2 and gets, per version, a configuration that names the version and its official jar unless `CIRCUIT_SCENARIO_CONFIG_1_21_8` or `CIRCUIT_SCENARIO_CONFIG_1_21_4` names another file.

## T2: `t2.sh`

Exports one commit of a checkout (default `HEAD`; `--working-files` takes the files as they are), makes one copy per tree kind under the work directory, and in each copy builds every target and runs every test program:

| Tree | Build | Extra |
| --- | --- | --- |
| `debug` | from nothing, compiler cache off, zero warnings | compatibility ledger, ledger tool tests, Java SPI compilation |
| `asan` | incremental in the kept copy | `ASAN_OPTIONS=detect_leaks=1` |
| `ubsan` | incremental in the kept copy | `UBSAN_OPTIONS=print_stacktrace=1`; the mode stops at the first report |
| `tsan` | incremental in the kept copy | `TSAN_OPTIONS=halt_on_error=1`, run under `setarch -R` |

`--trees "debug ubsan"` selects kinds, `--side-by-side N` runs N trees at a time (default 1; each tree uses `--jobs` compilers and `--test-jobs` programs). The copy is verified against the exported commit before it is built. The gate uses the scripts next to `t2.sh`, not those of the candidate. Logs are in `<work>/<kind>/logs/`.

The work directory is the only place written. Give each concurrent run its own; nothing is shared between two work directories except the xmake global directory when `XMAKE_GLOBALDIR` is set.

## Client scenarios: `scenarios.py`

A scenario is a probe script and, per game version, exactly one official reference output. The probes and references are kept outside this repository, so the tool takes their directories as arguments, and a manifest pins every reference by path and SHA-256:

```text
# <scenario> <version> <reference> <sha256> <probe> [option=value ...]
effects2  1.21.8  c61/ef6-official-1.21.8.out  437a3457...  effects2.js  tags=effects
effects2  1.21.4  c61/ef6-official-1.21.4.out  0c9d11e2...  effects2.js  tags=effects
ns-place  1.21.8  c513b/np1-official-1.21.8.out  5b0e...    ns-place.js  compare=added-as-set refpack=c513b/nscraft
old-only  1.21.4  not-applicable
```

The manifest is rejected when a scenario and version has two lines, when a scenario lacks a line for a version, or when a line is malformed. A step fails when its reference is missing or no longer has the pinned hash, when its output is missing, or when the output differs. `python3 tools/gate/scenarios.py --help` lists the options of a manifest line.

| Subcommand | Purpose |
| --- | --- |
| `check` | validate the manifest, the pins and, with `--probes`, the probe files |
| `run` | start the given server binary once per scenario and version on the given port with a fresh world, run the probe, stop the server it started, and compare the output with the pinned reference |
| `compare` | judge outputs that are already stored as `<outputs>/<scenario>-<version>.out` |
| `diff` | show the steps where two outputs differ; the exit status follows the bytes, not the parsed view |
| `pin` | print `<reference> <sha256>` for new manifest lines |

`run` never kills a process it did not start: a port that is already in use makes the step `NOT-RUN`. A probe that times out, exits with an unexpected status, or loses its server fails its step. Probes that wait on the wall clock produce false differences on a loaded machine; `--cpus` pins the server and the probe to a CPU list.

## Shared functions: `lib.sh`

`tools/check.sh --lane` and the tier scripts source `lib.sh` for the step records, the build with its warning count, the side-by-side program runner and the summary.
