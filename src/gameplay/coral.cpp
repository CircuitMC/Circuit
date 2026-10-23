// SPDX-License-Identifier: AGPL-3.0-only
// Coral fans and wall fans, as both official servers run them (checked
// against their traces; minecraft.wiki "Coral Fan").
//
// A living fan without water — not waterlogged, and no water in a neighbor
// cell (water, or a waterlogged block) — schedules its death 60 to 99 ticks
// later (60 plus a level-random draw below 40, drawn also when a death is
// already scheduled) when it is placed and on every shape update; the tick
// turns it into the dead fan of its kind, dry, quietly, unless water came
// back. A fan needs a sturdy top below it, a wall fan a sturdy face behind
// it; a shape update from there that finds none destroys it, leaving its
// water. Water does not flow out of waterlogged fans (waterlogging is
// stored only).
#include "internal.hpp"
#include <string_view>

namespace circuit::gameplay::detail {
namespace {
bool has_water(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (s.flag()) return true;
    for (const auto d : contracts::all_directions)
        if (holds_water_fluid(ctx.block(pos.relative(d)))) return true;
    return false;
}

void schedule_death(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (dead_coral(s) || has_water(ctx, pos, s)) return;
    const auto delay = 60 + ctx.random_int(40);
    schedule(ctx, TickList::block, pos, s.type(), static_cast<std::uint32_t>(delay), 0);
}

Direction support_side(BlockState s) { return s.is(BlockType::coral_fan) ? Direction::down : contracts::opposite(s.facing()); }
} // namespace

bool coral_supported(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto side = support_side(s);
    return face_sturdy(ctx.block(pos.relative(side)), contracts::opposite(side));
}

void coral_placed(RuleContext& ctx, BlockPos pos, BlockState s) { schedule_death(ctx, pos, s); }

void coral_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d) {
    if (d == support_side(s) && !coral_supported(ctx, pos, s)) {
        destroy_block(ctx, pos);
        return;
    }
    schedule_death(ctx, pos, s);
}

void tick_coral(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (dead_coral(s) || has_water(ctx, pos, s)) return;
    set_block_quiet(ctx, pos, s.with_variant(static_cast<std::uint8_t>(s.variant() + blocks::coral_kinds)).with_flag(false));
}

bool waterlogged(BlockState s) {
    switch (s.type()) {
    case BlockType::coral_fan: case BlockType::coral_wall_fan: case BlockType::chest: return s.flag();
    case BlockType::trapdoor: return (s.level() & 2) != 0;
    case BlockType::rail: return s.extra() != 0;
    case BlockType::other:
        for (const auto& [key, value] : registry::properties(s.registry_state()))
            if (key == "waterlogged") return value == "true";
        return false;
    default: return false;
    }
}

bool holds_water_fluid(BlockState s) {
    if (s.is(BlockType::water)) return true;
    if (s.is(BlockType::other)) {
        const auto block = registry::block_of(s.registry_state());
        const auto name = block ? registry::block(*block).name : std::string_view{};
        if (name == "kelp" || name == "kelp_plant" || name == "seagrass" || name == "tall_seagrass" || name == "bubble_column") return true;
    }
    return waterlogged(s);
}

} // namespace circuit::gameplay::detail
