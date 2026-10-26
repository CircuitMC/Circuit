// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Leaves (both official servers; leaves_decay; minecraft.wiki "Leaves"):
// - a leaf's distance is one more than the least of its six neighbors', a log
//   or wood block counting 0 and any other block 7, and at most 7;
// - an update from a neighbor schedules its block tick a tick later, unless
//   that neighbor is a log and the leaf is at distance 1 already; the tick
//   sets the distance it has then, with full updates;
// - one at distance 7 and not persistent decays in a random tick, dropping
//   what its loot table rolls.
namespace circuit::gameplay::detail {
namespace {
std::string_view name_of(BlockState s) {
    const auto state = physics::registry_state_of(s);
    const auto block = state ? registry::block_of(*state) : std::nullopt;
    return block ? registry::game_name(*block) : std::string_view{};
}

int property(BlockState s, std::string_view key) {
    const auto state = physics::registry_state_of(s);
    if (!state) return 0;
    for (const auto& [k, v] : registry::properties(*state))
        if (k == key) return v == "true" ? 1 : v == "false" ? 0 : std::stoi(v);
    return 0;
}

// The block tag logs: logs, wood and their stripped kinds, and the Nether's
// stems and hyphae.
bool is_log(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    const auto n = name_of(s);
    if (n.ends_with("_log") || n.ends_with("_wood") || n.ends_with("_hyphae")) return true;
    return n == "crimson_stem" || n == "warped_stem" || n == "stripped_crimson_stem" || n == "stripped_warped_stem";
}

int distance_at(BlockState s) {
    if (is_log(s)) return 0;
    if (is_leaves(s)) return property(s, "distance");
    return 7;
}
} // namespace

bool is_leaves(BlockState s) { return s.is(BlockType::other) && name_of(s).ends_with("_leaves"); }

void leaves_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d) {
    const int distance = distance_at(ctx.block(pos.relative(d))) + 1;
    if (distance != 1 || property(s, "distance") != distance) schedule(ctx, TickList::block, pos, BlockType::other, 1, 0);
}

void tick_leaves(RuleContext& ctx, BlockPos pos, BlockState s) {
    int distance = 7;
    for (const auto d : contracts::all_directions) {
        distance = std::min(distance, distance_at(ctx.block(pos.relative(d))) + 1);
        if (distance == 1) break;
    }
    if (distance == property(s, "distance")) return;
    auto props = registry::properties(s.registry_state());
    for (auto& [k, v] : props)
        if (k == "distance") v = std::to_string(distance);
    set_block(ctx, pos, BlockState::registered(registry::state_of(*registry::block_of(s.registry_state()), props).value()));
}

void leaves_random_tick(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (property(s, "persistent") == 0 && property(s, "distance") == 7) destroy_block(ctx, pos);
}

} // namespace circuit::gameplay::detail
