#!/usr/bin/env python3
"""Compiles the map colors of blocks from the official servers' observations.

Reads tests/fixtures/maps/colors-<release>.txt (each observed state and the
base color a held map drew for it) and PrismarineJS minecraft-data (MIT) at
the pinned commit (blocks.json and blockCollisionShapes.json of each
release). Per block it keeps the color of its states that hold no water, and
the one property whose values change it (logs by their axis, beds by their
part, wheat by its age); a release whose color differs gets its own entry.

Waterlogged states are checked against the rule the observations follow: a
state that holds water draws WATER when its top face does not bear a block
whole (its collision shape does not cover it, or it is leaves, which bear
nothing), and its own color when it does; barriers always draw WATER, and
rails, ladders, iron bars, chains and glass panes never do. The rule is
Circuit's (gameplay/map_colors.cpp applies it with the registry's sturdy top
face); the tool fails when an observation contradicts it.

Usage:
  tools/compile_map_colors.py <data>/1.21.8 <data>/1.21.4 --fixtures tests/fixtures/maps \\
      --source src/gameplay/map_colors_data.cpp
"""
import argparse
import collections
import json
import os
import sys

RELEASES = ["1.21.8", "1.21.4"]
WATER = 12
# Waterlogged states of these always draw WATER, or never (they keep their own color).
ALWAYS_WATER = {"barrier"}
NEVER_WATER = {"rail", "powered_rail", "detector_rail", "activator_rail", "ladder", "iron_bars", "chain", "glass_pane"} | {
    f"{c}_stained_glass_pane" for c in ("white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray", "light_gray",
                                        "cyan", "purple", "blue", "brown", "green", "red", "black")}
# Blocks the probes could not show: always under another block (dripleaf stems
# under their leaf, twisting vines under their tip), or water itself.
ASSUMED = {"big_dripleaf_stem": 7, "twisting_vines_plant": 23, "kelp_plant": WATER, "bubble_column": WATER,
           "air": 0, "cave_air": 0, "void_air": 0}
# Living coral wall fans (they die without water beside them) take their fans' colors.
CORALS = ("tube", "brain", "bubble", "fire", "horn")


def values_of(s):
    if s["type"] == "bool":
        return ["true", "false"]
    if s["type"] == "int" and not s.get("values"):
        return [str(i) for i in range(s["num_values"])]
    return list(s["values"])


def parse_notation(text):
    name, _, rest = text.partition("[")
    props = {}
    if rest:
        for kv in rest.rstrip("]").split(","):
            k, _, v = kv.partition("=")
            props[k] = v
    return name.removeprefix("minecraft:"), props


def top_whole(name, boxes):
    if name.endswith("_leaves"):
        return False  # leaves bear nothing (tests/fixtures/collision/support-*.txt)
    rects = [b for b in boxes if b[4] >= 1.0]
    return all(any(b[0] <= (i + 0.5) / 16 <= b[3] and b[2] <= (j + 0.5) / 16 <= b[5] for b in rects)
               for i in range(16) for j in range(16))


def compile_release(data_dir, fixture):
    blocks = {b["name"]: b for b in json.load(open(os.path.join(data_dir, "blocks.json")))}
    shapes = json.load(open(os.path.join(data_dir, "blockCollisionShapes.json")))

    def boxes(name, props):
        b = blocks[name]
        idx = 0
        for s in b.get("states", []):
            vs = values_of(s)
            idx = idx * len(vs) + vs.index(props[s["name"]])
        sh = shapes["blocks"][name]
        return shapes["shapes"][str(sh if isinstance(sh, int) else sh[idx])]

    dry, wet = collections.defaultdict(list), []
    for line in open(fixture):
        if line.startswith("#") or not line.strip():
            continue
        text, color = line.split()
        name, props = parse_notation(text)
        if name not in blocks:
            sys.exit(f"compile_map_colors: {fixture}: unknown block {name}")
        if props.get("waterlogged") == "true":
            wet.append((name, props, int(color)))
        else:
            dry[name].append((props, int(color)))
    table = {}
    for name in blocks:
        obs = dry.get(name)
        if not obs:
            if name in ASSUMED:
                table[name] = (ASSUMED[name], None, {})
            elif name.endswith("_coral_wall_fan") and name.removesuffix("_coral_wall_fan") in CORALS:
                table[name] = None  # filled from the fan below
            else:
                sys.exit(f"compile_map_colors: {fixture}: no color observed for {name}")
            continue
        base = collections.Counter(c for _, c in obs).most_common(1)[0][0]
        prop, by = None, {}
        for props, c in obs:
            if c == base:
                continue
            explained = [k for k, v in props.items() if all(cc == c for pp, cc in obs if pp.get(k) == v)]
            if not explained or (prop and prop not in explained):
                sys.exit(f"compile_map_colors: {fixture}: {name} {props} drew {c}, explained by no single property")
            prop = prop or explained[0]
            by[props[prop]] = c
        table[name] = (base, prop, by)
    for name, entry in table.items():
        if entry is None:
            table[name] = table[name.replace("_wall_fan", "_fan")]

    def color(name, props):
        base, prop, by = table[name]
        return by.get(props.get(prop), base) if prop else base

    for name, props, c in wet:
        if name in ALWAYS_WATER:
            want = WATER
        elif name in NEVER_WATER:
            want = color(name, props)
        else:
            want = color(name, props) if top_whole(name, boxes(name, props)) else WATER
        if want != c:
            sys.exit(f"compile_map_colors: {fixture}: waterlogged {name} {props} drew {c}, the rule gives {want}")
    return table, len(wet), sum(len(o) for o in dry.values())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data", nargs=2)
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--source", required=True)
    args = ap.parse_args()
    tables = {}
    for release, data_dir in zip(RELEASES, args.data):
        tables[release], wet, dry = compile_release(data_dir, os.path.join(args.fixtures, f"colors-{release}.txt"))
        print(f"{release}: {len(tables[release])} blocks from {dry} states, {wet} waterlogged states checked")
    entries = []
    names = sorted(set(tables[RELEASES[0]]) | set(tables[RELEASES[1]]))
    for name in names:
        per = {r: tables[r].get(name) for r in RELEASES}
        present = [r for r in RELEASES if per[r] is not None]
        if all(per[r] == per[present[0]] for r in present):
            entries.append((name, "", per[present[0]]))
        else:
            for r in present:
                entries.append((name, r, per[r]))
    out = [
        "// SPDX-License-Identifier: AGPL-3.0-only",
        "// Generated by tools/compile_map_colors.py from tests/fixtures/maps/colors-*.txt; do not edit.",
        '#include "map_colors_data.hpp"',
        "",
        "namespace circuit::gameplay::map_color_data {",
        "",
        "const Entry entries[] = {",
    ]
    for name, release, (base, prop, by) in entries:
        values = ",".join(f"{v}={c}" for v, c in sorted(by.items()))
        out.append(f'    {{"{name}", "{release}", {base}, {json.dumps(prop) if prop else "nullptr"}, {json.dumps(values) if values else "nullptr"}}},')
    out += ["};", f"const std::size_t entry_count = {len(entries)};", "", "const char* const always_water[] = {"]
    out += [f'    "{n}",' for n in sorted(ALWAYS_WATER)]
    out += ["};", f"const std::size_t always_water_count = {len(ALWAYS_WATER)};", "", "const char* const never_water[] = {"]
    out += [f'    "{n}",' for n in sorted(NEVER_WATER)]
    out += ["};", f"const std::size_t never_water_count = {len(NEVER_WATER)};", "", "} // namespace circuit::gameplay::map_color_data", ""]
    open(args.source, "w").write("\n".join(out))
    print(f"wrote {args.source}: {len(entries)} entries")


if __name__ == "__main__":
    main()
