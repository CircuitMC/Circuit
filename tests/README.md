# Tests

Each directory below holds one test program; `xmake.lua` builds every `.cpp`
file of the directory into it.

| Program | Directory |
| --- | --- |
| `contract_tests` | `tests/contracts/` |
| `rules_tests` | `tests/rules/` |
| `simulation_tests` | `tests/sim/` |
| `control_tests` | `tests/control/` |
| `evidence_tests` | `tests/evidence/` |
| `protocol_tests` | `tests/protocol/` |
| `storage_tests` | `tests/storage/` |
| `data_tests` | `tests/data/` |
| `anvil_tests` | `tests/anvil/` |
| `server_tests` | `tests/server/` |
| `extension_tests` | `tests/extensions/` (all but `sdk_probe.cpp`, the probe libraries' source) |

`runtime_tests`, `interface_tests`, `validation_tests`, `plugin_tests` and
`parallel_models` are single programs without named cases.

## Adding cases

Add a new `.cpp` file to the program's directory and define cases with
`CIRCUIT_TEST` from `tests/support/check.hpp`:

```cpp
// SPDX-License-Identifier: AGPL-3.0-only
#include "../support/check.hpp"

using circuit::test::require;

CIRCUIT_TEST("levers power the block they are on") {
    require(1 + 1 == 2, "arithmetic");
}
```

No other file changes: the build picks the file up, and the program's runner
finds the case. Case names are unique within a program; a duplicate makes the
program exit with status 2. `CIRCUIT_SOURCE_DIR` is the repository root, for
reading `tests/fixtures`.

The file that holds a program's `main()` (`<name>_tests.cpp`) registers its
cases with `suite.run(name, body)` and runs every case of the program in
`suite.finish()`. Bodies given to `suite.run` execute in `finish()`, not where
they are written. These files change only together with the interfaces they
share with other work; new cases go into new files.

## Running

A program runs its cases sorted by name and ends with one summary line:

```
server_tests: 99 passed, 0 failed, 0 skipped for a missing jar
```

The exit status is 0 when every case passed, 1 when a case failed, and 2 when
the selection or the set of names is invalid.

| Variable | Effect |
| --- | --- |
| `CIRCUIT_TEST_FILTER=text` | Run only the cases whose names contain the text |
| `CIRCUIT_TEST_SHARD=i/n` | Of the selected cases, sorted by name, run those whose index modulo `n` is `i` (`0 <= i < n`); `n` processes with `i` from 0 to `n - 1` run every case exactly once |
| `CIRCUIT_TEST_LIST=1` | Print the selected case names, one per line, and run nothing |
| `CIRCUIT_TEST_TIMES=1` | Print the duration of each passed case |
| `CIRCUIT_REQUIRE_JARS=1` | Fail every case that needs an official server jar that was not given |

Shard processes of one program can run side by side: the programs use
ephemeral ports and their own temporary directories.

## Official server jars

Some cases of `server_tests` and `data_tests` read the data packs of the
official server jars named by `CIRCUIT_VANILLA_JAR_1_21_8` and
`CIRCUIT_VANILLA_JAR_1_21_4`. They get the paths from
`circuit::test::official_jar()`. Without a jar such a case leaves out the
checks that need it, still passes, and is counted in the summary line as
skipped for a missing jar. With `CIRCUIT_REQUIRE_JARS=1` it fails instead, so a
gate cannot pass without having run those checks.

## Long cases

Four walks over large inputs are several cases each, so that shards divide
them:

- `evidence_tests`: the target-version traces (per version) and the
  repository's fixtures and skeletons, each in four parts;
- `server_tests`: the official traces with the official data packs, per
  version in four parts;
- `storage_tests`: the crash at every file operation, one case per history and
  storage policy.

A part is every fourth file of the sorted directory listing
(`tests/support/fixture_parts.hpp`), so each file is checked in exactly one
part.
