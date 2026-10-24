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
block, every block's blast resistance, how pistons treat it (push_class), the note block instrument it gives (instrument) and
whether it is a redstone conductor (conductor), and what fences, panes and walls
make of it (connector). Per
state it keeps the collision class (empty, partial, full cube),
which faces the collision shape covers completely, the faces fences, panes and
walls join (join_faces), what it supports (support), its light: emission,
dampening and, for states whose shape occludes light, the parts of each face
the occlusion shape covers (light_of below; minecraft-data gives light per
block and describes its default state, so the state rules are Circuit's), and
whether it counts for the MOTION_BLOCKING heightmap (motion_blocking below).
Its collision shape (the boxes of blockCollisionShapes.json) is kept too, for
entity movement; a release whose shape of a state differs gets an override.

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


# How pistons treat a block (push_class): 0 move (pushed, and pulled by sticky
# pistons), 1 destroy (broken with its drops), 2 push only (pushed, never
# pulled), 3 immovable. Unbreakable blocks are immovable; the lists name the
# rest. Iterated against a report of every block of both official servers
# (tests/fixtures/collision/push-*.txt) until no block differed, and checked
# by pushing and pulling every block on both (tests/fixtures/harness/push_grid).
MOVE, DESTROY, PUSH_ONLY, IMMOVABLE = 0, 1, 2, 3
IMMOVABLE_NAMES = {
    "obsidian", "crying_obsidian", "respawn_anchor", "reinforced_deepslate", "anvil", "chipped_anvil", "damaged_anvil",
    "grindstone", "lodestone", "piston_head", "moving_piston",
    # Blocks with block entities that pistons leave in place.
    "dispenser", "dropper", "chest", "trapped_chest", "ender_chest", "furnace", "blast_furnace", "smoker", "barrel", "hopper",
    "chiseled_bookshelf", "spawner", "trial_spawner", "vault", "creaking_heart", "jukebox", "enchanting_table", "brewing_stand",
    "beacon", "conduit", "daylight_detector", "lectern", "campfire", "soul_campfire", "bee_nest", "beehive", "sculk_sensor",
    "calibrated_sculk_sensor", "sculk_catalyst", "sculk_shrieker", "crafter", "end_portal", "end_gateway", "nether_portal",
}
DESTROY_NAMES = {
    "water", "lava", "suspicious_sand", "suspicious_gravel", "cobweb", "short_grass", "fern", "dead_bush", "bush", "short_dry_grass",
    "tall_dry_grass", "seagrass", "tall_seagrass", "dandelion", "torchflower", "poppy", "blue_orchid", "allium", "azure_bluet",
    "oxeye_daisy", "cornflower", "wither_rose", "lily_of_the_valley", "brown_mushroom", "red_mushroom", "torch", "wall_torch",
    "fire", "soul_fire", "redstone_wire", "wheat", "ladder", "lever", "redstone_torch", "redstone_wall_torch", "snow", "cactus",
    "cactus_flower", "sugar_cane", "soul_torch", "soul_wall_torch", "carved_pumpkin", "jack_o_lantern", "cake", "repeater",
    "pumpkin", "melon", "attached_pumpkin_stem", "attached_melon_stem", "pumpkin_stem", "melon_stem", "vine", "glow_lichen",
    "resin_clump", "lily_pad", "nether_wart", "dragon_egg", "cocoa", "tripwire_hook", "tripwire", "flower_pot", "carrots",
    "potatoes", "comparator", "sunflower", "lilac", "rose_bush", "peony", "tall_grass", "large_fern", "chorus_plant",
    "chorus_flower", "torchflower_crop", "pitcher_crop", "pitcher_plant", "beetroots", "structure_void", "kelp", "kelp_plant",
    "turtle_egg", "sea_pickle", "bamboo_sapling", "bamboo", "bubble_column", "scaffolding", "bell", "lantern", "soul_lantern",
    "sweet_berry_bush", "warped_fungus", "warped_roots", "nether_sprouts", "crimson_fungus", "weeping_vines",
    "weeping_vines_plant", "twisting_vines", "twisting_vines_plant", "crimson_roots", "budding_amethyst", "amethyst_cluster",
    "large_amethyst_bud", "medium_amethyst_bud", "small_amethyst_bud", "sculk_vein", "pointed_dripstone", "cave_vines",
    "cave_vines_plant", "spore_blossom", "azalea", "flowering_azalea", "moss_carpet", "pink_petals", "wildflowers", "leaf_litter",
    "moss_block", "big_dripleaf", "big_dripleaf_stem", "small_dripleaf", "hanging_roots", "frogspawn", "decorated_pot",
    "pale_moss_block", "pale_moss_carpet", "pale_hanging_moss", "open_eyeblossom", "closed_eyeblossom", "firefly_bush",
    "mangrove_propagule", "candle", "shulker_box",
}
DESTROY_PATTERNS = [r"_sapling$", r"_leaves$", r"_bed$", r"_tulip$", r"_door$", r"_pressure_plate$", r"_button$", r"_skull$",
                    r"_head$", r"_shulker_box$", r"^(tube|brain|bubble|fire|horn)_coral(_fan|_wall_fan)?$", r"^potted_",
                    r"_candle$", r"candle_cake$"]
IMMOVABLE_PATTERNS = [r"_sign$", r"_banner$", r"command_block$"]

def push_class(name, hardness):
    if name in ("air", "cave_air", "void_air") or name.endswith("_glazed_terracotta"):
        return PUSH_ONLY
    if hardness is None or hardness < 0 or name in IMMOVABLE_NAMES or any(re.search(p, name) for p in IMMOVABLE_PATTERNS):
        return IMMOVABLE
    if name in DESTROY_NAMES or any(re.search(p, name) for p in DESTROY_PATTERNS):
        return DESTROY
    return MOVE


# The note block instrument of a block (instrument): what a note block above it
# plays, unless it is a mob head's, which a note block below it plays. In the
# order of the note block's `instrument` values. Wood (the axe's blocks) plays
# the bass and stone (the pickaxe's) the bass drum, but for the lists; the
# rest the harp. Iterated against a report of every block of both official
# servers (tests/fixtures/collision/instrument-*.txt) until no block differed.
INSTRUMENTS = ["harp", "basedrum", "snare", "hat", "bass", "flute", "bell", "guitar", "chime", "xylophone", "iron_xylophone",
               "cow_bell", "didgeridoo", "bit", "banjo", "pling", "zombie", "skeleton", "creeper", "dragon", "wither_skeleton",
               "piglin", "custom_head"]
INSTRUMENT_NAMES = {
    "gold_block": "bell", "iron_block": "iron_xylophone", "clay": "flute", "soul_sand": "cow_bell", "glowstone": "pling",
    "pumpkin": "didgeridoo", "emerald_block": "bit", "hay_block": "banjo", "packed_ice": "chime", "bone_block": "xylophone",
    "skeleton_skull": "skeleton", "wither_skeleton_skull": "wither_skeleton", "zombie_head": "zombie", "player_head": "custom_head",
    "creeper_head": "creeper", "dragon_head": "dragon", "piglin_head": "piglin", "beacon": "hat", "sea_lantern": "hat",
    "conduit": "hat", "heavy_core": "snare",
}
HARP_PATTERNS = [r"_button$", r"rail$", r"^infested_", r"shulker_box$", r"cauldron$", r"anvil$", r"amethyst", r"copper_door$",
                 r"lantern$", r"(^|_)ice$"]
HARP_NAMES = {"ladder", "bamboo", "scaffolding", "piston", "sticky_piston", "piston_head", "iron_door", "iron_trapdoor", "packed_mud",
              "iron_bars", "chain", "brewing_stand", "light_weighted_pressure_plate", "heavy_weighted_pressure_plate",
              "redstone_block", "hopper", "grindstone", "bell", "lodestone"}
BASEDRUM_NAMES = {"bedrock", "obsidian", "crying_obsidian", "respawn_anchor", "resin_block", "end_portal_frame",
                  "reinforced_deepslate", "trial_spawner", "vault", "creaking_heart"}

def instrument(name, material):
    if name in INSTRUMENT_NAMES:
        return INSTRUMENTS.index(INSTRUMENT_NAMES[name])
    if re.search(r"(^|_)glass(_pane)?$", name):
        return INSTRUMENTS.index("hat")
    if re.fullmatch(r"(red_)?sand|gravel|suspicious_(sand|gravel)|.*_concrete_powder", name):
        return INSTRUMENTS.index("snare")
    if name.endswith("_wool"):
        return INSTRUMENTS.index("guitar")
    if name in HARP_NAMES or any(re.search(p, name) for p in HARP_PATTERNS):
        return INSTRUMENTS.index("harp")
    if name in BASEDRUM_NAMES or re.search(r"_ore$|^raw_.*_block$", name):
        return INSTRUMENTS.index("basedrum")
    tools = (material or "").split(";")
    if "mineable/axe" in tools and "plant" not in tools and "gourd" not in tools:
        return INSTRUMENTS.index("bass")
    if "mineable/pickaxe" in tools:
        return INSTRUMENTS.index("basedrum")
    return INSTRUMENTS.index("harp")


# Redstone conductors (both official servers report them for every state; see
# tests/fixtures/collision/conductor-*.txt): a state conducts when its
# collision shape is a full cube, except the blocks below that never do, and
# soul sand and mud conduct anyway. 0 never, 1 always, 2 in full-cube states.
NEVER_CONDUCTS = re.compile(r"^(.*_leaves|glass|.*_stained_glass|tinted_glass|ice|frosted_ice|beacon|chorus_flower|"
                            r"(waxed_)?((exposed|weathered|oxidized)_)?copper_(grate|bulb)|glowstone|sea_lantern|observer|"
                            r"piston|sticky_piston|redstone_block|tnt)$")
ALWAYS_CONDUCTS = {"soul_sand", "mud"}

def conductor(name):
    if name in ALWAYS_CONDUCTS:
        return 1
    return 0 if NEVER_CONDUCTS.match(name) else 2


# Fences, panes and walls (both official servers report how each joins every
# state beside and above it; see tests/fixtures/collision/connection-*.tsv).
# Connector kinds: 1 wooden fence, 2 nether brick fence, 3 pane or bars, 4
# wall; bit 3 marks the blocks over which a wall always keeps its post.
WALL_POST_OVERRIDE = re.compile(r"^(.*_banner|(?!.*hanging).*_sign|.*_pressure_plate|torch|soul_torch|redstone_torch|tripwire|"
                                r"cactus_flower)$")
# They join a state through a face that its support shape covers whole: its
# collision shape's, except that soul sand, mud and eight snow layers count
# as full cubes. They never join the blocks below.
NEVER_JOINED = re.compile(r"^(.*_leaves|barrier|(.*_)?shulker_box|carved_pumpkin|jack_o_lantern|melon|pumpkin|chorus_flower)$")
FULL_SUPPORT = {"soul_sand", "mud"}


def connector(name, sch):
    kind = 0
    props = dict(sch)
    if name.endswith("_fence"):
        kind = 2 if name == "nether_brick_fence" else 1
    elif name == "iron_bars" or name.endswith("glass_pane"):
        kind = 3
    elif name.endswith("_wall") and props.get("east") == ("none", "low", "tall"):
        kind = 4
    return kind | (8 if WALL_POST_OVERRIDE.match(name) else 0)


def join_faces(name, props, shape):
    """Bits 0-5 (down, up, north, south, west, east): the faces fences, panes and walls join."""
    if NEVER_JOINED.match(name):
        return 0
    if name in FULL_SUPPORT or (name == "snow" and props.get("layers") == "8"):
        return 63
    return shape_byte(shape) >> 2


# What blocks support (both official servers report it for every state; see
# tests/fixtures/collision/support-*.txt): their support shape is their
# collision shape, except that leaves support nothing, chorus flowers only
# what hangs under them, soul sand and mud are full cubes, snow is as tall as
# its layers, hanging signs support what hangs under them (and, turned
# between the four sides, what stands on them), and fence gates nothing that
# hangs. Bit 0: the top's middle (7 to 9 sixteenths both ways) bears
# torches and lanterns; bit 1: the bottom's middle holds hanging lanterns;
# bits 2-7: whole faces (down, up, north, south, west, east) for wall torches
# and ladders. Flowers and saplings grow on SOIL.
SOIL = {"dirt", "coarse_dirt", "grass_block", "podzol", "mycelium", "rooted_dirt", "moss_block", "pale_moss_block", "mud",
        "muddy_mangrove_roots", "farmland"}


def middle_covered(shape, top):
    """Whether the boxes touching the top (or bottom) plane cover 7 to 9 sixteenths both ways."""
    rects = [(b[0], b[2], b[3], b[5]) for b in shape if (b[1] < 1.0 <= b[4] if top else b[1] <= 0.0 < b[4])]
    for x in (7, 8):
        for z in (7, 8):
            cx, cz = (x + 0.5) / 16, (z + 0.5) / 16
            if not any(r[0] <= cx <= r[2] and r[1] <= cz <= r[3] for r in rects):
                return False
    return True


def support_byte(name, props, shape):
    if name.endswith("_leaves"):
        return 0
    if name in FULL_SUPPORT:
        return 255
    if name == "snow":
        shape = [[0.0, 0.0, 0.0, 1.0, int(props["layers"]) / 8, 1.0]]
    top, bottom, faces = middle_covered(shape, True), middle_covered(shape, False), shape_byte(shape) >> 2
    if name == "chorus_flower":
        top, faces = False, faces & 1
    if name.endswith("_wall_hanging_sign"):
        bottom = True
    elif name.endswith("_hanging_sign"):
        top, bottom = int(props["rotation"]) % 4 != 0, True
    if name.endswith("_fence_gate"):
        bottom = False
    return int(top) | int(bottom) << 1 | faces << 2


# Block entity types (both official servers report every block that has one
# and the data it saves; see tests/fixtures/collision/block-entities-*.txt).
BLOCK_ENTITY_TYPES = [
    (r"(suspicious_sand|suspicious_gravel)", "brushable_block"), (r"dispenser", "dispenser"), (r"dropper", "dropper"),
    (r".*_bed", "bed"), (r"chiseled_bookshelf", "chiseled_bookshelf"), (r"spawner", "mob_spawner"),
    (r"creaking_heart", "creaking_heart"), (r"chest", "chest"), (r"trapped_chest", "trapped_chest"), (r"ender_chest", "ender_chest"),
    (r"furnace", "furnace"), (r"smoker", "smoker"), (r"blast_furnace", "blast_furnace"), (r".*_hanging_sign", "hanging_sign"),
    (r".*_sign", "sign"), (r"jukebox", "jukebox"), (r"enchanting_table", "enchanting_table"), (r"brewing_stand", "brewing_stand"),
    (r"end_portal", "end_portal"), (r"end_gateway", "end_gateway"), (r"(repeating_|chain_)?command_block", "command_block"),
    (r"beacon", "beacon"), (r"(?!piston).*_(head|skull)", "skull"), (r"comparator", "comparator"),
    (r"daylight_detector", "daylight_detector"), (r"hopper", "hopper"), (r".*_banner", "banner"), (r"(.*_)?shulker_box", "shulker_box"),
    (r"conduit", "conduit"), (r"barrel", "barrel"), (r"lectern", "lectern"), (r"bell", "bell"), (r"(soul_)?campfire", "campfire"),
    (r"structure_block", "structure_block"), (r"jigsaw", "jigsaw"), (r"test_block", "test_block"),
    (r"test_instance_block", "test_instance_block"), (r"(bee_nest|beehive)", "beehive"), (r"sculk_sensor", "sculk_sensor"),
    (r"calibrated_sculk_sensor", "calibrated_sculk_sensor"), (r"sculk_catalyst", "sculk_catalyst"),
    (r"sculk_shrieker", "sculk_shrieker"), (r"decorated_pot", "decorated_pot"), (r"crafter", "crafter"),
    (r"trial_spawner", "trial_spawner"), (r"vault", "vault"),
]


def block_entity_type(name):
    for pattern, kind in BLOCK_ENTITY_TYPES:
        if re.fullmatch(pattern, name):
            return kind
    return None


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
        "// hardness (negative: unbreakable), opaque, effective tool, harvest tier, blast resistance,",
        "// push class (0 move, 1 destroy, 2 push only, 3 immovable), note block instrument, conductor,",
        "// connector (bits 0-2: 1 wooden fence, 2 nether brick fence, 3 pane, 4 wall; bit 3: walls keep their post under it;",
        "// bit 4: flowers and saplings grow on it), block entity type",
        "const BlockData blocks[] = {",
    ]
    properties = []  # (name, first value, count)
    values = []
    shape_bytes = []
    collision_box_list = []  # distinct boxes
    collision_box_index = {}
    collision_shape_list = []  # distinct shapes as (first box, count)
    collision_shape_index = {}
    collision_ids = []  # per internal state
    collision_overrides = []  # (release, internal state, shape)

    def intern_collision(shape):
        key = tuple(tuple(float(c) for c in box) for box in shape)
        if key not in collision_shape_index:
            first = len(collision_box_list)
            collision_box_list.extend(key)
            collision_shape_index[key] = len(collision_shape_list)
            collision_shape_list.append((first, len(key)))
        return collision_shape_index[key]

    intern_collision([])  # shape 0: no collision
    light_bytes = []
    join_bytes = []
    support_bytes = []
    support_overrides = []  # (release, internal state, support byte)
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
        own = [intern_collision(shapes["shapes"][str(i)]) for i in ids]
        collision_ids.extend(own)
        # Other releases with the same schema may give a state another shape.
        for release, other_blocks, other_shapes, _ in versions:
            other = next((ob for ob in other_blocks if ob["name"] == b["name"] and schema(ob) == sch), None)
            if other is None or other_shapes is shapes:
                continue
            other_ids = other_shapes["blocks"].get(b["name"], 0)
            other_ids = other_ids if isinstance(other_ids, list) else [other_ids] * count
            for offset, (mine, theirs) in enumerate(zip(own, other_ids)):
                shape = intern_collision(other_shapes["shapes"][str(theirs)])
                if shape != mine:
                    collision_overrides.append((release, first_state + offset, shape))
        combos = list(itertools.product(*[vals for _, vals in sch])) or [()]
        names = [pname for pname, _ in sch]
        default = dict(zip(names, combos[b["defaultState"] - b["minStateId"]]))
        for combo, i in zip(combos, ids):
            join_bytes.append(join_faces(b["name"], dict(zip(names, combo)), shapes["shapes"][str(i)]))
            support_bytes.append(support_byte(b["name"], dict(zip(names, combo)), shapes["shapes"][str(i)]))
            boxes16 = [tuple(round(c * 16) for c in box) for box in shapes["shapes"][str(i)]]
            e, d, occlusion = light_of(b["name"], dict(zip(names, combo)), b, boxes16, default)
            motion = 1 if motion_blocking(b["name"], dict(zip(names, combo)), boxes16) else 0
            light_bytes.append(e | {0: 0, 1: 1, 15: 2}[d] << 4 | motion << 6)
            if occlusion is None:
                occlusion_ids.append(0)
            else:
                occlusion_ids.append(occlusion_shapes.setdefault(face_masks(occlusion), len(occlusion_shapes) + 1))
        # Another release's shape of a state may support otherwise.
        for release, other_blocks, other_shapes, _ in versions:
            other = next((ob for ob in other_blocks if ob["name"] == b["name"] and schema(ob) == sch), None)
            if other is None or other_shapes is shapes:
                continue
            other_ids = other_shapes["blocks"].get(b["name"], 0)
            other_ids = other_ids if isinstance(other_ids, list) else [other_ids] * count
            for offset, (combo, theirs) in enumerate(zip(combos, other_ids)):
                byte = support_byte(b["name"], dict(zip(names, combo)), other_shapes["shapes"][str(theirs)])
                if byte != support_bytes[first_state - 1 + offset]:
                    support_overrides.append((release, first_state + offset, byte))
        hardness = b.get("hardness")
        hardness = -1.0 if hardness is None or hardness < 0 else float(hardness)
        opaque = 1 if b.get("filterLight", 0) == 15 and not b.get("transparent", False) else 0
        tool = tool_of(b.get("material"))
        resistance = float(b["resistance"])
        for release, other_blocks, _, _ in versions:
            other = next((ob for ob in other_blocks if ob["name"] == b["name"] and schema(ob) == sch), None)
            if other is not None and float(other["resistance"]) != resistance:
                sys.exit(f"{name}: blast resistance differs in {release}")
        push = push_class(b["name"], b.get("hardness"))
        sound = instrument(b["name"], b.get("material"))
        conducts = conductor(b["name"])
        # The full-cube rule must give each state the same answer in every release.
        if conducts == 2:
            for release, other_blocks, other_shapes, _ in versions:
                other = next((ob for ob in other_blocks if ob["name"] == b["name"] and schema(ob) == sch), None)
                if other is None or other_shapes is shapes:
                    continue
                other_ids = other_shapes["blocks"].get(b["name"], 0)
                other_ids = other_ids if isinstance(other_ids, list) else [other_ids] * count
                for mine, theirs in zip(ids, other_ids):
                    if (shapes["shapes"][str(mine)] == [[0, 0, 0, 1, 1, 1]]) != (other_shapes["shapes"][str(theirs)] == [[0, 0, 0, 1, 1, 1]]):
                        sys.exit(f"{name}: a state is a full cube in one release only")
        for release, other_blocks, _, _ in versions:
            other = next((ob for ob in other_blocks if ob["name"] == b["name"] and schema(ob) == sch), None)
            if other is not None and instrument(other["name"], other.get("material")) != sound:
                sys.exit(f"{name}: instrument differs in {release}")
        out.append(f'    {{"{name}", {first_prop}, {len(sch)}, {first_state}, {count}, {b["defaultState"] - b["minStateId"]}, '
                   f'{hardness!r}f, {opaque}, {tool}, {tier_of(b, tool, items)}, {resistance!r}f, {push}, {sound}, {conducts}, '
                   f'{connector(b["name"], sch) | (16 if b["name"] in SOIL else 0)}, '
                   + (f'"{block_entity_type(b["name"])}"' if block_entity_type(b["name"]) else "nullptr") + "},")
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
    out += ["// Per internal state (from 1): the faces fences, panes and walls join (bits 0-5: down, up, north, south,",
            "// west, east).",
            "const std::uint8_t join_faces[] = {"]
    for i in range(0, len(join_bytes), 32):
        out.append("    " + ", ".join(str(c) for c in join_bytes[i:i + 32]) + ",")
    out.append("};")
    out += ["// Per internal state (from 1): what it supports (bit 0 the top's middle, bit 1 the bottom's middle, bits 2-7",
            "// whole faces: down, up, north, south, west, east).",
            "const std::uint8_t support[] = {"]
    for i in range(0, len(support_bytes), 32):
        out.append("    " + ", ".join(str(c) for c in support_bytes[i:i + 32]) + ",")
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
    out += ["// Collision shapes (minecraft-data blockCollisionShapes.json): distinct boxes, shapes as (first box, count),",
            "// each internal state's shape, and the states a release gives another shape.",
            "const CollisionBox collision_boxes[] = {"]
    for box in collision_box_list or [(0.0,) * 6]:
        out.append("    {" + ", ".join(repr(v) for v in box) + "},")
    out += ["};", "const CollisionShape collision_shapes[] = {"]
    for first, count in collision_shape_list:
        out.append(f"    {{{first}, {count}}},")
    out += ["};", "const std::uint16_t collision_shape[] = {"]
    for i in range(0, len(collision_ids), 32):
        out.append("    " + ", ".join(str(c) for c in collision_ids[i:i + 32]) + ",")
    out += ["};", "const CollisionOverride collision_overrides[] = {"]
    for release, state, shape in collision_overrides or [("", 0, 0)]:
        out.append(f'    {{"{release}", {state}, {shape}}},')
    out += ["};", f"const std::size_t collision_override_count = {len(collision_overrides)};", ""]
    out += ["// The states a release's shape makes support otherwise (support).", "const SupportOverride support_overrides[] = {"]
    for release, state, byte in support_overrides or [("", 0, 0)]:
        out.append(f'    {{"{release}", {state}, {byte}}},')
    out += ["};", f"const std::size_t support_override_count = {len(support_overrides)};", ""]

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
