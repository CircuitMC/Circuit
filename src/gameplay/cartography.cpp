// SPDX-License-Identifier: AGPL-3.0-only
// Empty maps as both official servers fill them (client/maps, both versions
// identical; minecraft.wiki "Map"), and the new maps of marked results. The
// cartography table's window is in menus.cpp, the areas and markers in
// maps.cpp.
#include "internal.hpp"
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/items.hpp>

namespace circuit::gameplay::detail {

contracts::Result<std::function<void()>> plan_use_empty_map(RuleContext& ctx, const contracts::PlayerUseItem& c) {
    const auto* data = ctx.data();
    if (!data || !data->with_int_component) return std::unexpected(contracts::ErrorCode::not_usable);
    return [&ctx, c, data] {
        // A new map of scale 0 over the area holding the player's feet, in
        // the player's level.
        MapData map;
        map.x_center = map_center(c.eye.x, 0);
        map.z_center = map_center(c.eye.z, 0);
        map.dimension = ctx.dimension();
        const auto id = ctx.add_map(std::move(map));
        ItemStack filled{items::filled_map, 1, 0};
        if (const auto components = data->with_int_component({}, "map_id", id)) filled.components = *components;
        // In survival the last empty map becomes the filled one; else one is
        // used up and the filled map goes into the inventory (dropped
        // without room), as it does in creative mode, which keeps the map.
        auto& inv = ctx.player(c.player);
        auto& slot = inv.slots[c.slot];
        if (c.survival && slot.count == 1) {
            slot = filled;
            record_slot(ctx, inv, c.slot);
            return;
        }
        if (c.survival) {
            --slot.count;
            record_slot(ctx, inv, c.slot);
        }
        if (const auto rest = add_to_player(ctx, inv, filled, c.slot); !rest.empty()) {
            const Vec3 at{c.eye.x, c.eye.y - 0.3, c.eye.z};
            spawn_dropped(ctx, contracts::block_of(at), at, {}, rest, 40);
        }
    };
}

ItemStack taken_marked_map(RuleContext& ctx, const ItemStack& marked) {
    const auto* data = ctx.data();
    if (!data || !data->int_component || !data->with_int_component || !data->int_component(marked.components, "map_post_processing"))
        return marked;
    auto out = marked;
    auto components = data->with_int_component(marked.components, "map_post_processing", std::nullopt);
    if (components) components = data->with_int_component(*components, "map_id", ctx.last_map_id() + 1);
    if (components) out.components = *components;
    return out;
}

void add_marked_map(RuleContext& ctx, const ItemStack& marked) {
    const auto* data = ctx.data();
    if (!data || !data->int_component) return;
    const auto how = data->int_component(marked.components, "map_post_processing");
    const auto id = data->int_component(marked.components, "map_id");
    if (const auto* map = how && id ? ctx.peek_map(*id) : nullptr) ctx.add_map(*how == map_zooms ? zoomed_map(*map) : locked_map(*map));
}

} // namespace circuit::gameplay::detail
