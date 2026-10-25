// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Crops and stems growing in their random ticks (both official servers,
// 1.21.8; crops_growth, stems_growth; minecraft.wiki "Tutorial:Crop farming",
// "Beetroot Seeds", "Melon Seeds"):
// - a crop or stem grows only with light 9 or more at its own cell, the most
//   of its block light and its sky light whatever the time of day;
// - its growth speed f is 1, plus 3 for moist farmland under it (1 for dry),
//   plus a quarter of that for each farmland block around that one; halved
//   when the same block grows both beside it along x and along z, or else on
//   a diagonal; it grows when nextInt((int)(25 / f) + 1) of the level random
//   is 0 (computed in single precision);
// - beetroots and torchflower crops first draw nextInt(3) and skip the tick
//   on 0; a torchflower crop past age 1 becomes a torchflower;
// - a ripe stem picks a side instead (nextInt(4): north, east, south, west):
//   a fruit is set there when it is air over farmland or a dirt block, and the
//   stem becomes the attached stem facing it; an attached stem whose fruit is
//   gone becomes the ripe stem again.
// Ages change without neighbor updates; fruit and stems with them.
namespace circuit::gameplay::detail {
namespace {
std::optional<std::uint32_t> block_id(BlockState s) {
    const auto state = physics::registry_state_of(s);
    return state ? registry::block_of(*state) : std::nullopt;
}

std::string_view name_of(BlockState s) {
    const auto block = block_id(s);
    return block ? registry::game_name(*block) : std::string_view{};
}

int property(BlockState s, std::string_view key) {
    const auto state = physics::registry_state_of(s);
    if (!state) return 0;
    for (const auto& [k, v] : registry::properties(*state))
        if (k == key) return std::stoi(v);
    return 0;
}

BlockState named(RuleContext& ctx, std::string_view name, const std::vector<registry::Property>& props = {}) {
    const auto block = registry::find_block(ctx.bundle().schema.content_release, name);
    if (props.empty()) return BlockState::registered(registry::block(*block).default_state);
    return BlockState::registered(registry::state_of(*block, props).value());
}

BlockState with_age(BlockState s, int age) {
    if (s.is(BlockType::wheat)) return s.with_level(static_cast<std::uint8_t>(age));
    const std::vector<registry::Property> props{{"age", std::to_string(age)}};
    return BlockState::registered(registry::state_of(*block_id(s), props).value());
}

int age_of(BlockState s) { return s.is(BlockType::wheat) ? s.level() : property(s, "age"); }

float growth_speed(RuleContext& ctx, BlockPos pos, BlockState s) {
    float f = 1.0F;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz) {
            const auto ground = ctx.block({pos.x + dx, pos.y - 1, pos.z + dz});
            float g = 0.0F;
            if (is_farmland(ground)) g = property(ground, "moisture") > 0 ? 3.0F : 1.0F;
            if (dx != 0 || dz != 0) g /= 4.0F;
            f += g;
        }
    const auto self = block_id(s);
    const auto same = [&](int dx, int dz) { return block_id(ctx.block({pos.x + dx, pos.y, pos.z + dz})) == self; };
    const bool along_x = same(-1, 0) || same(1, 0);
    const bool along_z = same(0, -1) || same(0, 1);
    if (along_x && along_z) f /= 2.0F;
    else if (same(-1, -1) || same(1, -1) || same(1, 1) || same(-1, 1)) f /= 2.0F;
    return f;
}

bool grows(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto f = growth_speed(ctx, pos, s);
    return ctx.random_int(static_cast<std::int32_t>(25.0F / f) + 1) == 0;
}

std::string_view side_name(Direction d) {
    switch (d) {
    case Direction::north: return "north";
    case Direction::south: return "south";
    case Direction::west: return "west";
    case Direction::east: return "east";
    default: return "";
    }
}

bool fruit_ground(BlockState s) {
    static constexpr std::array<std::string_view, 11> grounds{"farmland", "dirt", "coarse_dirt", "rooted_dirt", "grass_block", "podzol",
                                                             "mycelium", "moss_block", "pale_moss_block", "mud", "muddy_mangrove_roots"};
    return std::ranges::find(grounds, name_of(s)) != grounds.end();
}
} // namespace

bool is_crop(BlockState s) {
    if (s.is(BlockType::wheat)) return true;
    if (!s.is(BlockType::other)) return false;
    const auto n = name_of(s);
    return n == "carrots" || n == "potatoes" || n == "beetroots" || n == "torchflower_crop";
}

void crop_random_tick(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto n = name_of(s);
    const bool slow = n == "beetroots" || n == "torchflower_crop";
    if (slow && ctx.random_int(3) == 0) return;
    if (ctx.light(pos) < 9) return;
    const int max = n == "beetroots" ? 3 : n == "torchflower_crop" ? 2 : 7;
    const int age = age_of(s);
    if (age >= max || !grows(ctx, pos, s)) return;
    if (n == "torchflower_crop" && age + 1 >= max) set_block_quiet(ctx, pos, named(ctx, "torchflower"));
    else set_block_quiet(ctx, pos, with_age(s, age + 1));
}

bool is_stem(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    const auto n = name_of(s);
    return n == "melon_stem" || n == "pumpkin_stem";
}

void stem_random_tick(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (ctx.light(pos) < 9 || !grows(ctx, pos, s)) return;
    const int age = property(s, "age");
    if (age < 7) {
        set_block_quiet(ctx, pos, with_age(s, age + 1));
        return;
    }
    static constexpr std::array<Direction, 4> sides{Direction::north, Direction::east, Direction::south, Direction::west};
    const auto side = sides[static_cast<std::size_t>(ctx.random_int(4))];
    const auto at = pos.relative(side);
    if (!ctx.block(at).is_air() || !fruit_ground(ctx.block(at.relative(Direction::down)))) return;
    const bool melon = name_of(s) == "melon_stem";
    set_block(ctx, at, named(ctx, melon ? "melon" : "pumpkin"));
    const std::vector<registry::Property> facing{{"facing", std::string(side_name(side))}};
    set_block(ctx, pos, named(ctx, melon ? "attached_melon_stem" : "attached_pumpkin_stem", facing));
}

bool is_attached_stem(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    const auto n = name_of(s);
    return n == "attached_melon_stem" || n == "attached_pumpkin_stem";
}

bool attached_stem_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d) {
    const auto state = physics::registry_state_of(s);
    std::string facing;
    for (const auto& [k, v] : registry::properties(*state))
        if (k == "facing") facing = v;
    if (facing != side_name(d)) return false;
    const bool melon = name_of(s) == "attached_melon_stem";
    if (name_of(ctx.block(pos.relative(d))) == (melon ? "melon" : "pumpkin")) return false;
    const std::vector<registry::Property> ripe{{"age", "7"}};
    set_block_quiet(ctx, pos, named(ctx, melon ? "melon_stem" : "pumpkin_stem", ripe));
    return true;
}

} // namespace circuit::gameplay::detail
