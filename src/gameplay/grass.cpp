// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/light.hpp>
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Grass blocks and mycelium in their random ticks (both official servers,
// 1.21.8; grass_spread; minecraft.wiki "Grass Block", "Mycelium"):
// - one whose top light cannot pass into the block above (that block's
//   faces cover it, or it dampens all light), or that has a full block of
//   water above, becomes dirt; a single snow layer does not count;
// - otherwise, with light 9 or more above it (block light, or sky light less
//   the sky's darkening), it tries four positions: each draws x - 1 + nextInt(3),
//   y - 3 + nextInt(5), z - 1 + nextInt(3) from the level random, and dirt
//   there becomes the same block when it could stay one and has no water
//   above, snowy when snow is on it.
namespace circuit::gameplay::detail {
namespace {
std::string_view name_of(BlockState s) {
    const auto state = physics::registry_state_of(s);
    const auto block = state ? registry::block_of(*state) : std::nullopt;
    return block ? registry::game_name(*block) : std::string_view{};
}

std::string value_of(BlockState s, std::string_view key) {
    const auto state = physics::registry_state_of(s);
    if (!state) return {};
    for (const auto& [k, v] : registry::properties(*state))
        if (k == key) return v;
    return {};
}

BlockState named(RuleContext& ctx, std::string_view name, const std::vector<registry::Property>& props = {}) {
    const auto block = registry::find_block(ctx.bundle().schema.content_release, name);
    if (props.empty()) return BlockState::registered(registry::block(*block).default_state);
    return BlockState::registered(registry::state_of(*block, props).value());
}

// A source, falling water or a waterlogged block: a full block of water.
bool full_water(BlockState s) { return (s.is(BlockType::water) && (s.flag() || s.level() == 0)) || waterlogged(s); }

// Whether a grass block (or mycelium) could stay one at `pos`.
bool can_stay(RuleContext& ctx, BlockPos pos, BlockState self) {
    const auto above = ctx.block(pos.relative(Direction::up));
    if (name_of(above) == "snow" && value_of(above, "layers") == "1") return true;
    if (full_water(above)) return false;
    return light_passed(LightKind::sky, 15, light_info(self), light_info(above), Direction::up) > 0;
}
} // namespace

bool is_spreading_dirt(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    const auto n = name_of(s);
    return n == "grass_block" || n == "mycelium";
}

void spreading_dirt_random_tick(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (!can_stay(ctx, pos, s)) {
        set_block(ctx, pos, named(ctx, "dirt"));
        return;
    }
    const auto up = pos.relative(Direction::up);
    const int sky = static_cast<int>(ctx.sky_light(up)) - static_cast<int>(ctx.sky().darken);
    if (std::max(static_cast<int>(ctx.block_light(up)), sky) < 9) return;
    const auto block = named(ctx, name_of(s));
    for (int i = 0; i < 4; ++i) {
        const int dx = ctx.random_int(3) - 1;
        const int dy = ctx.random_int(5) - 3;
        const int dz = ctx.random_int(3) - 1;
        const BlockPos at{pos.x + dx, pos.y + dy, pos.z + dz};
        if (!ctx.world().contains_y(at.y) || name_of(ctx.block(at)) != "dirt") continue;
        const auto above = ctx.block(at.relative(Direction::up));
        if (!can_stay(ctx, at, block) || holds_water_fluid(above)) continue;
        const auto a = name_of(above);
        const bool snowy = a == "snow" || a == "snow_block" || a == "powder_snow";
        const std::vector<registry::Property> props{{"snowy", snowy ? "true" : "false"}};
        set_block(ctx, at, named(ctx, name_of(s), props));
    }
}

} // namespace circuit::gameplay::detail
