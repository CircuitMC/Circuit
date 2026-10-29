// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <string>
#include <string_view>
#include <vector>

// Shears on blocks (both official servers; shears_use, shears_break,
// dispenser_shears, client/shears_items):
// - used on a pumpkin, it becomes a carved pumpkin facing the face used, or,
//   used from above or below, facing the player; four pumpkin seeds come out
//   0.65 in front of that face's center and 0.1 above the block's bottom,
//   drawing a new item's velocity from the level random and then 0.05 out of
//   that face plus nextDouble() * 0.02 on each horizontal axis, 0.05 up;
// - used on a beehive or bee nest with honey level 5, it drops three
//   honeycomb as block drops fall and empties the hive (bees are not
//   modeled); a dispenser with shears does the same to the hive in front;
// - breaking tripwire with shears disarms it first, without updates.
// Shears' dig speeds and the blocks they harvest are in survival.cpp.
namespace circuit::gameplay::detail {
namespace {
std::string_view name_of(BlockState s) {
    if (!s.is(BlockType::other)) return {};
    const auto block = registry::block_of(s.registry_state());
    return block ? registry::game_name(*block) : std::string_view{};
}

std::string value_of(BlockState s, std::string_view key) {
    for (const auto& [k, v] : registry::properties(s.registry_state()))
        if (k == key) return v;
    return {};
}

BlockState with_value(BlockState s, std::string_view key, std::string value) {
    auto props = registry::properties(s.registry_state());
    for (auto& [k, v] : props)
        if (k == key) v = value;
    const auto block = registry::block_of(s.registry_state());
    const auto state = block ? registry::state_of(*block, props) : std::nullopt;
    return state ? BlockState::registered(*state) : s;
}

std::string_view direction_name(Direction d) {
    switch (d) {
    case Direction::north: return "north";
    case Direction::south: return "south";
    case Direction::west: return "west";
    case Direction::east: return "east";
    default: return "north";
    }
}
} // namespace

bool shears_carve(BlockState s) { return name_of(s) == "pumpkin"; }

void carve_pumpkin(RuleContext& ctx, BlockPos pos, Direction face, Direction player_facing) {
    const auto facing = face == Direction::up || face == Direction::down ? contracts::opposite(player_facing) : face;
    const auto release = ctx.bundle().schema.content_release;
    const std::vector<registry::Property> props{{"facing", std::string(direction_name(facing))}};
    const auto carved = registry::state_of(*registry::find_block(release, "carved_pumpkin"), props);
    set_block(ctx, pos, BlockState::registered(carved.value()));
    const auto o = contracts::offset(facing);
    Entity e;
    e.position = {pos.x + 0.5 + o.dx * 0.65, pos.y + 0.1, pos.z + 0.5 + o.dz * 0.65};
    // A new item's own velocity is drawn, then replaced.
    (void)ctx.random_double();
    (void)ctx.random_double();
    const double vx = 0.05 * o.dx + ctx.random_double() * 0.02;
    const double vz = 0.05 * o.dz + ctx.random_double() * 0.02;
    e.velocity = {vx, 0.05, vz};
    e.stack = {*items::by_name("minecraft:pumpkin_seeds"), 4};
    record_spawn(ctx, ctx.spawn_entity(e), e);
}

bool full_hive(BlockState s) {
    const auto n = name_of(s);
    return (n == "beehive" || n == "bee_nest") && value_of(s, "honey_level") == "5";
}

void harvest_hive(RuleContext& ctx, BlockPos pos) {
    pop_resource(ctx, pos, ItemStack{*items::by_name("minecraft:honeycomb"), 3});
    set_block(ctx, pos, with_value(ctx.block(pos), "honey_level", "0"));
}

std::optional<BlockState> disarmed_tripwire(BlockState s) {
    if (name_of(s) != "tripwire" || value_of(s, "disarmed") == "true") return std::nullopt;
    return with_value(s, "disarmed", "true");
}

} // namespace circuit::gameplay::detail
