// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>

// Snow and ice (both official servers; snow_layers, ice_break, frosted_ice;
// tests/fixtures/collision/snow-*.txt; minecraft.wiki "Ice", "Frosted Ice"):
// - grass blocks, podzol and mycelium are snowy while a snow layer, a snow
//   block or powder snow is on them, taking it from an update from above;
// - ice and frosted ice a player breaks become water when the block below
//   blocks movement or is a liquid, unless the tool has an enchantment that
//   keeps ice (silk touch); in the Nether they leave air;
// - frosted ice melts into water when an update from frosted ice (one that
//   melted or was replaced) finds fewer than two frosted ice blocks beside it;
// - every state set for frosted ice draws the delay of its block tick, 60 +
//   nextInt(61) (a tick already scheduled is kept). In the tick it draws
//   nextInt(3); when that is 0 or fewer than four frosted ice blocks are
//   beside it, and its cell is brighter than 11 less its age and its light
//   dampening (block light, or sky light less the sky's darkening), it ages
//   by one (without neighbor updates), or at age 3 melts and then ages each
//   frosted neighbor (down, up, north, south, west, east), drawing a delay
//   of 20 + nextInt(21) for each that did not melt; otherwise it draws that
//   delay for itself.
// Snow layers' support is a block's need (support.cpp). Not modeled: melting
// and freezing by light and weather in random ticks, snow layers stacked by
// players.
namespace circuit::gameplay::detail {
namespace {
std::string_view name_of(BlockState s) {
    const auto state = physics::registry_state_of(s);
    const auto block = state ? registry::block_of(*state) : std::nullopt;
    return block ? registry::game_name(*block) : std::string_view{};
}

std::string value_of(BlockState s, std::string_view key) {
    for (const auto& [k, v] : registry::properties(s.registry_state()))
        if (k == key) return v;
    return {};
}

// A solid block other than cobwebs and bamboo shoots, or a liquid.
bool holds_melted_ice(BlockState below) {
    if (below.is(BlockType::water) || is_lava(below)) return true;
    const auto state = physics::registry_state_of(below);
    if (!state || !registry::solid(*state)) return false;
    const auto n = name_of(below);
    return n != "cobweb" && n != "bamboo_sapling";
}
} // namespace

bool is_snowy_dirt(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    const auto n = name_of(s);
    return n == "grass_block" || n == "podzol" || n == "mycelium";
}

void snowy_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d) {
    if (d != Direction::up) return;
    const auto above = name_of(ctx.block(pos.relative(d)));
    const std::string snowy = above == "snow" || above == "snow_block" || above == "powder_snow" ? "true" : "false";
    if (value_of(s, "snowy") == snowy) return;
    const std::vector<registry::Property> props{{"snowy", snowy}};
    set_block_quiet(ctx, pos, BlockState::registered(registry::state_of(*registry::block_of(s.registry_state()), props).value()));
}

bool is_frosted_ice(BlockState s) { return s.is(BlockType::other) && name_of(s) == "frosted_ice"; }

namespace {
void schedule_frosted(RuleContext& ctx, BlockPos pos, std::int32_t least, std::int32_t most) {
    const auto delay = static_cast<std::uint32_t>(least + ctx.random_int(most - least + 1));
    schedule(ctx, TickList::block, pos, BlockType::other, delay, 0);
}

void melt(RuleContext& ctx, BlockPos pos) {
    set_block(ctx, pos, ctx.dimension() == contracts::DimensionId::the_nether ? blocks::air() : blocks::water_source());
}

// One age older, or melted at age 3 (true).
bool slightly_melt(RuleContext& ctx, BlockPos pos, BlockState s) {
    const int age = std::stoi(value_of(s, "age"));
    if (age >= 3) {
        melt(ctx, pos);
        return true;
    }
    const std::vector<registry::Property> props{{"age", std::to_string(age + 1)}};
    set_block_quiet(ctx, pos, BlockState::registered(registry::state_of(*registry::block_of(s.registry_state()), props).value()));
    return false;
}
} // namespace

void frosted_ice_placed(RuleContext& ctx, BlockPos pos) { schedule_frosted(ctx, pos, 60, 120); }

void tick_frosted_ice(RuleContext& ctx, BlockPos pos, BlockState s) {
    const bool chance = ctx.random_int(3) == 0;
    const auto frosted = std::ranges::count_if(contracts::all_directions, [&](Direction d) { return is_frosted_ice(ctx.block(pos.relative(d))); });
    const int sky = static_cast<int>(ctx.sky_light(pos)) - static_cast<int>(ctx.sky().darken);
    const int bright = std::max(static_cast<int>(ctx.block_light(pos)), sky);
    const int age = std::stoi(value_of(s, "age"));
    if ((chance || frosted < 4) && bright > 11 - age - registry::light_dampening(s.registry_state()) && slightly_melt(ctx, pos, s)) {
        for (const auto d : contracts::all_directions) {
            const auto q = pos.relative(d);
            if (const auto n = ctx.block(q); is_frosted_ice(n) && !slightly_melt(ctx, q, n)) schedule_frosted(ctx, q, 20, 40);
        }
        return;
    }
    schedule_frosted(ctx, pos, 20, 40);
}

void frosted_ice_neighbor_changed(RuleContext& ctx, BlockPos pos, BlockState cause) {
    if (!is_frosted_ice(cause)) return;
    const auto frosted = std::ranges::count_if(contracts::all_directions, [&](Direction d) { return is_frosted_ice(ctx.block(pos.relative(d))); });
    if (frosted < 2) melt(ctx, pos);
}

void ice_broken(RuleContext& ctx, BlockPos pos, BlockState s, const ItemStack& tool) {
    const auto n = name_of(s);
    if (n != "ice" && n != "frosted_ice") return;
    if (const auto* data = ctx.data(); data && data->enchantment_level)
        if (std::ranges::any_of(data->keeps_ice, [&](std::uint32_t e) { return data->enchantment_level(tool, e) > 0; })) return;
    if (ctx.dimension() == contracts::DimensionId::the_nether) return;
    if (holds_melted_ice(ctx.block(pos.relative(Direction::down)))) set_block(ctx, pos, blocks::water_source());
}

} // namespace circuit::gameplay::detail
