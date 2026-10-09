// SPDX-License-Identifier: AGPL-3.0-only
#include "registries.hpp"
#include <circuit/protocol/content.hpp>

namespace circuit::server {
namespace nbt = protocol::nbt;
namespace {
// Registry entries for protocol 772 (Java Edition 1.21.8), hand-compiled from
// public documentation:
// - dimension types: minecraft.wiki "Dimension type", revision 3019989;
// - biomes: minecraft.wiki "Biome" (Biome IDs) and PrismarineJS minecraft-data
//   1.21.8 biomes.json (commit 095709f3), which agree;
// - damage types: minecraft.wiki "Damage type", revision 3162861;
// - chat types: minecraft.wiki "Chat type", revision 3032067;
// - mob and painting variants: the pages cited at each registry. The 1.21.8
//   client requires these registries to be non-empty.
// Entries are sent without data to clients that share the "minecraft:core"
// pack. Their order defines the numeric ids; the first entry of each registry
// is the one the server uses.
const std::vector<RegistryEntries> registries_772{
    {"minecraft:dimension_type",
     {
        "minecraft:overworld", "minecraft:overworld_caves", "minecraft:the_nether", "minecraft:the_end",
     }},
    {"minecraft:worldgen/biome",
     {
        "minecraft:plains", "minecraft:badlands", "minecraft:bamboo_jungle", "minecraft:basalt_deltas",
        "minecraft:beach", "minecraft:birch_forest", "minecraft:cherry_grove", "minecraft:cold_ocean",
        "minecraft:crimson_forest", "minecraft:dark_forest", "minecraft:deep_cold_ocean", "minecraft:deep_dark",
        "minecraft:deep_frozen_ocean", "minecraft:deep_lukewarm_ocean", "minecraft:deep_ocean", "minecraft:desert",
        "minecraft:dripstone_caves", "minecraft:end_barrens", "minecraft:end_highlands", "minecraft:end_midlands",
        "minecraft:eroded_badlands", "minecraft:flower_forest", "minecraft:forest", "minecraft:frozen_ocean",
        "minecraft:frozen_peaks", "minecraft:frozen_river", "minecraft:grove", "minecraft:ice_spikes",
        "minecraft:jagged_peaks", "minecraft:jungle", "minecraft:lukewarm_ocean", "minecraft:lush_caves",
        "minecraft:mangrove_swamp", "minecraft:meadow", "minecraft:mushroom_fields", "minecraft:nether_wastes",
        "minecraft:ocean", "minecraft:old_growth_birch_forest", "minecraft:old_growth_pine_taiga",
        "minecraft:old_growth_spruce_taiga", "minecraft:pale_garden", "minecraft:river", "minecraft:savanna",
        "minecraft:savanna_plateau", "minecraft:small_end_islands", "minecraft:snowy_beach", "minecraft:snowy_plains",
        "minecraft:snowy_slopes", "minecraft:snowy_taiga", "minecraft:soul_sand_valley", "minecraft:sparse_jungle",
        "minecraft:stony_peaks", "minecraft:stony_shore", "minecraft:sunflower_plains", "minecraft:swamp",
        "minecraft:taiga", "minecraft:the_end", "minecraft:the_void", "minecraft:warm_ocean",
        "minecraft:warped_forest", "minecraft:windswept_forest", "minecraft:windswept_gravelly_hills",
        "minecraft:windswept_hills", "minecraft:windswept_savanna", "minecraft:wooded_badlands",
     }},
    {"minecraft:damage_type",
     {
        "minecraft:arrow", "minecraft:bad_respawn_point", "minecraft:cactus", "minecraft:campfire",
        "minecraft:cramming", "minecraft:dragon_breath", "minecraft:drown", "minecraft:dry_out",
        "minecraft:ender_pearl", "minecraft:explosion", "minecraft:fall", "minecraft:falling_anvil",
        "minecraft:falling_block", "minecraft:falling_stalactite", "minecraft:fireball", "minecraft:fireworks",
        "minecraft:fly_into_wall", "minecraft:freeze", "minecraft:generic", "minecraft:generic_kill",
        "minecraft:hot_floor", "minecraft:in_fire", "minecraft:in_wall", "minecraft:indirect_magic", "minecraft:lava",
        "minecraft:lightning_bolt", "minecraft:mace_smash", "minecraft:magic", "minecraft:mob_attack",
        "minecraft:mob_attack_no_aggro", "minecraft:mob_projectile", "minecraft:on_fire", "minecraft:out_of_world",
        "minecraft:outside_border", "minecraft:player_attack", "minecraft:player_explosion", "minecraft:sonic_boom",
        "minecraft:spit", "minecraft:stalagmite", "minecraft:starve", "minecraft:sting", "minecraft:sweet_berry_bush",
        "minecraft:thorns", "minecraft:thrown", "minecraft:trident", "minecraft:unattributed_fireball",
        "minecraft:wind_charge", "minecraft:wither", "minecraft:wither_skull",
     }},
    {"minecraft:chat_type",
     {
        "minecraft:chat", "minecraft:emote_command", "minecraft:msg_command_incoming",
        "minecraft:msg_command_outgoing", "minecraft:say_command", "minecraft:team_msg_command_incoming",
        "minecraft:team_msg_command_outgoing",
     }},
    // minecraft.wiki "Cat" revision 3166263
    {"minecraft:cat_variant",
     {
        "minecraft:all_black", "minecraft:black", "minecraft:british_shorthair", "minecraft:calico",
        "minecraft:jellie", "minecraft:persian", "minecraft:ragdoll", "minecraft:red", "minecraft:siamese",
        "minecraft:tabby", "minecraft:white",
     }},
    // minecraft.wiki "Chicken" revision 3170487 (temperate, warm and cold chickens)
    {"minecraft:chicken_variant",
     {
        "minecraft:temperate", "minecraft:warm", "minecraft:cold",
     }},
    // minecraft.wiki "Cow" revision 3166539
    {"minecraft:cow_variant",
     {
        "minecraft:temperate", "minecraft:warm", "minecraft:cold",
     }},
    // minecraft.wiki "Frog" revision 3166353
    {"minecraft:frog_variant",
     {
        "minecraft:temperate", "minecraft:warm", "minecraft:cold",
     }},
    // minecraft.wiki "Painting" (Canvases) revision 3164308, with the four
    // element paintings it lists as unplaceable (earth, wind, fire, water);
    // they were missing until the official servers' registries showed them.
    {"minecraft:painting_variant",
     {
        "minecraft:kebab", "minecraft:aztec", "minecraft:alban", "minecraft:aztec2", "minecraft:bomb",
        "minecraft:plant", "minecraft:wasteland", "minecraft:meditative", "minecraft:wanderer", "minecraft:graham",
        "minecraft:prairie_ride", "minecraft:pool", "minecraft:courbet", "minecraft:sunset", "minecraft:sea",
        "minecraft:creebet", "minecraft:match", "minecraft:bust", "minecraft:stage", "minecraft:void",
        "minecraft:skull_and_roses", "minecraft:wither", "minecraft:baroque", "minecraft:humble", "minecraft:bouquet",
        "minecraft:cavebird", "minecraft:cotan", "minecraft:endboss", "minecraft:fern", "minecraft:owlemons",
        "minecraft:sunflowers", "minecraft:tides", "minecraft:dennis", "minecraft:backyard", "minecraft:pond",
        "minecraft:fighters", "minecraft:changing", "minecraft:finding", "minecraft:lowmist", "minecraft:passage",
        "minecraft:skeleton", "minecraft:donkey_kong", "minecraft:pointer", "minecraft:pigscene",
        "minecraft:burning_skull", "minecraft:orb", "minecraft:unpacked", "minecraft:earth", "minecraft:wind",
        "minecraft:fire", "minecraft:water",
     }},
    // minecraft.wiki "Pig" revision 3159133
    {"minecraft:pig_variant",
     {
        "minecraft:temperate", "minecraft:warm", "minecraft:cold",
     }},
    // minecraft.wiki "Wolf" (Sound Variants) revision 3171446
    {"minecraft:wolf_sound_variant",
     {
        "minecraft:classic", "minecraft:angry", "minecraft:big", "minecraft:cute", "minecraft:grumpy",
        "minecraft:puglin", "minecraft:sad",
     }},
    // minecraft.wiki "Wolf" (Variants) revision 3171446
    {"minecraft:wolf_variant",
     {
        "minecraft:pale", "minecraft:ashen", "minecraft:black", "minecraft:chestnut", "minecraft:rusty",
        "minecraft:snowy", "minecraft:spotted", "minecraft:striped", "minecraft:woods",
     }},
    // minecraft.wiki "Smithing" revision 3820533 (trim materials and patterns,
    // the same in 1.21.4 and 1.21.8). Their definitions refer to no tags.
    {"minecraft:trim_material",
     {
        "minecraft:amethyst", "minecraft:copper", "minecraft:diamond", "minecraft:emerald", "minecraft:gold",
        "minecraft:iron", "minecraft:lapis", "minecraft:netherite", "minecraft:quartz", "minecraft:redstone",
        "minecraft:resin",
     }},
    {"minecraft:trim_pattern",
     {
        "minecraft:bolt", "minecraft:coast", "minecraft:dune", "minecraft:eye", "minecraft:flow", "minecraft:host",
        "minecraft:raiser", "minecraft:rib", "minecraft:sentry", "minecraft:shaper", "minecraft:silence",
        "minecraft:snout", "minecraft:spire", "minecraft:tide", "minecraft:vex", "minecraft:ward", "minecraft:wayfinder",
        "minecraft:wild",
     }},
    // minecraft.wiki "Banner/Patterns" revision 3556873 (Java Edition resource names)
    {"minecraft:banner_pattern",
     {
        "minecraft:base", "minecraft:stripe_bottom", "minecraft:stripe_top", "minecraft:stripe_left",
        "minecraft:stripe_right", "minecraft:stripe_center", "minecraft:stripe_middle", "minecraft:stripe_downright",
        "minecraft:stripe_downleft", "minecraft:small_stripes", "minecraft:cross", "minecraft:straight_cross",
        "minecraft:diagonal_left", "minecraft:diagonal_right", "minecraft:diagonal_up_left", "minecraft:diagonal_up_right",
        "minecraft:half_vertical", "minecraft:half_vertical_right", "minecraft:half_horizontal",
        "minecraft:half_horizontal_bottom", "minecraft:square_bottom_left", "minecraft:square_bottom_right",
        "minecraft:square_top_left", "minecraft:square_top_right", "minecraft:triangle_bottom", "minecraft:triangle_top",
        "minecraft:triangles_bottom", "minecraft:triangles_top", "minecraft:circle", "minecraft:rhombus",
        "minecraft:border", "minecraft:curly_border", "minecraft:bricks", "minecraft:gradient", "minecraft:gradient_up",
        "minecraft:creeper", "minecraft:skull", "minecraft:flower", "minecraft:mojang", "minecraft:globe",
        "minecraft:piglin", "minecraft:flow", "minecraft:guster",
     }},
    // minecraft.wiki "Instrument definition" revision 3778776 (goat horns)
    {"minecraft:instrument",
     {
        "minecraft:admire_goat_horn", "minecraft:call_goat_horn", "minecraft:dream_goat_horn",
        "minecraft:feel_goat_horn", "minecraft:ponder_goat_horn", "minecraft:seek_goat_horn",
        "minecraft:sing_goat_horn", "minecraft:yearn_goat_horn",
     }},
    // minecraft.wiki "Music Disc" revision 3798476 (one song per disc),
    // "Music Disc Creator (Music Box)" revision 3808799 and "Music Disc Lava
    // Chicken" revision 3831177. Tears came in 1.21.6 and Lava Chicken in 1.21.7.
    {"minecraft:jukebox_song",
     {
        "minecraft:11", "minecraft:13", "minecraft:5", "minecraft:blocks", "minecraft:cat", "minecraft:chirp",
        "minecraft:creator", "minecraft:creator_music_box", "minecraft:far", "minecraft:lava_chicken",
        "minecraft:mall", "minecraft:mellohi", "minecraft:otherside", "minecraft:pigstep", "minecraft:precipice",
        "minecraft:relic", "minecraft:stal", "minecraft:strad", "minecraft:tears", "minecraft:wait", "minecraft:ward",
     }},
};

// Registry entries for protocol 769 (Java Edition 1.21.4). The cat, chicken,
// cow, frog, pig and wolf-sound variant registries became synchronized only in
// 1.21.5, so they are not sent. Trim materials and patterns, banner patterns
// and instruments are as for 1.21.8; jukebox songs lack Tears and Lava
// Chicken. Sources, all from early 2025 and describing 1.21.4:
// - dimension types, biomes, damage types and chat types: as for 1.21.8.
//   minecraft.wiki "Damage type" revision 2892923 lists the same 49 types
//   (campfire and wind_charge since 1.21, mace_smash and ender_pearl since
//   1.21.2), and minecraft-data's 1.21.4 biomes.json the same 65 biomes;
// - paintings: minecraft.wiki "Painting" revision 2884854. "Dennis" came only
//   in 1.21.7;
// - wolf variants: minecraft.wiki "Wolf" revision 2897968 (9 variants).
std::vector<RegistryEntries> make_769() {
    std::vector<RegistryEntries> out;
    for (const auto& r : registries_772) {
        if (r.registry == "minecraft:dimension_type" || r.registry == "minecraft:worldgen/biome" || r.registry == "minecraft:damage_type" ||
            r.registry == "minecraft:chat_type" || r.registry == "minecraft:wolf_variant" || r.registry == "minecraft:trim_material" ||
            r.registry == "minecraft:trim_pattern" || r.registry == "minecraft:banner_pattern" || r.registry == "minecraft:instrument")
            out.push_back(r);
        else if (r.registry == "minecraft:painting_variant") {
            out.push_back(r);
            std::erase(out.back().entries, std::string_view("minecraft:dennis"));
        } else if (r.registry == "minecraft:jukebox_song") {
            out.push_back(r);
            std::erase(out.back().entries, std::string_view("minecraft:tears"));
            std::erase(out.back().entries, std::string_view("minecraft:lava_chicken"));
        }
    }
    return out;
}

std::vector<RegistryData> data_772(bool with_cloud_height = true) {
    // Overworld column of minecraft.wiki "Dimension type" (revision 3019989).
    auto overworld = nbt::Tag::compound();
    overworld.set("ultrawarm", nbt::Tag::boolean(false));
    overworld.set("natural", nbt::Tag::boolean(true));
    overworld.set("piglin_safe", nbt::Tag::boolean(false));
    overworld.set("respawn_anchor_works", nbt::Tag::boolean(false));
    overworld.set("bed_works", nbt::Tag::boolean(true));
    overworld.set("has_raids", nbt::Tag::boolean(true));
    overworld.set("has_skylight", nbt::Tag::boolean(true));
    overworld.set("has_ceiling", nbt::Tag::boolean(false));
    overworld.set("coordinate_scale", nbt::Tag::double_(1.0));
    overworld.set("ambient_light", nbt::Tag::float_(0.0f));
    overworld.set("min_y", nbt::Tag::int_(-64));
    overworld.set("height", nbt::Tag::int_(384));
    overworld.set("logical_height", nbt::Tag::int_(384));
    if (with_cloud_height) overworld.set("cloud_height", nbt::Tag::int_(192)); // 1.21.5+
    auto light = nbt::Tag::compound();
    light.set("type", nbt::Tag::string("minecraft:uniform"));
    light.set("min_inclusive", nbt::Tag::int_(0));
    light.set("max_inclusive", nbt::Tag::int_(7));
    overworld.set("monster_spawn_light_level", std::move(light));
    overworld.set("monster_spawn_block_light_limit", nbt::Tag::int_(0));
    overworld.set("infiniburn", nbt::Tag::string("#minecraft:infiniburn_overworld"));
    overworld.set("effects", nbt::Tag::string("minecraft:overworld"));

    // Plains row of minecraft.wiki "Biome" (List of biome climates, revision
    // 3167982); water colors are the standard defaults.
    auto plains = nbt::Tag::compound();
    plains.set("has_precipitation", nbt::Tag::boolean(true));
    plains.set("temperature", nbt::Tag::float_(0.8f));
    plains.set("downfall", nbt::Tag::float_(0.4f));
    auto effects = nbt::Tag::compound();
    effects.set("sky_color", nbt::Tag::int_(0x78a7ff));
    effects.set("fog_color", nbt::Tag::int_(0xc0d8ff));
    effects.set("water_color", nbt::Tag::int_(0x3f76e4));
    effects.set("water_fog_color", nbt::Tag::int_(0x050533));
    plains.set("effects", std::move(effects));

    // "chat" row of minecraft.wiki "Chat type" (revision 3032067).
    const auto decoration = [](std::string key) {
        auto d = nbt::Tag::compound();
        d.set("translation_key", nbt::Tag::string(std::move(key)));
        auto params = nbt::Tag::list(nbt::TagType::string);
        params.push(nbt::Tag::string("sender"));
        params.push(nbt::Tag::string("content"));
        d.set("parameters", std::move(params));
        return d;
    };
    auto chat = nbt::Tag::compound();
    chat.set("chat", decoration("chat.type.text"));
    chat.set("narration", decoration("chat.type.text.narrate"));

    std::vector<RegistryData> out;
    out.push_back({"minecraft:dimension_type", {}});
    out.back().entries.emplace_back("minecraft:overworld", std::move(overworld));
    out.push_back({"minecraft:worldgen/biome", {}});
    out.back().entries.emplace_back("minecraft:plains", std::move(plains));
    out.push_back({"minecraft:chat_type", {}});
    out.back().entries.emplace_back("minecraft:chat", std::move(chat));
    return out;
}
} // namespace

namespace {
// The registry set of the bundle that plays a protocol.
std::string_view set_of(std::int32_t protocol) noexcept {
    for (const auto& b : contracts::BundleRegistry::candidates())
        if (b.protocol == protocol && b.rules) return b.schema.registries;
    return {};
}
} // namespace

const std::vector<RegistryEntries>* synced_registries(std::string_view set) noexcept {
    static const auto registries_769 = make_769();
    if (set == "java-1.21.8") return &registries_772;
    if (set == "java-1.21.4") return &registries_769;
    return nullptr;
}
const std::vector<RegistryEntries>* synced_registries(std::int32_t protocol) noexcept { return synced_registries(set_of(protocol)); }

const std::vector<RegistryData>* registries_with_data(std::string_view set) {
    static const auto v772 = data_772();
    static const auto v769 = data_772(false);
    if (set == "java-1.21.8") return &v772;
    if (set == "java-1.21.4") return &v769;
    return nullptr;
}
const std::vector<RegistryData>* registries_with_data(std::int32_t protocol) { return registries_with_data(set_of(protocol)); }

const std::vector<RegistryTags>& synced_tags(std::int32_t protocol) { return synced_tags(set_of(protocol)); }

const std::vector<RegistryTags>& synced_tags(std::string_view set) {
    // The 1.21.4 wolf variants refer to these biome tags, and the client
    // refuses unbound tags. Contents: minecraft.wiki "Biome tag (Java
    // Edition)" revision 2888034.
    static const std::vector<RegistryTags> v769{
        {"minecraft:worldgen/biome",
         {
             {"minecraft:is_badlands", {"minecraft:badlands", "minecraft:eroded_badlands", "minecraft:wooded_badlands"}},
             {"minecraft:is_jungle", {"minecraft:bamboo_jungle", "minecraft:jungle", "minecraft:sparse_jungle"}},
             {"minecraft:is_savanna", {"minecraft:savanna", "minecraft:savanna_plateau", "minecraft:windswept_savanna"}},
         }},
    };
    static const std::vector<RegistryTags> none;
    return set == "java-1.21.4" ? v769 : none;
}

protocol::ComponentContext component_context(const contracts::VersionBundle& bundle) {
    protocol::ComponentContext ctx;
    if (const auto* tables = protocol::ContentTables::for_protocol(bundle.schema.component_schema)) {
        ctx.items = tables->item_count();
        ctx.blocks = tables->block_count();
    }
    const auto* registries = synced_registries(bundle.schema.registries);
    const auto find = [registries](std::string_view name) -> const RegistryEntries* {
        if (!registries) return nullptr;
        for (const auto& r : *registries)
            if (r.registry.substr(r.registry.find(':') + 1) == name) return &r;
        return nullptr;
    };
    ctx.synced = [find](std::string_view name) -> std::optional<std::uint32_t> {
        const auto* r = find(name);
        return r ? std::optional(static_cast<std::uint32_t>(r->entries.size())) : std::nullopt;
    };
    ctx.name_of = [find](std::string_view registry, std::uint32_t id) -> std::optional<std::string_view> {
        const auto* r = find(registry);
        return r && id < r->entries.size() ? std::optional(r->entries[id]) : std::nullopt;
    };
    ctx.id_of = [find](std::string_view registry, std::string_view entry) -> std::optional<std::uint32_t> {
        const auto* r = find(registry);
        if (!r) return std::nullopt;
        const auto full = entry.find(':') == std::string_view::npos ? "minecraft:" + std::string(entry) : std::string(entry);
        for (std::size_t i = 0; i < r->entries.size(); ++i)
            if (r->entries[i] == full) return static_cast<std::uint32_t>(i);
        return std::nullopt;
    };
    return ctx;
}

protocol::ComponentContext component_context(std::int32_t protocol) {
    for (const auto& b : contracts::BundleRegistry::candidates())
        if (b.protocol == protocol && b.rules) return component_context(b);
    return {};
}

std::optional<std::int32_t> registry_index(const std::vector<RegistryEntries>& registries, std::string_view registry,
                                           std::string_view entry) noexcept {
    for (const auto& r : registries) {
        if (r.registry != registry) continue;
        for (std::size_t i = 0; i < r.entries.size(); ++i)
            if (r.entries[i] == entry) return static_cast<std::int32_t>(i);
    }
    return std::nullopt;
}

} // namespace circuit::server
