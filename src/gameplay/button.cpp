// SPDX-License-Identifier: AGPL-3.0-only
// Buttons, as both official servers run them (checked against their traces;
// minecraft.wiki "Button"). A button stands, hangs or sticks on a block as a
// lever does. Used while it is out, it is pressed: it powers its neighbors,
// and the block it is on strongly, updates its neighbors and those of that
// block, and checks again after 20 ticks (stone and polished blackstone) or
// 30 (the wooden ones). Using a pressed button does nothing.
//
// The check releases the button unless an arrow's box meets the button's
// shape, which only wooden buttons heed: an entity that ends its move in
// their cell (an arrow flying or stuck) makes them check, and they stay
// pressed, checking again, while an arrow is there. An arrow flying
// through without ending a tick there does not press them.
//
// The shape is 6 by 4 pixels on its block's face, 2 deep (1 pressed),
// centered; on floors and ceilings its long side runs across its facing.
#include "internal.hpp"

namespace circuit::gameplay::detail {
namespace {
using physics::Aabb;

std::uint32_t press_ticks(BlockState s) { return wooden_button(s) ? 30 : 20; }

Aabb button_shape(BlockPos pos, BlockState s) {
    constexpr double px = 1.0 / 16;
    const double depth = (s.flag() ? 1 : 2) * px;
    const auto facing = s.facing();
    Aabb b{};
    const auto face = static_cast<blocks::LeverFace>(s.level());
    if (face == blocks::LeverFace::wall) {
        b.min_y = 6 * px, b.max_y = 10 * px;
        const auto across = [&](double& lo, double& hi) { lo = 5 * px, hi = 11 * px; };
        switch (facing) {
        case Direction::east: b.min_x = 0, b.max_x = depth, across(b.min_z, b.max_z); break;
        case Direction::west: b.min_x = 1 - depth, b.max_x = 1, across(b.min_z, b.max_z); break;
        case Direction::south: b.min_z = 0, b.max_z = depth, across(b.min_x, b.max_x); break;
        default: b.min_z = 1 - depth, b.max_z = 1, across(b.min_x, b.max_x); break;
        }
    } else {
        if (face == blocks::LeverFace::floor) b.min_y = 0, b.max_y = depth;
        else b.min_y = 1 - depth, b.max_y = 1;
        const bool x_axis = facing == Direction::east || facing == Direction::west;
        b.min_x = (x_axis ? 6 : 5) * px, b.max_x = (x_axis ? 10 : 11) * px;
        b.min_z = (x_axis ? 5 : 6) * px, b.max_z = (x_axis ? 11 : 10) * px;
    }
    return b.move(pos.x, pos.y, pos.z);
}

bool arrow_in(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (!wooden_button(s)) return false;
    for (const auto id : entities_intersecting(ctx, button_shape(pos, s)))
        if (const auto* e = ctx.peek_entity(id); e && e->kind == EntityKind::arrow) return true;
    return false;
}

void button_updates(RuleContext& ctx, BlockPos pos, BlockState s) {
    notify_neighbors(ctx, pos, std::nullopt, s);
    notify_neighbors(ctx, pos.relative(attachment(s)), std::nullopt, s);
}

// Sets the button pressed while an arrow is in it, released otherwise; a
// pressed one checks again later.
void check_pressed(RuleContext& ctx, BlockPos pos, BlockState s) {
    const bool arrow = arrow_in(ctx, pos, s);
    if (arrow != s.flag()) {
        set_block(ctx, pos, s.with_flag(arrow));
        button_updates(ctx, pos, s);
    }
    if (arrow) schedule(ctx, TickList::block, pos, BlockType::button, press_ticks(s), 0);
}
} // namespace

void press_button(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (s.flag()) return;
    set_block(ctx, pos, s.with_flag(true));
    button_updates(ctx, pos, s);
    schedule(ctx, TickList::block, pos, BlockType::button, press_ticks(s), 0);
}

void tick_button(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (s.flag()) check_pressed(ctx, pos, s);
}

void button_removed(RuleContext& ctx, BlockPos pos, BlockState previous) {
    if (previous.flag()) button_updates(ctx, pos, previous);
}

void entity_at_button(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (wooden_button(s) && !s.flag()) check_pressed(ctx, pos, s);
}

} // namespace circuit::gameplay::detail
