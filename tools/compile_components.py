#!/usr/bin/env python3
"""Compiles the network schema of item data components for each protocol.

Reads PrismarineJS minecraft-data (MIT) at the pinned commit: protocol.json of
each version, whose `SlotComponentType` maps component type ids to names and
whose `SlotComponent` switch gives the network layout of each component's
value in ProtoDef notation; and the static registry tables (effects, entities,
attributes, sounds, particles) for their sizes.

Each ProtoDef type expression becomes a node of a flat table that
src/protocol/components.cpp interprets: it reads a component value, checks
it, and writes it again in canonical form. Named types are expanded where
they are used, except a type used inside itself, which refers back to its
enclosing expansion.

Every number that names an entry of a registry or an enumeration is
annotated below with that registry, so that a value outside it is refused
instead of reaching a client that cannot decode it. The generator fails when
a component has a varint or a registry holder that is neither annotated nor
listed as a plain number, so a new component cannot slip through unchecked.

Usage:
  tools/compile_components.py <data>/1.21.8:772 <data>/1.21.4:769 --commit <sha> \\
      --output src/protocol/component_schema.cpp
"""
import argparse
import json
import os
import sys

# Registry or enumeration of each id-like number, by path from the component
# (fields joined with '.', array elements as '[]', switch cases by value;
# '#holder' and '#set' name registry holders and holder sets).
REGISTRY = {
    "attribute_modifiers[].typeId": "attribute",
    "attribute_modifiers.attributes[].typeId": "attribute",
    "axolotl/variant": "enum:axolotl_variant",
    "banner_patterns.layers[].colorId": "enum:dye_color",
    "banner_patterns.layers[].pattern#holder": "banner_pattern",
    "base_color": "enum:dye_color",
    "blocks_attacks.blockSound#holder": "sound_event",
    "blocks_attacks.damageReductions[].type#set": "damage_type",
    "blocks_attacks.disableSound#holder": "sound_event",
    "break_sound#holder": "sound_event",
    "can_break.predicates[].blockSet#set": "block",
    "can_break.predicates[].components.partialMatchers[]": "data_component_predicate_type",
    "can_place_on.predicates[].blockSet#set": "block",
    "can_place_on.predicates[].components.partialMatchers[]": "data_component_predicate_type",
    "cat/collar": "enum:dye_color",
    "cat/variant": "cat_variant",
    "chicken/variant.variant.true": "chicken_variant",
    "consumable.effects[].apply_effects.effects[].id": "mob_effect",
    "consumable.effects[].play_sound.sound#holder": "sound_event",
    "consumable.effects[].remove_effects.effects#set": "mob_effect",
    "consumable.sound#holder": "sound_event",
    "cow/variant": "cow_variant",
    "death_protection.effects[].apply_effects.effects[].id": "mob_effect",
    "death_protection.effects[].play_sound.sound#holder": "sound_event",
    "death_protection.effects[].remove_effects.effects#set": "mob_effect",
    "enchantments.enchantments[].id": "enchantment",
    "equippable.allowedEntities#set": "entity_type",
    "equippable.shearingSound#holder": "sound_event",
    "equippable.sound#holder": "sound_event",
    "fox/variant": "enum:fox_variant",
    "frog/variant": "frog_variant",
    "horse/variant": "enum:horse_variant",
    "instrument#holder": "instrument",
    "instrument.data.soundEvent#holder": "sound_event",
    "instrument.data.true#holder": "instrument",
    "instrument.data.true.data.soundEvent#holder": "sound_event",
    "jukebox_playable.song.true#holder": "jukebox_song",
    "jukebox_playable.song.true.data.soundEvent#holder": "sound_event",
    "llama/variant": "enum:llama_variant",
    "map_post_processing": "enum:map_post_processing",
    "mooshroom/variant": "enum:mooshroom_variant",
    "painting/variant#holder": "painting_variant",
    "parrot/variant": "enum:parrot_variant",
    "pig/variant": "pig_variant",
    "pot_decorations.decorations[]": "item",
    "potion_contents.customEffects[].id": "mob_effect",
    "potion_contents.potionId": "potion",
    "provides_trim_material.material.true#holder": "trim_material",
    "rabbit/variant": "enum:rabbit_variant",
    "repairable.items#set": "item",
    "salmon/size": "enum:salmon_size",
    "sheep/color": "enum:dye_color",
    "shulker/color": "enum:dye_color",
    "stored_enchantments.enchantments[].id": "enchantment",
    "suspicious_stew_effects.effects[].effect": "mob_effect",
    "tool.rules[].blocks#set": "block",
    "tooltip_display.hiddenComponents[]": "data_component_type",
    "trim.material#holder": "trim_material",
    "trim.material.data.ingredientId": "item",
    "trim.pattern#holder": "trim_pattern",
    "trim.pattern.data.templateItemId": "item",
    "tropical_fish/base_color": "enum:dye_color",
    "tropical_fish/pattern": "enum:tropical_fish_pattern",
    "tropical_fish/pattern_color": "enum:dye_color",
    "villager/variant": "villager_type",
    "wolf/collar": "enum:dye_color",
    "wolf/sound_variant": "wolf_sound_variant",
    "wolf/variant": "wolf_variant",
}
# Plain numbers: counts, amounts, durations and levels.
PLAIN = {
    "bees.bees[].minTicksInHive", "bees.bees[].ticksInHive",
    "consumable.effects[].apply_effects.effects[].details.amplifier",
    "consumable.effects[].apply_effects.effects[].details.duration",
    "damage",
    "death_protection.effects[].apply_effects.effects[].details.amplifier",
    "death_protection.effects[].apply_effects.effects[].details.duration",
    "enchantable", "enchantments.enchantments[].level", "fireworks.flightDuration", "food.nutrition",
    "jukebox_playable.song.true.data.comparatorOutput", "map_id", "max_damage", "max_stack_size",
    "ominous_bottle_amplifier", "potion_contents.customEffects[].details.amplifier",
    "potion_contents.customEffects[].details.duration", "repair_cost", "stored_enchantments.enchantments[].level",
    "suspicious_stew_effects.effects[].duration", "tool.damagePerBlock", "weapon.itemDamagePerAttack",
    "written_book_content.generation",
}
# Lists that hold a map or a set: the game compares them without regard to
# order and sends them in hash order, so their canonical form sorts them.
SORTED = {
    "enchantments.enchantments", "stored_enchantments.enchantments", "block_state.properties",
    "tooltip_display.hiddenComponents",
}
# Sizes of enumerations sent as plain varints (minecraft.wiki entity data
# pages). Values outside them make the client fall back to a default, so a
# wrong size refuses valid stacks or lets a harmless value through.
ENUMS = {
    "dye_color": 16, "axolotl_variant": 5, "fox_variant": 2, "horse_variant": 7, "llama_variant": 4,
    "map_post_processing": 2, "mooshroom_variant": 2, "parrot_variant": 5, "salmon_size": 3, "tropical_fish_pattern": 12,
    "rabbit_variant": 6,
}
# Static registries without a minecraft-data table: their entries are listed
# on minecraft.wiki ("Potion" data values: 46 potions; "Villager" types: 7),
# and the official servers of both versions send the last potion, infested,
# as id 45 (tests/fixtures/components).
LISTED = {"potion": 46, "villager_type": 7}
# Static registries whose sizes come from the minecraft-data tables.
STATIC_TABLES = {"mob_effect": "effects", "attribute": "attributes", "sound_event": "sounds", "entity_type": "entities",
                 "particle_type": "particles"}
OPS = ["void", "boolean", "i8", "u8", "i16", "u16", "i32", "i64", "f32", "f64", "varint", "varlong", "string", "uuid",
       "position", "nbt", "optional_nbt", "option", "array", "container", "switch", "mapper", "bitfield", "holder",
       "holder_set", "slot", "component", "buffer", "ref"]
SCALARS = {"void": "void", "bool": "boolean", "i8": "i8", "u8": "u8", "i16": "i16", "u16": "u16", "i32": "i32",
           "i64": "i64", "f32": "f32", "f64": "f64", "varint": "varint", "varlong": "varlong", "UUID": "uuid",
           "position": "position", "anonymousNbt": "nbt", "anonOptionalNbt": "optional_nbt", "string": "string"}


class Compiler:
    def __init__(self, types):
        self.types = types
        self.nodes = []      # (op, registry, a, b, c, d)
        self.fields = []     # (name, node, anon)
        self.cases = []      # (value, node)
        self.mappings = []   # (key, value)
        self.bits = []       # (name, size, signed)
        self.used = set()
        self.unclassified = []

    def add(self, op, registry="", a=0, b=0, c=0, d=0):
        self.nodes.append([OPS.index(op), registry, a, b, c, d])
        return len(self.nodes) - 1

    def registry_for(self, path):
        if path in REGISTRY:
            self.used.add(path)
            return REGISTRY[path]
        return None

    def compile(self, x, path, stack):
        if isinstance(x, str):
            if x in SCALARS:
                op = SCALARS[x]
                registry = ""
                if op in ("varint", "varlong"):
                    registry = self.registry_for(path) or ""
                    if not registry:
                        if path in PLAIN:
                            self.used.add(path)
                        else:
                            self.unclassified.append(path)
                return self.add(op, registry)
            if x == "Slot":
                return self.add("slot")
            if x == "SlotComponent":
                return self.add("component")
            if x == "ByteArray":
                return self.add("buffer")
            for name, node in stack:
                if name == x:
                    return self.add("ref", "", node)
            if x not in self.types or self.types[x] == "native":
                sys.exit(f"unknown type {x} at {path}")
            placeholder = self.add("ref")
            body = self.compile(self.types[x], path, stack + [(x, placeholder)])
            self.nodes[placeholder][2] = body
            return placeholder
        kind, arg = x[0], (x[1] if len(x) > 1 else None)
        if kind == "pstring":
            if arg.get("countType") != "varint":
                sys.exit(f"pstring without a varint length at {path}")
            return self.add("string")
        if kind == "buffer":
            return self.add("buffer")
        if kind == "option":
            node = self.add("option")
            self.nodes[node][2] = self.compile(arg, path, stack)
            return node
        if kind == "array":
            node = self.add("array")
            count = arg.get("count")
            if arg.get("countType", "varint") != "varint" and not count:
                sys.exit(f"array without a varint count at {path}")
            self.nodes[node][2] = self.compile(arg["type"], path + "[]", stack)
            self.nodes[node][3] = self.string(count) if count else 0
            if path in SORTED:
                self.used.add(path)
                self.nodes[node][4] = 1
            return node
        if kind == "container":
            node = self.add("container")
            entries = []
            for f in arg:
                name = f.get("name", "")
                sub = path + ("." + name if not f.get("anon") else "")
                entries.append((name, self.compile(f["type"], sub, stack), 1 if f.get("anon") else 0))
            self.nodes[node][2] = len(self.fields)
            self.nodes[node][3] = len(entries)
            self.fields.extend(entries)
            return node
        if kind == "switch":
            node = self.add("switch")
            entries = [(value, self.compile(case, path + "." + value, stack)) for value, case in arg["fields"].items()]
            default = self.compile(arg["default"], path + ".default", stack) + 1 if "default" in arg else 0
            self.nodes[node][2] = self.string(arg["compareTo"])
            self.nodes[node][3] = len(self.cases)
            self.nodes[node][4] = len(entries)
            self.nodes[node][5] = default
            self.cases.extend(entries)
            return node
        if kind == "mapper":
            node = self.add("mapper")
            if arg["type"] != "varint":
                sys.exit(f"mapper over {arg['type']} at {path}")
            self.nodes[node][2] = len(self.mappings)
            self.nodes[node][3] = len(arg["mappings"])
            self.mappings.extend((int(k), v) for k, v in arg["mappings"].items())
            return node
        if kind == "bitfield":
            node = self.add("bitfield")
            self.nodes[node][2] = len(self.bits)
            self.nodes[node][3] = len(arg)
            self.bits.extend((b["name"], b["size"], 1 if b["signed"] else 0) for b in arg)
            return node
        if kind == "registryEntryHolder":
            registry = self.registry_for(path + "#holder")
            if not registry:
                self.unclassified.append(path + "#holder")
            node = self.add("holder", registry or "")
            other = arg["otherwise"]
            self.nodes[node][2] = self.compile(other["type"], path + "." + other["name"], stack)
            return node
        if kind == "registryEntryHolderSet":
            registry = self.registry_for(path + "#set")
            if not registry:
                self.unclassified.append(path + "#set")
            if arg["base"]["type"] != "string" or arg["otherwise"]["type"] != "varint":
                sys.exit(f"unexpected holder set at {path}")
            return self.add("holder_set", registry or "")
        sys.exit(f"unsupported ProtoDef type {kind} at {path}")

    def string(self, s):
        return s


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("versions", nargs="+", help="<data dir>:<protocol>")
    parser.add_argument("--commit", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    strings = [""]
    string_index = {"": 0}

    def intern(s):
        if s not in string_index:
            string_index[s] = len(strings)
            strings.append(s)
        return string_index[s]

    out_tables = []
    used_paths = set()
    for spec in args.versions:
        directory, protocol = spec.rsplit(":", 1)
        release = os.path.basename(os.path.normpath(directory))
        types = json.load(open(os.path.join(directory, "protocol.json"), encoding="utf-8"))["types"]
        names = types["SlotComponentType"][1]["mappings"]
        switch = types["SlotComponent"][1][1]["type"][1]["fields"]
        comp = Compiler(types)
        components = []
        for type_id in sorted(names, key=int):
            name = names[type_id]
            components.append((int(type_id), name, comp.compile(switch[name], name, [])))
        if [c[0] for c in components] != list(range(len(components))):
            sys.exit(f"{release}: component type ids are not contiguous")
        if comp.unclassified:
            sys.exit(f"{release}: unclassified ids: " + ", ".join(sorted(set(comp.unclassified))))
        used_paths |= comp.used
        sizes = {}
        for registry, table in STATIC_TABLES.items():
            sizes[registry] = len(json.load(open(os.path.join(directory, "tables", table + ".json"), encoding="utf-8")))
        sizes["data_component_type"] = len(components)
        for name, size in ENUMS.items():
            sizes["enum:" + name] = size
        sizes.update(LISTED)
        out_tables.append((int(protocol), release, comp, components, sizes))

    unused = (set(REGISTRY) | PLAIN | SORTED) - used_paths
    if unused:
        sys.exit("annotations that match nothing: " + ", ".join(sorted(unused)))

    for _, _, comp, components, sizes in out_tables:
        for n in comp.nodes:
            intern(n[1])
        for f in comp.fields:
            intern(f[0])
        for c in comp.cases:
            intern(c[0])
        for m in comp.mappings:
            intern(m[1])
        for b in comp.bits:
            intern(b[0])
        for _, name, _ in components:
            intern(name)
        for n in comp.nodes:
            if OPS[n[0]] in ("array",) and n[3]:
                intern(n[3])
            if OPS[n[0]] == "switch":
                intern(n[2])
        for k in sizes:
            intern(k)

    out = [
        "// SPDX-License-Identifier: AGPL-3.0-only",
        "// Generated by tools/compile_components.py from PrismarineJS minecraft-data (MIT),",
        f"// commit {args.commit}, protocol.json and registry tables of "
        + ", ".join(r for _, r, *_ in out_tables) + ". Do not edit by hand.",
        '#include "component_schema.hpp"',
        "#include <iterator>",
        "",
        "namespace circuit::protocol::detail {",
        "namespace {",
        "const char* const strings[] = {",
    ]
    for i in range(0, len(strings), 8):
        out.append("    " + ", ".join(c_string(s) for s in strings[i:i + 8]) + ",")
    out.append("};")
    for protocol, release, comp, components, sizes in out_tables:
        out.append(f"// {release}")
        out.append(f"const SchemaNode nodes_{protocol}[] = {{")
        for op, registry, a, b, c, d in comp.nodes:
            if OPS[op] == "array":
                b = string_index[b] if b else 0
            if OPS[op] == "switch":
                a = string_index[a]
            out.append(f"    {{SchemaOp::{OPS[op]}_, {string_index[registry]}, {a}, {b}, {c}, {d}}},")
        out.append("};")
        out.append(f"const SchemaField fields_{protocol}[] = {{")
        for name, node, anon in comp.fields:
            out.append(f"    {{{string_index[name]}, {node}, {anon}}},")
        out.append("    {0, 0, 0},")
        out.append("};")
        out.append(f"const SchemaCase cases_{protocol}[] = {{")
        for value, node in comp.cases:
            out.append(f"    {{{string_index[value]}, {node}}},")
        out.append("    {0, 0},")
        out.append("};")
        out.append(f"const SchemaMapping mappings_{protocol}[] = {{")
        for key, value in comp.mappings:
            out.append(f"    {{{key}, {string_index[value]}}},")
        out.append("    {0, 0},")
        out.append("};")
        out.append(f"const SchemaBit bits_{protocol}[] = {{")
        for name, size, signed in comp.bits:
            out.append(f"    {{{string_index[name]}, {size}, {signed}}},")
        out.append("    {0, 0, 0},")
        out.append("};")
        out.append(f"const SchemaComponent components_{protocol}[] = {{")
        for _, name, node in components:
            out.append(f"    {{{string_index[name]}, {node}}},")
        out.append("};")
        out.append(f"const SchemaRegistrySize sizes_{protocol}[] = {{")
        for name, size in sorted(sizes.items()):
            out.append(f"    {{{string_index[name]}, {size}}},")
        out.append("};")
    out.append("} // namespace")
    out.append("")
    out.append("const char* schema_string(std::uint32_t index) noexcept { return strings[index]; }")
    out.append("")
    out.append("const ComponentSchema* component_schema(std::int32_t protocol) noexcept {")
    for protocol, release, *_ in out_tables:
        out.append(f"    static const ComponentSchema s{protocol}{{{protocol}, nodes_{protocol}, fields_{protocol}, cases_{protocol}, "
                   f"mappings_{protocol}, bits_{protocol}, components_{protocol}, std::size(components_{protocol}), "
                   f"sizes_{protocol}, std::size(sizes_{protocol})}};")
        out.append(f"    if (protocol == {protocol}) return &s{protocol};")
    out += ["    return nullptr;", "}", "", "} // namespace circuit::protocol::detail", ""]
    with open(args.output, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    for protocol, release, comp, components, _ in out_tables:
        print(f"{release}: {len(components)} component types, {len(comp.nodes)} nodes")


if __name__ == "__main__":
    main()
