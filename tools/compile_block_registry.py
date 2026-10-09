#!/usr/bin/env python3
"""Compiles the block registry: every block state of every supported version.

Reads PrismarineJS minecraft-data (MIT) at the pinned commit: blocks.json,
blockCollisionShapes.json and items.json of each version. Each block schema (a
name with its property list) is one internal block. Shared schemas appear
once; a block whose properties differ between versions has one internal block
per schema, the older one named `<name>@<release>`. Internal state ids number
the states of the internal blocks in order, starting at 1; they are stable as
long as the internal block list only grows, which `--previous` checks against
the committed table.

Each version maps its states to internal ids and back. Per internal block the
table keeps the data the rules use for blocks without rules of their own:
hardness, opacity, the effective tool and the lowest tier that harvests the
block. Per state it keeps the collision class (empty, partial, full cube) and
which faces the collision shape covers completely.

Usage:
  tools/compile_block_registry.py <data>/1.21.8 <data>/1.21.4 --commit <sha> \\
      --source src/gameplay/block_registry_data.cpp [--previous src/gameplay/block_registry_data.cpp]
"""
import argparse
import json
import os
import re
import sys

REGISTRY_ID = "circuit-blocks-1"
TOOLS = ["none", "pickaxe", "axe", "shovel", "hoe"]
TIERS = {"wooden": 1, "golden": 1, "stone": 2, "iron": 3, "diamond": 4, "netherite": 5}
# Faces in the order of contracts::Direction: down, up, north, south, west, east.
# Each is (axis of the face normal, side: 0 for the low plane, 1 for the high one).
FACES = [(1, 0), (1, 1), (2, 0), (2, 1), (0, 0), (0, 1)]


def values_of(state):
    if state["type"] == "bool":
        return ["true", "false"]
    if state["type"] == "int" and not state.get("values"):
        return [str(i) for i in range(state["num_values"])]
    return list(state["values"])


def schema(block):
    return tuple((s["name"], tuple(values_of(s))) for s in block.get("states", []))


def face_covered(shape, axis, side):
    """Whether the boxes touching one face plane cover the whole unit face."""
    u, v = [a for a in range(3) if a != axis]
    plane = float(side)
    rects = []
    for box in shape:
        lo, hi = box[axis], box[axis + 3]
        if (side == 0 and lo <= 0.0 < hi) or (side == 1 and lo < 1.0 <= hi):
            rects.append((max(box[u], 0.0), max(box[v], 0.0), min(box[u + 3], 1.0), min(box[v + 3], 1.0)))
    del plane
    if not rects:
        return False
    us = sorted({0.0, 1.0} | {c for r in rects for c in (r[0], r[2]) if 0.0 < c < 1.0})
    vs = sorted({0.0, 1.0} | {c for r in rects for c in (r[1], r[3]) if 0.0 < c < 1.0})
    for i in range(len(us) - 1):
        for j in range(len(vs) - 1):
            cu, cv = (us[i] + us[i + 1]) / 2, (vs[j] + vs[j + 1]) / 2
            if not any(r[0] <= cu <= r[2] and r[1] <= cv <= r[3] for r in rects):
                return False
    return True


def shape_byte(shape):
    """Bits 0-1: 0 no collision, 1 partial shape, 2 full cube; bits 2-7: covered faces."""
    if not shape:
        return 0
    kind = 2 if len(shape) == 1 and shape[0] == [0.0, 0.0, 0.0, 1.0, 1.0, 1.0] else 1
    faces = 0
    for i, (axis, side) in enumerate(FACES):
        if face_covered(shape, axis, side):
            faces |= 1 << i
    return kind | faces << 2


def tool_of(material):
    parts = (material or "").split(";")
    if "mineable/pickaxe" in parts or "incorrect_for_wooden_tool" in parts:
        return 1
    for i, name in enumerate(TOOLS[2:], 2):
        if f"mineable/{name}" in parts:
            return i
    return 0


def tier_of(block, tool, item_names):
    """Lowest tier of the effective tool that harvests the block; 0 when any
    tool does, 255 when only tools of another kind do."""
    harvest = block.get("harvestTools")
    if not harvest:
        return 0
    tiers = []
    for item in harvest:
        name = item_names[int(item)]
        material, _, kind = name.partition("_")
        if tool and kind == TOOLS[tool] and material in TIERS:
            tiers.append(TIERS[material])
    return min(tiers) if tiers else 255


def previous_blocks(path):
    text = open(path, encoding="utf-8").read()
    body = text.split("const BlockData blocks[] = {", 1)[1].split("};", 1)[0]
    return re.findall(r'^    \{"([^"]+)", ', body, re.M)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("versions", nargs="+", help="minecraft-data directories, newest first")
    parser.add_argument("--commit", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--previous", help="committed table; its internal blocks must keep their ids")
    args = parser.parse_args()

    versions = []
    for directory in args.versions:
        load = lambda name: json.load(open(os.path.join(directory, name), encoding="utf-8"))  # noqa: E731
        items = {i["id"]: i["name"] for i in load("items.json")}
        versions.append((os.path.basename(os.path.normpath(directory)), load("blocks.json"), load("blockCollisionShapes.json"), items))

    # Internal blocks: the newest version's schemas in its registry order, then
    # schemas only older versions have.
    internal = []  # (name, schema, block json, shapes json, item names)
    index = {}
    for release, blocks, shapes, items in versions:
        for b in blocks:
            key = (b["name"], schema(b))
            if key in index:
                continue
            name = b["name"] if not any(n == b["name"] for n, *_ in internal) else f"{b['name']}@{release}"
            index[key] = len(internal)
            internal.append((name, key[1], b, shapes, items))

    if args.previous and os.path.exists(args.previous):
        previous = previous_blocks(args.previous)
        if [n for n, *_ in internal][: len(previous)] != previous:
            sys.exit("the internal block list must only grow: an existing block moved or changed")

    out = [
        "// SPDX-License-Identifier: AGPL-3.0-only",
        "// Generated by tools/compile_block_registry.py from PrismarineJS minecraft-data (MIT),",
        f"// commit {args.commit}, blocks.json, blockCollisionShapes.json and items.json of "
        + ", ".join(r for r, *_ in versions) + ". Do not edit by hand.",
        '#include "block_registry_data.hpp"',
        "",
        "namespace circuit::gameplay::registry::data {",
        f'const char* const registry_id = "{REGISTRY_ID}";',
        "",
        "// name, first property, property count, first internal state, state count, default state offset,",
        "// hardness (negative: unbreakable), opaque, effective tool, harvest tier",
        "const BlockData blocks[] = {",
    ]
    properties = []  # (name, first value, count)
    values = []
    shape_bytes = []
    first_state = 1
    for name, sch, b, shapes, items in internal:
        count = 1
        for _, vals in sch:
            count *= len(vals)
        if count != b["maxStateId"] - b["minStateId"] + 1:
            sys.exit(f"{name}: {count} states by properties, {b['maxStateId'] - b['minStateId'] + 1} by ids")
        first_prop = len(properties)
        for pname, vals in sch:
            properties.append((pname, len(values), len(vals)))
            values.extend(vals)
        ids = shapes["blocks"].get(b["name"], 0)
        ids = ids if isinstance(ids, list) else [ids] * count
        if len(ids) != count:
            sys.exit(f"{name}: {len(ids)} shapes for {count} states")
        shape_bytes.extend(shape_byte(shapes["shapes"][str(i)]) for i in ids)
        hardness = b.get("hardness")
        hardness = -1.0 if hardness is None or hardness < 0 else float(hardness)
        opaque = 1 if b.get("filterLight", 0) == 15 and not b.get("transparent", False) else 0
        tool = tool_of(b.get("material"))
        out.append(f'    {{"{name}", {first_prop}, {len(sch)}, {first_state}, {count}, {b["defaultState"] - b["minStateId"]}, '
                   f'{hardness!r}f, {opaque}, {tool}, {tier_of(b, tool, items)}}},')
        first_state += count
    out += ["};", f"const std::uint32_t block_count = {len(internal)};", f"const std::uint32_t state_count = {first_state - 1};", ""]
    out.append("const PropertyData properties[] = {")
    for pname, first, count in properties:
        out.append(f'    {{"{pname}", {first}, {count}}},')
    out += ["};", "const char* const values[] = {"]
    for i in range(0, len(values), 12):
        out.append("    " + ", ".join(f'"{v}"' for v in values[i:i + 12]) + ",")
    out += ["};", "// Per internal state (from 1): bits 0-1 collision (0 none, 1 partial, 2 full cube),",
            "// bits 2-7 the faces the collision shape covers (down, up, north, south, west, east).",
            "const std::uint8_t shapes[] = {"]
    for i in range(0, len(shape_bytes), 32):
        out.append("    " + ", ".join(str(c) for c in shape_bytes[i:i + 32]) + ",")
    out.append("};")
    out.append("")

    # Per version: each of its blocks in its registry order, as (first wire
    # state id, internal block). Within a block, states are in the same order
    # on both sides because the schema is the same.
    tables = []
    for release, blocks, _, _ in versions:
        ident = release.replace(".", "_")
        out.append(f"const VersionBlock blocks_{ident}[] = {{")
        for b in sorted(blocks, key=lambda b: b["minStateId"]):
            out.append(f"    {{{b['minStateId']}, {index[(b['name'], schema(b))]}}},")
        out.append("};")
        tables.append((release, ident, len(blocks), max(b["maxStateId"] for b in blocks) + 1))
    out.append("const VersionTable versions[] = {")
    for release, ident, count, total in tables:
        out.append(f'    {{"{release}", blocks_{ident}, {count}, {total}}},')
    out += ["};", f"const std::size_t version_count = {len(tables)};", "", "} // namespace circuit::gameplay::registry::data", ""]
    with open(args.source, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print(f"{len(internal)} internal blocks, {first_state - 1} internal states, {len(properties)} properties")


if __name__ == "__main__":
    main()
