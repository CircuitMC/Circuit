// SPDX-License-Identifier: AGPL-3.0-only
// Composters as both official servers run them (composters_items,
// composter_use, composter_break, composter_inputs; client/composter;
// minecraft.wiki "Composter"):
// - An item composts by its chance (the wiki's table: 30, 50, 65, 85 or 100
//   percent), drawn as a double from the level random; the first item into an
//   empty composter always adds a layer and draws nothing. A composted item
//   is used up whether or not it adds a layer.
// - Items go in only below level 7: from a player, and through the top from
//   a hopper, dropper or crafter above (one item at a time). Level 7 turns
//   into 8, bone meal ready, at a block tick 20 ticks later.
// - At level 8 a player's use pops the bone meal out on top, and a hopper
//   below takes it; either empties the composter. Broken at level 8, it drops
//   the bone meal after itself.
// - Comparators read its level.
#include "internal.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace circuit::gameplay::detail {
namespace {
constexpr std::uint8_t full = 7, ready = 8;
// A bone meal a player takes out waits as long as a block drop.
constexpr std::uint16_t pickup_delay = 10;

// minecraft.wiki "Composter", the items of the target versions (each of the
// 115 composts on both official servers, composters_items; items of other
// versions are skipped).
struct Compostable {
    std::string_view item;
    float chance;
};
constexpr std::array<Compostable, 115> compostables{{
    {"beetroot_seeds", 0.3f}, {"bush", 0.3f}, {"cactus_flower", 0.3f}, {"dried_kelp", 0.3f}, {"firefly_bush", 0.3f},
    {"glow_berries", 0.3f}, {"hanging_roots", 0.3f}, {"kelp", 0.3f}, {"oak_leaves", 0.3f}, {"spruce_leaves", 0.3f},
    {"birch_leaves", 0.3f}, {"jungle_leaves", 0.3f}, {"acacia_leaves", 0.3f}, {"cherry_leaves", 0.3f}, {"dark_oak_leaves", 0.3f},
    {"pale_oak_leaves", 0.3f}, {"mangrove_leaves", 0.3f}, {"azalea_leaves", 0.3f}, {"leaf_litter", 0.3f},
    {"mangrove_propagule", 0.3f}, {"mangrove_roots", 0.3f}, {"melon_seeds", 0.3f}, {"moss_carpet", 0.3f},
    {"pale_hanging_moss", 0.3f}, {"pale_moss_carpet", 0.3f}, {"pink_petals", 0.3f}, {"pitcher_pod", 0.3f},
    {"pumpkin_seeds", 0.3f}, {"oak_sapling", 0.3f}, {"spruce_sapling", 0.3f}, {"birch_sapling", 0.3f}, {"jungle_sapling", 0.3f},
    {"acacia_sapling", 0.3f}, {"cherry_sapling", 0.3f}, {"dark_oak_sapling", 0.3f}, {"pale_oak_sapling", 0.3f},
    {"seagrass", 0.3f}, {"short_grass", 0.3f}, {"short_dry_grass", 0.3f}, {"small_dripleaf", 0.3f}, {"sweet_berries", 0.3f},
    {"tall_dry_grass", 0.3f}, {"torchflower_seeds", 0.3f}, {"wheat_seeds", 0.3f}, {"wildflowers", 0.3f},
    {"cactus", 0.5f}, {"dried_kelp_block", 0.5f}, {"flowering_azalea_leaves", 0.5f}, {"glow_lichen", 0.5f}, {"melon_slice", 0.5f},
    {"nether_sprouts", 0.5f}, {"sugar_cane", 0.5f}, {"tall_grass", 0.5f}, {"twisting_vines", 0.5f}, {"vine", 0.5f},
    {"weeping_vines", 0.5f},
    {"apple", 0.65f}, {"azalea", 0.65f}, {"beetroot", 0.65f}, {"big_dripleaf", 0.65f}, {"brown_mushroom", 0.65f},
    {"carrot", 0.65f}, {"carved_pumpkin", 0.65f}, {"cocoa_beans", 0.65f}, {"crimson_roots", 0.65f}, {"fern", 0.65f},
    {"dandelion", 0.65f}, {"poppy", 0.65f}, {"blue_orchid", 0.65f}, {"allium", 0.65f}, {"azure_bluet", 0.65f},
    {"red_tulip", 0.65f}, {"orange_tulip", 0.65f}, {"white_tulip", 0.65f}, {"pink_tulip", 0.65f}, {"oxeye_daisy", 0.65f},
    {"cornflower", 0.65f}, {"lily_of_the_valley", 0.65f}, {"sunflower", 0.65f}, {"lilac", 0.65f}, {"rose_bush", 0.65f},
    {"peony", 0.65f}, {"open_eyeblossom", 0.65f}, {"closed_eyeblossom", 0.65f}, {"large_fern", 0.65f}, {"lily_pad", 0.65f},
    {"melon", 0.65f}, {"moss_block", 0.65f}, {"mushroom_stem", 0.65f}, {"crimson_fungus", 0.65f}, {"warped_fungus", 0.65f},
    {"nether_wart", 0.65f}, {"pale_moss_block", 0.65f}, {"potato", 0.65f}, {"pumpkin", 0.65f}, {"red_mushroom", 0.65f},
    {"sea_pickle", 0.65f}, {"shroomlight", 0.65f}, {"spore_blossom", 0.65f}, {"warped_roots", 0.65f}, {"wheat", 0.65f},
    {"wither_rose", 0.65f},
    {"baked_potato", 0.85f}, {"bread", 0.85f}, {"cookie", 0.85f}, {"flowering_azalea", 0.85f}, {"hay_block", 0.85f},
    {"brown_mushroom_block", 0.85f}, {"red_mushroom_block", 0.85f}, {"nether_wart_block", 0.85f}, {"pitcher_plant", 0.85f},
    {"torchflower", 0.85f}, {"warped_wart_block", 0.85f},
    {"cake", 1.0f}, {"pumpkin_pie", 1.0f},
}};

// A composting attempt's level event: 1 when it added a layer.
constexpr std::uint64_t fill_event = 1500;
} // namespace

float compost_chance(const ItemStack& stack) {
    static const auto table = [] {
        std::unordered_map<std::uint16_t, float> out;
        for (const auto& c : compostables)
            if (const auto id = items::by_name(std::string("minecraft:") + std::string(c.item))) out.emplace(*id, c.chance);
        return out;
    }();
    if (stack.empty()) return 0.0f;
    const auto it = table.find(stack.item);
    return it == table.end() ? 0.0f : it->second;
}

bool composter_takes(BlockState s, const ItemStack& stack) {
    return s.is(BlockType::composter) && s.level() < full && compost_chance(stack) > 0.0f;
}

void compost(RuleContext& ctx, BlockPos pos, const ItemStack& stack) {
    const auto s = ctx.block(pos);
    if (!composter_takes(s, stack)) return;
    const auto level = s.level();
    // The chance as a single-precision number, compared in double precision.
    const bool layer = level == 0 || ctx.random_double() < static_cast<double>(compost_chance(stack));
    // Placed full, it schedules its ready tick (updates.cpp).
    if (layer) set_block(ctx, pos, s.with_level(static_cast<std::uint8_t>(level + 1)));
    ctx.record(TraceKind::level_event, pos, fill_event, layer ? 1 : 0);
}

void tick_composter(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (s.level() == full) set_block(ctx, pos, s.with_level(ready));
}

bool composter_ready(BlockState s) { return s.is(BlockType::composter) && s.level() == ready; }

bool compost_through(RuleContext& ctx, BlockPos pos, Direction face, const ItemStack& stack) {
    if (face != Direction::up || !composter_takes(ctx.block(pos), stack)) return false;
    compost(ctx, pos, stack);
    return true;
}

void empty_composter(RuleContext& ctx, BlockPos pos) {
    const auto s = ctx.block(pos);
    if (s.is(BlockType::composter)) set_block(ctx, pos, s.with_level(0));
}

void pop_bone_meal(RuleContext& ctx, BlockPos pos) {
    // Above the composter's middle, 1.01 up, spread up to 0.35 on each axis
    // (the spread in single precision), then the velocity every new item
    // draws (composter_use: every bit matched).
    const auto spread = [&] { return static_cast<double>((ctx.random_float() - 0.5f) * 0.7f); };
    const double x = pos.x + 0.5 + spread();
    const double y = pos.y + 1.01 + spread();
    const double z = pos.z + 0.5 + spread();
    const double vx = ctx.random_double() * 0.2 - 0.1;
    const double vz = ctx.random_double() * 0.2 - 0.1;
    spawn_dropped(ctx, contracts::block_of({x, y, z}), {x, y, z}, {vx, 0.2, vz}, ItemStack{items::bone_meal, 1}, pickup_delay);
    empty_composter(ctx, pos);
}

} // namespace circuit::gameplay::detail
