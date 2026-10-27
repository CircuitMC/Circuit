// SPDX-License-Identifier: AGPL-3.0-only
// Shulker boxes as both official servers run them (shulker_boxes;
// client/shulker; minecraft.wiki "Shulker Box"):
// - 27 slots that take everything but shulker boxes, from players' windows
//   and from hoppers, droppers and crafters; hoppers below take from them,
//   comparators read them.
// - A box opens only when its lid can rise: nothing collides with the half
//   block beyond its front (a stone or a hopper there keeps it shut). Its
//   viewers are counted, each change queuing its lid's block event with the
//   count; nothing rechecks them.
// - Broken, by a player, a piston or an explosion, a box drops one item of
//   itself that keeps its contents (the container component) and its name;
//   placed, such an item gives them back.
#include "internal.hpp"
#include <circuit/gameplay/game_data.hpp>
#include <algorithm>
#include <array>

namespace circuit::gameplay::detail {
namespace {
const auto& names = blocks::shulker_box_names;

const std::array<std::uint16_t, 17>& box_items() {
    static const auto ids = [] {
        std::array<std::uint16_t, 17> out{};
        for (std::size_t k = 0; k < names.size(); ++k) out[k] = items::by_name(std::string("minecraft:") + std::string(names[k])).value_or(0);
        return out;
    }();
    return ids;
}
} // namespace

std::optional<std::uint8_t> shulker_box_color(std::uint16_t item) {
    const auto& ids = box_items();
    const auto it = std::ranges::find(ids, item);
    if (item == 0 || it == ids.end()) return std::nullopt;
    return static_cast<std::uint8_t>(it - ids.begin());
}

bool is_shulker_box_item(std::uint16_t item) { return shulker_box_color(item).has_value(); }

ItemStack shulker_box_item(RuleContext& ctx, BlockPos pos, BlockState s) {
    ItemStack box{box_items()[s.variant() % names.size()], 1};
    const auto* be = ctx.peek_block_entity(pos);
    const auto* data = ctx.data();
    if (!be || !data || !data->with_container) return box;
    // Its name and other components, and its contents.
    if (const auto patch = data->with_container(be->components, be->slots)) box.components = *patch;
    return box;
}

bool shulker_keeps_contents(RuleContext& ctx) {
    const auto* data = ctx.data();
    return data && data->with_container;
}

void fill_shulker_box(RuleContext& ctx, BlockPos pos, const ItemStack& item) {
    auto* be = ctx.block_entity(pos);
    const auto* data = ctx.data();
    if (!be || item.components.empty() || !data || !data->container_of || !data->with_container) return;
    const auto stacks = data->container_of(item.components);
    for (std::size_t k = 0; k < stacks.size() && k < be->slots.size(); ++k) be->slots[k] = stacks[k];
    // The item's other components (its name) are the box's.
    if (const auto rest = data->with_container(item.components, {})) be->components = *rest;
    container_changed(ctx, pos);
}

bool lid_can_rise(RuleContext& ctx, BlockPos pos, BlockState s) {
    // Viewed, its lid is up already.
    if (const auto* be = ctx.peek_block_entity(pos); be && !be->viewers.empty()) return true;
    // The half block beyond its front, shrunk by a millionth, against the
    // collision boxes of the block there.
    const auto front = s.facing();
    const auto o = contracts::offset(front);
    constexpr double e = 1.0e-6;
    const auto span = [&](int d, std::int32_t at) {
        return d > 0 ? std::pair{at + 1.0 + e, at + 1.5 - e} : d < 0 ? std::pair{at - 0.5 + e, at - e} : std::pair{at + e, at + 1.0 - e};
    };
    const auto [x0, x1] = span(o.dx, pos.x);
    const auto [y0, y1] = span(o.dy, pos.y);
    const auto [z0, z1] = span(o.dz, pos.z);
    const auto there = pos.relative(front);
    const auto state = physics::registry_state_of(ctx.block(there));
    if (!state) return true;
    for (const auto& b : registry::collision_boxes(*state, ctx.bundle().schema.content_release))
        if (there.x + b.min_x < x1 && there.x + b.max_x > x0 && there.y + b.min_y < y1 && there.y + b.max_y > y0 && there.z + b.min_z < z1
            && there.z + b.max_z > z0)
            return false;
    return true;
}

} // namespace circuit::gameplay::detail
