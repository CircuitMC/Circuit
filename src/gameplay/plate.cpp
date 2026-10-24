// SPDX-License-Identifier: AGPL-3.0-only
// Pressure plates and target blocks, as both official servers run them
// (checked against their traces; minecraft.wiki "Pressure Plate", "Target").
//
// A plate senses the entities whose box meets the lower quarter of its
// cell, a pixel in from each side. Wooden plates sense every entity, stone
// and polished blackstone plates only mobs and players (none of which are
// rule entities yet); a light weighted plate gives one level per entity (at
// most 15), a heavy one one level per ten. An unpowered plate checks when
// an entity's move ends with its box in the plate's cell; a powered one
// checks again 20 ticks (weighted: 10) after each check. A check that
// changes the output sets the plate quietly and updates around it and
// around the block below it, which it powers strongly.
//
// A target block hit by a projectile gives a signal by how near the center
// of the hit face the hit was, for 20 ticks after an arrow and 8 after
// another projectile; hits while it waits are ignored.
#include "internal.hpp"
#include <cmath>

namespace circuit::gameplay::detail {
namespace {
using physics::Aabb;

constexpr double px = 1.0 / 16;

bool senses(BlockState plate, const Entity&) {
    // Stone plates need living entities; the rules have none yet.
    return plate.is(BlockType::weighted_plate) || plate.variant() < static_cast<std::uint8_t>(blocks::ButtonKind::stone);
}

std::uint32_t pressed_ticks(BlockState s) { return s.is(BlockType::weighted_plate) ? 10 : 20; }

std::uint8_t output(BlockState s) {
    if (s.is(BlockType::weighted_plate)) return s.level();
    return s.flag() ? 15 : 0;
}

BlockState with_output(BlockState s, std::uint8_t signal) {
    return s.is(BlockType::weighted_plate) ? s.with_level(signal) : s.with_flag(signal > 0);
}

std::uint8_t sensed_signal(RuleContext& ctx, BlockPos pos, BlockState s) {
    const Aabb area = Aabb{px, 0.0, px, 1.0 - px, 0.25, 1.0 - px}.move(pos.x, pos.y, pos.z);
    std::size_t count = 0;
    for (const auto id : entities_intersecting(ctx, area))
        if (const auto* e = ctx.peek_entity(id); e && senses(s, *e)) ++count;
    if (!s.is(BlockType::weighted_plate)) return count > 0 ? 15 : 0;
    if (s.variant() == 0) return static_cast<std::uint8_t>(std::min<std::size_t>(count, 15));
    return static_cast<std::uint8_t>((std::min<std::size_t>(count, 150) + 9) / 10);
}

void plate_updates(RuleContext& ctx, BlockPos pos, BlockState s) {
    notify_neighbors(ctx, pos, std::nullopt, s);
    notify_neighbors(ctx, pos.relative(Direction::down), std::nullopt, s);
}

void check_plate(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto signal = sensed_signal(ctx, pos, s);
    if (signal != output(s)) {
        set_block_quiet(ctx, pos, with_output(s, signal));
        plate_updates(ctx, pos, s);
    }
    if (signal > 0) schedule(ctx, TickList::block, pos, s.type(), pressed_ticks(s), 0);
}
} // namespace

bool is_plate(BlockType t) noexcept { return t == BlockType::pressure_plate || t == BlockType::weighted_plate; }

std::uint8_t plate_output(BlockState s) noexcept { return is_plate(s.type()) ? output(s) : 0; }

void tick_plate(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (output(s) > 0) check_plate(ctx, pos, s);
}

void plate_removed(RuleContext& ctx, BlockPos pos, BlockState previous) {
    if (output(previous) > 0) plate_updates(ctx, pos, previous);
}

void entity_inside(RuleContext& ctx, const Entity& entity) {
    const auto box = entity_box(entity);
    const auto cell = [](double v) { return static_cast<std::int32_t>(std::floor(v)); };
    for (auto z = cell(box.min_z); z <= cell(box.max_z); ++z)
        for (auto y = cell(box.min_y); y <= cell(box.max_y); ++y)
            for (auto x = cell(box.min_x); x <= cell(box.max_x); ++x) {
                const BlockPos pos{x, y, z};
                const auto s = ctx.block(pos);
                if (is_plate(s.type()) && output(s) == 0) check_plate(ctx, pos, s);
                else if (s.is(BlockType::button)) entity_at_button(ctx, pos, s);
                else if (s.is(BlockType::rail)) detector_entity_inside(ctx, pos, s);
                else if (is_big_dripleaf(s)) dripleaf_entity_inside(ctx, pos, s, entity);
            }
}

void target_hit(RuleContext& ctx, BlockPos pos, Vec3 at, int axis, bool arrow) {
    const auto s = ctx.block(pos);
    if (!s.is(BlockType::target) || ctx.has_tick(TickList::block, pos, BlockType::target)) return;
    const auto off = [](double v) { return std::abs(v - std::floor(v) - 0.5); };
    const double dx = off(at.x), dy = off(at.y), dz = off(at.z);
    const double e = axis == 1 ? std::max(dx, dz) : axis == 2 ? std::max(dx, dy) : std::max(dy, dz);
    const auto signal = static_cast<std::uint8_t>(std::max(1.0, std::ceil(15.0 * std::clamp((0.5 - e) / 0.5, 0.0, 1.0))));
    set_block(ctx, pos, s.with_level(signal));
    schedule(ctx, TickList::block, pos, BlockType::target, arrow ? 20 : 8, 0);
}

void tick_target(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (s.level() != 0) set_block(ctx, pos, s.with_level(0));
}

} // namespace circuit::gameplay::detail
