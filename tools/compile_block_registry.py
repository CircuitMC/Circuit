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
block. Per state it keeps the collision class (empty, partial, full cube),
which faces the collision shape covers completely, its light: emission,
dampening and, for states whose shape occludes light, the parts of each face
the occlusion shape covers (light_of below; minecraft-data gives light per
block and describes its default state, so the state rules are Circuit's), and
whether it counts for the MOTION_BLOCKING heightmap (motion_blocking below).

Usage:
  tools/compile_block_registry.py <data>/1.21.8 <data>/1.21.4 --commit <sha> \\
      --source src/gameplay/block_registry_data.cpp [--previous src/gameplay/block_registry_data.cpp]
"""
import argparse
import itertools
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


OXIDATION = {"": 15, "exposed_": 12, "weathered_": 8, "oxidized_": 4}
FACE_NAMES = ("down", "up", "north", "south", "west", "east")
# Blocks whose shape occludes light, besides slabs and stairs (minecraft.wiki
# "Light", directional opacity). Their occlusion shape is their collision
# shape, except snow, whose layers occlude by their height.
SHAPE_BLOCKS = {"farmland", "dirt_path", "daylight_detector", "lectern", "enchanting_table", "end_portal_frame", "stonecutter",
                "piston", "sticky_piston", "piston_head", "sculk_sensor", "calibrated_sculk_sensor", "sculk_shrieker", "snow"}


def light_of(name, p, block, boxes16, default):
    """(emission, dampening 0/1/15, occlusion boxes in sixteenths or None) of one state."""
    e = block.get("emitLight", 0)
    d = block.get("filterLight", 0)
    lit = p.get("lit") == "true"
    faces = any(p.get(f) == "true" for f in FACE_NAMES)
    if name in ("furnace", "smoker", "blast_furnace"):
        e = 13 if lit else 0
    elif name == "redstone_lamp":
        e = 15 if lit else 0
    elif name in ("redstone_ore", "deepslate_redstone_ore"):
        e = 9 if lit else 0
    elif name in ("redstone_torch", "redstone_wall_torch"):
        e = 7 if lit else 0
    elif name in ("campfire", "soul_campfire"):
        e = (15 if name == "campfire" else 10) if lit else 0
    elif name.endswith("candle"):
        e = 3 * int(p["candles"]) if lit else 0
    elif name.endswith("candle_cake"):
        e = 3 if lit else 0
    elif name.endswith("copper_bulb"):
        e = OXIDATION[name.removeprefix("waxed_").removesuffix("copper_bulb")] if lit else 0
    elif name == "light":
        e = int(p["level"])
    elif name == "sea_pickle":
        e = 3 + 3 * int(p["pickles"]) if p["waterlogged"] == "true" else 0
    elif name == "respawn_anchor":
        e = 15 * int(p["charges"]) // 4
    elif name in ("cave_vines", "cave_vines_plant"):
        e = 14 if p["berries"] == "true" else 0
    elif name == "glow_lichen":
        e = 7 if faces else 0
    elif name == "vault":
        e = 6 if p["vault_state"] == "inactive" else 12
    elif name == "trial_spawner":
        e = {"waiting_for_players": 4, "active": 8, "waiting_for_reward_ejection": 8, "ejecting_reward": 8}.get(p["trial_spawner_state"], 0)
    # minecraft-data's filterLight describes the default state: a block that
    # is waterlogged by default filters light only through its water.
    if default.get("waterlogged") == "true" and d == 1 and p.get("waterlogged") == "false":
        d = 0
    if name in ("sculk_vein", "resin_clump") and p.get("waterlogged") == "false" and faces:
        d = 0
    occlusion = None
    if (name.endswith("_slab") and p.get("type") != "double") or name.endswith("_stairs") or name in SHAPE_BLOCKS:
        if not (name in ("piston", "sticky_piston") and p["extended"] == "false"):
            occlusion = [(0, 0, 0, 16, 2 * int(p["layers"]), 16)] if name == "snow" else boxes16
            if name in ("piston", "sticky_piston"):
                d = 0
    if name.endswith("_slab") and p.get("type") == "double":
        d = 15
    if name == "snow" and p["layers"] == "8":
        d = 15
    if p.get("waterlogged") == "true" and d == 0 and not name.endswith("copper_grate"):
        d = 1
    return e, d, occlusion


# The MOTION_BLOCKING heightmap counts states that are solid or hold a fluid
# (minecraft.wiki "Heightmap": the legacy solid property, not the collision
# box). Circuit's rules, checked against a report of every state of both
# official servers: a state is solid when its collision box's three sides add
# up to at least 35/16 of a block or it is a full block tall, except for
# families that are solid in every state or never solid.
ALWAYS_SOLID = re.compile(r"(.*_(sign|banner|pressure_plate|fence_gate)|(?!resin_brick_wall).*_wall|dead_.*_coral(_wall)?(_fan)?|moving_piston|"
                          r"sculk_vein|amethyst_cluster|.*_amethyst_bud|bell|cake|chain|conduit|dried_ghast|(soul_)?lantern|lightning_rod|"
                          r"piston_head|pointed_dripstone|turtle_egg)$")
NEVER_SOLID = re.compile(r"(.*_carpet|.*candle|.*_(head|skull)|flower_pot|potted_.*|ladder|lily_pad|(flowering_)?azalea|big_dripleaf|"
                         r"chorus_(flower|plant)|cocoa|end_rod|heavy_core|scaffolding|snow|sea_pickle|pitcher_crop)$")
FLUID_BLOCKS = {"water", "lava", "bubble_column", "kelp", "kelp_plant", "seagrass", "tall_seagrass"}


def motion_blocking(name, p, boxes16):
    if name in FLUID_BLOCKS or p.get("waterlogged") == "true":
        return True
    if ALWAYS_SOLID.match(name):
        return True
    if NEVER_SOLID.match(name) or not boxes16:
        return False
    lo = [min(b[k] for b in boxes16) for k in range(3)]
    hi = [max(b[k + 3] for b in boxes16) for k in range(3)]
    return sum(h - l for h, l in zip(hi, lo)) >= 35 or hi[1] - lo[1] >= 16


def face_masks(boxes16):
    """Per face (FACES order), the 16x16 cells the boxes touching that face cover, as 256-bit integers."""
    masks = []
    for axis, side in FACES:
        u, v = [a for a in range(3) if a != axis]
        mask = 0
        for box in boxes16:
            if (side == 0 and box[axis] == 0) or (side == 1 and box[axis + 3] == 16):
                for a in range(box[u], box[u + 3]):
                    for b in range(box[v], box[v + 3]):
                        mask |= 1 << (a * 16 + b)
        masks.append(mask)
    return tuple(masks)


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
    light_bytes = []
    occlusion_ids = []
    occlusion_shapes = {}  # face masks -> id (from 1)
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
        combos = list(itertools.product(*[vals for _, vals in sch])) or [()]
        names = [pname for pname, _ in sch]
        default = dict(zip(names, combos[b["defaultState"] - b["minStateId"]]))
        for combo, i in zip(combos, ids):
            boxes16 = [tuple(round(c * 16) for c in box) for box in shapes["shapes"][str(i)]]
            e, d, occlusion = light_of(b["name"], dict(zip(names, combo)), b, boxes16, default)
            motion = 1 if motion_blocking(b["name"], dict(zip(names, combo)), boxes16) else 0
            light_bytes.append(e | {0: 0, 1: 1, 15: 2}[d] << 4 | motion << 6)
            if occlusion is None:
                occlusion_ids.append(0)
            else:
                occlusion_ids.append(occlusion_shapes.setdefault(face_masks(occlusion), len(occlusion_shapes) + 1))
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
    out += ["// Per internal state (from 1): bits 0-3 light emission, bits 4-5 dampening (0: 0, 1: 1, 2: 15),",
            "// bit 6 counts for the MOTION_BLOCKING heightmap.",
            "const std::uint8_t light[] = {"]
    for i in range(0, len(light_bytes), 32):
        out.append("    " + ", ".join(str(c) for c in light_bytes[i:i + 32]) + ",")
    out += ["};", "// Per internal state (from 1): its light occlusion shape (light_faces), 0 when its shape does not occlude light.",
            "const std::uint16_t light_shape[] = {"]
    for i in range(0, len(occlusion_ids), 32):
        out.append("    " + ", ".join(str(c) for c in occlusion_ids[i:i + 32]) + ",")
    out += ["};", "// Light occlusion shapes from 1: per face (down, up, north, south, west, east), the 16x16 cells the shape",
            "// covers, as four words (bit u * 16 + v of the face's two other axes in x, y, z order).",
            "const LightFaces light_faces[] = {", "    {},"]
    for masks, _ in sorted(occlusion_shapes.items(), key=lambda kv: kv[1]):
        words = []
        for m in masks:
            words.append("{" + ", ".join(f"0x{(m >> (64 * k)) & (2**64 - 1):x}ull" for k in range(4)) + "}")
        out.append("    {{" + ", ".join(words) + "}},")
    out += ["};", f"const std::uint32_t light_shape_count = {len(occlusion_shapes)};", ""]

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
