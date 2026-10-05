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
- **Changes.** With `changes ordered`, the recorder lists every block state change inside the area during each tick from 1 on, in the order the changes were applied, including intermediate states. `unordered` compares each tick's changes as a multiset. A change that only affects properties the reference rules do not model, such as dust connection shape, maps to no change and is dropped before comparison.
- **Probes.** Probes read state at the end of the tick, after all phases.
- **Randomness.** Random ticks are off unless `random-tick-speed` is set. Circuit's random-tick stream is a declared L1 choice, so exact random outcomes are not expected to match the target version.

Evaluation reports one of four outcomes:

- `PASS` — every compared observation matched.
- `FAIL` — the first mismatch, with its fixture line.
- `UNSUPPORTED` — the fixture uses content or setups the reference rules cannot represent, such as locked repeaters, waterlogged chests, unknown blocks, or a setup action the rules reject.
- `INVALID` — skeletons with placeholders and unknown mechanisms.

## Content mapping for 1.21.8

The mapping translates target-version notation into the reference registry. Properties listed as ignored are accepted and discarded. None of the conventions below is verified yet; a wrong convention makes fixtures fail rather than pass.

| Block | Modeled properties | Ignored or restricted |
| --- | --- | --- |
| `air`, `cave_air`, `void_air`, `stone`, `glass`, `obsidian`, `redstone_block`, `slime_block` | — | no other properties |
| `lever` | `powered` | `face`, `facing` ignored |
| `redstone_wire` | `power` 0–15 | `north`/`south`/`east`/`west` ignored |
| `repeater` | `delay` 1–4, `facing`, `powered` | `locked` must be `false`; `facing` is taken to name the **input** side |
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

Items: `minecraft:stone`, `minecraft:redstone`, `minecraft:wheat`, `minecraft:iron_ingot`, `minecraft:lever`, `minecraft:repeater`, `minecraft:comparator`, `minecraft:wheat_seeds`. Version 1 has no entity observations; dropped and pushed items are visible only through container slots that collect them.

## Workflow

```bash
xmake run circuit-server --record-fixture "$PWD/tests/fixtures/skeletons/vertical_slice.ctrace" \
  --output "$PWD/tests/fixtures/reference/vertical_slice.ctrace"
xmake run circuit-server --check-fixture "$PWD/tests/fixtures/reference/vertical_slice.ctrace"
xmake run evidence_tests
```

`tests/fixtures/skeletons/` holds scenario skeletons, and `tests/fixtures/reference/` holds their reviewed reference recordings. `evidence_tests` re-records every skeleton and fails if the result differs from the committed recording, so changes to the rules show up as fixture diffs. `tests/fixtures/documented/` holds hand-written fixtures from public documentation.

An external harness takes the same skeletons and produces `harness_trace` fixtures. It lives outside this repository: no game code, decompiled source, or mappings enter it. Review harness output for third-party content before committing it, and record the harness build identity in the `harness` key.
