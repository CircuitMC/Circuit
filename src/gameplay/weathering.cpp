// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Blocks that change in their random ticks by light, neighbors and chance (the
// official 1.21.8 server; melt_weather; minecraft.wiki "Ice", "Snow",
// "Oxidation", "Redstone Ore", "Budding Amethyst"):
// - ice melts (into water, in the Nether into air) when its block light is
//   more than 11 less its light dampening; a snow layer goes with its drops
//   when its block light is more than 11;
// - lit redstone ore goes out;
// - budding amethyst draws nextInt(5) and on 0 a side (nextInt(6): down, up,
//   north, south, west, east): air or a full block of water there takes a
//   small bud facing that side (waterlogged in a water source), and a bud
//   facing it grows a stage, a large bud into a cluster;
// - copper that can still weather draws nextFloat() < 64 / 1125; then, unless
//   a copper block that weathers is less weathered within a taxicab distance
//   of 4, it weathers a stage when nextFloat() < m c^2, c = (b + 1) / (a + 1)
//   with a such blocks around and b of them more weathered, m 0.75 for
//   unweathered copper and 1 otherwise.
namespace circuit::gameplay::detail {
namespace {
std::string_view name_of(BlockState s) {
    const auto state = physics::registry_state_of(s);
    const auto block = state ? registry::block_of(*state) : std::nullopt;
    return block ? registry::game_name(*block) : std::string_view{};
}

BlockState with(BlockState s, std::string_view key, const std::string& value) {
    auto props = registry::properties(s.registry_state());
    for (auto& [k, v] : props)
        if (k == key) v = value;
    return BlockState::registered(registry::state_of(*registry::block_of(s.registry_state()), props).value());
}

std::string value_of(BlockState s, std::string_view key) {
    const auto state = physics::registry_state_of(s);
    if (!state) return {};
    for (const auto& [k, v] : registry::properties(*state))
        if (k == key) return v;
    return {};
}

constexpr std::array<std::string_view, 4> stages{"", "exposed_", "weathered_", "oxidized_"};

std::string_view side_name(Direction d) {
    switch (d) {
    case Direction::down: return "down";
    case Direction::up: return "up";
    case Direction::north: return "north";
    case Direction::south: return "south";
    case Direction::west: return "west";
    default: return "east";
    }
}

// The weathering stage of copper that weathers (not waxed), with the name of
// its kind (`copper` for the copper block).
std::optional<std::pair<int, std::string_view>> copper_stage(std::string_view n) {
    if (n.starts_with("waxed_")) return std::nullopt;
    for (int i = 3; i >= 1; --i)
        if (n.starts_with(stages[static_cast<std::size_t>(i)])) {
            const auto kind = n.substr(stages[static_cast<std::size_t>(i)].size());
            if (kind == "copper" || kind.starts_with("cut_copper") || kind == "chiseled_copper" || kind == "copper_grate"
                || kind == "copper_bulb" || kind == "copper_door" || kind == "copper_trapdoor")
                return std::pair{i, kind};
            return std::nullopt;
        }
    if (n == "copper_block") return std::pair{0, std::string_view("copper")};
    if (n.starts_with("cut_copper") || n == "chiseled_copper" || n == "copper_grate" || n == "copper_bulb" || n == "copper_door"
        || n == "copper_trapdoor")
        return std::pair{0, n};
    return std::nullopt;
}

std::string copper_name(int stage, std::string_view kind) {
    if (stage == 0 && kind == "copper") return "copper_block";
    return std::string(stages[static_cast<std::size_t>(stage)]) + std::string(kind);
}

void weather_copper(RuleContext& ctx, BlockPos pos, BlockState s, int stage, std::string_view kind) {
    if (!(ctx.random_float() < 0.05688889F)) return;
    int same = 0, more = 0;
    for (int dx = -4; dx <= 4; ++dx)
        for (int dy = -4; dy <= 4; ++dy)
            for (int dz = -4; dz <= 4; ++dz) {
                if (std::abs(dx) + std::abs(dy) + std::abs(dz) > 4 || (dx == 0 && dy == 0 && dz == 0)) continue;
                const BlockPos q{pos.x + dx, pos.y + dy, pos.z + dz};
                if (!ctx.world().contains_y(q.y)) continue;
                const auto other = copper_stage(name_of(ctx.block(q)));
                if (!other) continue;
                if (other->first < stage) return;
                if (other->first > stage) ++more;
                else ++same;
            }
    const float c = static_cast<float>(more + 1) / static_cast<float>(more + same + 1);
    const float chance = c * c * (stage == 0 ? 0.75F : 1.0F);
    if (!(ctx.random_float() < chance)) return;
    // The block of the next stage, its properties kept.
    const auto release = ctx.bundle().schema.content_release;
    const auto next = registry::find_block(release, copper_name(stage + 1, kind));
    if (!next) return;
    auto props = registry::properties(s.registry_state());
    const auto state = props.empty() ? std::optional(registry::block(*next).default_state) : registry::state_of(*next, props);
    if (state) set_block(ctx, pos, BlockState::registered(*state));
}

void grow_amethyst(RuleContext& ctx, BlockPos pos) {
    if (ctx.random_int(5) != 0) return;
    const auto side = contracts::all_directions[static_cast<std::size_t>(ctx.random_int(6))];
    const auto at = pos.relative(side);
    const auto there = ctx.block(at);
    const auto n = name_of(there);
    std::string_view grown;
    const bool water = there.is(BlockType::water) && (there.level() == 0 || there.flag());
    if (there.is_air() || water) grown = "small_amethyst_bud";
    else if (value_of(there, "facing") == side_name(side)) {
        if (n == "small_amethyst_bud") grown = "medium_amethyst_bud";
        else if (n == "medium_amethyst_bud") grown = "large_amethyst_bud";
        else if (n == "large_amethyst_bud") grown = "amethyst_cluster";
    }
    if (grown.empty()) return;
    const bool source = (there.is(BlockType::water) && there.level() == 0 && !there.flag()) || value_of(there, "waterlogged") == "true";
    const std::vector<registry::Property> props{{"facing", std::string(side_name(side))}, {"waterlogged", source ? "true" : "false"}};
    const auto block = registry::find_block(ctx.bundle().schema.content_release, grown);
    set_block(ctx, at, BlockState::registered(registry::state_of(*block, props).value()));
}
} // namespace

bool is_weathering(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    const auto n = name_of(s);
    if (n == "ice" || n == "snow" || n == "redstone_ore" || n == "deepslate_redstone_ore" || n == "budding_amethyst") return true;
    const auto copper = copper_stage(n);
    return copper && copper->first < 3;
}

void weathering_random_tick(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto n = name_of(s);
    if (n == "ice") {
        const int dampening = registry::light_dampening(s.registry_state());
        if (ctx.block_light(pos) > 11 - dampening) {
            set_block(ctx, pos, ctx.dimension() == contracts::DimensionId::the_nether ? blocks::air() : blocks::water_source());
        }
        return;
    }
    if (n == "snow") {
        if (ctx.block_light(pos) > 11) destroy_block(ctx, pos);
        return;
    }
    if (n == "redstone_ore" || n == "deepslate_redstone_ore") {
        if (value_of(s, "lit") == "true") set_block(ctx, pos, with(s, "lit", "false"));
        return;
    }
    if (n == "budding_amethyst") {
        grow_amethyst(ctx, pos);
        return;
    }
    if (const auto copper = copper_stage(n); copper && copper->first < 3) weather_copper(ctx, pos, s, copper->first, copper->second);
}

} // namespace circuit::gameplay::detail

namespace circuit::gameplay {
namespace {
// The blocks lava sets fire beside (both official servers' report for every
// state, tests/fixtures/collision/lava-ignites-*.txt): the overworld's and
// bamboo's wooden blocks but buttons, logs, wood and their stripped kinds,
// leaves, wool, carpets, beds and banners, bookshelves, the wooden work and
// storage blocks, TNT, mushroom blocks, grasses, ferns, bushes, tall
// flowers, vines and some other plants.
bool ignites(std::string_view n) {
    static constexpr std::array<std::string_view, 10> woods{"oak", "spruce", "birch", "jungle", "acacia", "dark_oak", "mangrove", "cherry",
                                                           "pale_oak", "bamboo"};
    static constexpr std::array<std::string_view, 15> wooden{"_door", "_fence", "_fence_gate", "_hanging_sign", "_wall_hanging_sign", "_sign",
                                                            "_wall_sign", "_leaves", "_log", "_wood", "_planks", "_pressure_plate", "_slab",
                                                            "_stairs", "_trapdoor"};
    auto bare = n;
    if (bare.starts_with("stripped_")) bare.remove_prefix(9);
    for (const auto w : woods)
        if (bare.starts_with(w) && bare.size() > w.size() && bare[w.size()] == '_') {
            const auto rest = bare.substr(w.size());
            if (std::ranges::find(wooden, rest) != wooden.end()) return w != "bamboo" || (rest != "_log" && rest != "_wood" && rest != "_leaves");
        }
    if (n.ends_with("_wool") || n.ends_with("_carpet") || n.ends_with("_bed") || n.ends_with("_banner")) return n != "moss_carpet";
    static constexpr std::array<std::string_view, 49> others{
        "azalea_leaves", "flowering_azalea_leaves", "bamboo", "bamboo_block", "stripped_bamboo_block", "bamboo_mosaic", "bamboo_mosaic_slab",
        "bamboo_mosaic_stairs", "bamboo_sapling", "barrel", "bee_nest", "beehive", "bookshelf", "chiseled_bookshelf", "brown_mushroom_block",
        "red_mushroom_block", "mushroom_stem", "bush", "cactus_flower", "campfire", "soul_campfire", "cartography_table", "chest",
        "trapped_chest", "composter", "crafting_table", "daylight_detector", "dead_bush", "fern", "large_fern", "firefly_bush",
        "fletching_table", "glow_lichen", "hanging_roots", "jukebox", "lectern", "lilac", "peony", "rose_bush", "sunflower", "loom",
        "mangrove_roots", "note_block", "pale_hanging_moss", "pale_moss_block", "pale_moss_carpet", "pitcher_plant", "resin_clump",
        "smithing_table"};
    static constexpr std::array<std::string_view, 6> more{"short_dry_grass", "tall_dry_grass", "short_grass", "tall_grass", "tnt", "vine"};
    return std::ranges::find(others, n) != others.end() || std::ranges::find(more, n) != more.end();
}
} // namespace

bool ignited_by_lava(BlockState s) {
    static const std::vector<bool> table = [] {
        std::vector<bool> v(registry::state_count() + 1, false);
        for (std::uint32_t st = 1; st <= registry::state_count(); ++st) v[st] = ignites(registry::game_name(*registry::block_of(st)));
        return v;
    }();
    const auto state = physics::registry_state_of(s);
    return state && table[*state];
}
} // namespace circuit::gameplay
