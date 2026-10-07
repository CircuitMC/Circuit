# circuit-trace fixture format (v1)

## Purpose

A circuit-trace fixture describes one small scenario and what was observed when it ran. The same file format serves three roles:

| Role | `source` | Produced by |
| --- | --- | --- |
| Skeleton | any | A person writing a scenario: area, actions, and `?` placeholders for requested observations |
| Reference recording | `circuit_reference` | `circuit-server --record-fixture`, which runs Circuit's reference rules |
| Target-version trace | `harness_trace` | An external instrumented target-version server (not part of this repository) |

Hand-written fixtures can also use `documented_behavior`, for expectations taken from public documentation, or `controlled_experiment`. Only passing `harness_trace` fixtures can support raising a mechanism's claimed compatibility tier. Even then, the change to the compatibility table is reviewed by hand. Reference recordings document Circuit's own behavior and are regression tests, not compatibility evidence.

## Syntax

A fixture is UTF-8 text with one statement per line. Blank lines and lines starting with `#` are ignored; comments cannot follow a statement on the same line. The first line is `circuit-trace 1`. Header keys come next, each at most once, and must precede all body lines.

| Header key | Value |
| --- | --- |
| `title` | 1–200 characters (required) |
| `game-version` | Target game version, for example `1.21.8` (required) |
| `source` | `harness_trace`, `documented_behavior`, `controlled_experiment` or `circuit_reference` (required) |
| `harness` | Build identity of the recorder, or `-` (required) |
| `mechanisms` | One or more mechanism ids from the compatibility table (required) |
| `area` | `x1 y1 z1 x2 y2 z2`, inclusive, at most 128 blocks per horizontal axis (required) |
| `ticks` | Scenario ticks after the setup tick, 1–10000 (required) |
| `random-tick-speed` | Random tick speed game rule, default 0 |
| `neighbor-update-limit` | Chained neighbor update limit, 1–1,000,000; default: the bundle's limit (1,000,000). A harness sets the server's `max-chained-neighbor-updates` property |
| `changes` | `ordered`, `unordered` or `none` (default) |

| Body line | Meaning |
| --- | --- |
| `at <t> set <x> <y> <z> <state>` | Replace the block at the start of tick `t` |
| `at <t> use <x> <y> <z>` | Use the block at the start of tick `t`: toggle a lever, cycle a repeater's delay, or toggle a comparator's mode |
| `at <t> insert <x> <y> <z> <slot> <item> <count>` | Add items to a container slot at the start of tick `t` |
| `change <t> <x> <y> <z> <old> <new>` | A block changed during tick `t ≥ 1` |
| `block <t> <x> <y> <z> <state or ?>` | Block state at the end of tick `t` |
| `slot <t> <x> <y> <z> <slot> <item or empty> <count>` | Container slot at the end of tick `t`; `slot <t> <x> <y> <z> <slot> ?` requests a recording |

Every position must lie inside the area. Actions run in file order within a tick, and change lines are listed in tick order. Tick 0 is the setup tick: its actions build the scenario, and changes during tick 0 are not observed. Block states use `namespace:id[key=value,...]`; items use `namespace:id`. A file is limited to 16 MiB, 200,000 lines and 4096 characters per line.

## Semantics

- **Loading.** The chunks covering the area plus one chunk of margin are loaded and ticking for the whole run; the next ring is border-loaded. A harness must keep the same chunks force-loaded.
- **Actions.** All actions for a tick are applied before that tick's scheduled ticks, with normal neighbor updates, as inputs at the start of the tick would be.
- **Changes.** With `changes ordered`, the recorder lists every block state change inside the area during each tick from 1 on, in the order the changes were applied, including intermediate states. `unordered` compares each tick's changes as a multiset. A change that only affects properties the reference rules do not model maps to no change and is dropped before comparison; since rules v6 every property of the modeled blocks except `piston_head` `short` is modeled, so dust shape changes are compared too.
- **Probes.** Probes read state at the end of the tick, after all phases.
- **Randomness.** Random ticks are off unless `random-tick-speed` is set. Circuit's random-tick stream is a declared L1 choice, so exact random outcomes are not expected to match the target version.

Evaluation reports one of four outcomes:

- `PASS` — every compared observation matched.
- `FAIL` — the first mismatch, with its fixture line.
- `UNSUPPORTED` — the fixture uses content or setups the reference rules cannot represent, such as waterlogged chests, unknown blocks, or a setup action the rules reject.
- `INVALID` — skeletons with placeholders and unknown mechanisms.

## Content mapping for 1.21.8

The mapping translates target-version notation into the reference registry. Properties listed as ignored are accepted and discarded. Target-version traces confirm the conventions below:

- repeater and comparator `facing` names the input side (vertical_slice, diode_priority, comparator_signal);
- observer `facing` names the observed side (observer_pulse);
- chest `left` and `right` pairing (double_chest, double_chest_left_input);
- water levels (water_channel).

A wrong convention makes fixtures fail rather than pass.

| Block | Modeled properties | Ignored or restricted |
| --- | --- | --- |
| `air`, `cave_air`, `void_air`, `stone`, `glass`, `obsidian`, `redstone_block`, `slime_block` | — | no other properties |
| `lever` | `face`, `facing`, `powered` | `face` and `facing` locate the block the lever hangs on |
| `redstone_wire` | `power` 0–15, `north`/`south`/`east`/`west` (`none`, `side`, `up`) | the stored shape; absent sides are `none`, so dust given without sides is a dot |
| `repeater` | `delay` 1–4, `facing`, `locked`, `powered` | `facing` names the **input** side |
| `redstone_lamp` | `lit` | — |
| `observer` | `facing` (observed side), `powered` | — |
| `piston`, `sticky_piston` | `facing`, `extended` | — |
| `piston_head` | `facing`, `type` | `short` ignored |
| `moving_piston` | `facing`, `type` | the moving block is block-entity state and is not part of the notation |
| `hopper` | `facing` (not `up`), `enabled` | — |
| `chest` | `facing`, `type` (`single`, `left`, `right`) | `waterlogged` must be `false`; a `left` half is taken to pair clockwise of its facing, `right` counter-clockwise |
| `comparator` | `facing`, `mode`, `powered` | `facing` is taken to name the **input** side; the output signal is block-entity state and is not part of the notation |
| `water` | `level`: 0 source, 1–7 flowing distance, 8–15 falling | — |
| `wheat` | `age` 0–7 | — |

Items: any item of the rule set's registry (since rules v7, every item of both versions), named `namespace:id`, without data components. Version 1 has no entity observations; dropped and pushed items are visible only through container slots that collect them.

## Workflow

```bash
xmake run circuit-server --record-fixture "$PWD/tests/fixtures/skeletons/vertical_slice.ctrace" \
  --output "$PWD/tests/fixtures/reference/vertical_slice.ctrace"
xmake run circuit-server --check-fixture "$PWD/tests/fixtures/reference/vertical_slice.ctrace"
xmake run evidence_tests
```

| Directory | Contents |
| --- | --- |
| `tests/fixtures/skeletons/` | Scenario skeletons |
| `tests/fixtures/reference/` | Reviewed reference recordings of the skeletons |
| `tests/fixtures/harness/` | Target-version traces recorded from the skeletons (or documented fixtures) of the same name |
| `tests/fixtures/documented/` | Hand-written fixtures from public documentation |
| `tests/fixtures/workloads/` | Profiling workloads generated by `tools/generate_workloads.py`: setup actions only |

`evidence_tests` re-records every skeleton and fails if the result differs from the committed recording, so changes to the rules show up as fixture diffs. It also checks that every target-version trace was recorded from the scenario currently in the repository, and that it passes on the engine and on the oracle.

An external harness takes the same skeletons and produces `harness_trace` fixtures. The harness is an instrumented target-version server and lives outside this repository. No game code, decompiled source, or mappings enter the repository; only trace files are copied in. Review harness output for third-party content before committing it, and record the harness build identity in the `harness` key. The [development guide](DEVELOPMENT.md) describes the workflow.
