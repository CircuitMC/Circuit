// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/commands.hpp>
#include <circuit/gameplay/context.hpp>
#include <circuit/gameplay/rng.hpp>
#include <array>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

// Entry points of circuit-reference-rules-v0. Each function is one root: an
// executor calls it in the phase's serial order with a context whose updater
// is idle. Immediate reactions complete before the function returns.
namespace circuit::gameplay {

// Applies one non-ticket command and records command_applied or
// command_rejected. Ticket commands are executor-owned (see TicketSet).
void apply_command(RuleContext& ctx, const contracts::Command& command);

void run_scheduled_tick(RuleContext& ctx, TickList list, const ScheduledTick& tick);
// One chunk's random ticks (random_ticks.cpp): randomTickSpeed precipitation
// checks from the level random, then randomTickSpeed positions picked in
// each of its `sections` sections, from the bottom, that holds blocks taking
// random ticks when its turn comes.
void run_random_ticks(RuleContext& ctx, ChunkPos chunk, std::int32_t sections);
// The order the official servers tick their random-ticking chunks in.
std::vector<ChunkPos> random_tick_order(std::vector<ChunkPos> chunks);
void run_block_event(RuleContext& ctx, const BlockEvent& event);
void run_entity(RuleContext& ctx, EntityId id);
// A new experience orb's first motion, drawn from its own random (seeded
// with its id): what experience given outside the command phase waits for
// until the commit gives the orb its id (Entity::launch).
void launch_orb(Entity& orb, contracts::OrbMotion motion);
void run_block_entity(RuleContext& ctx, BlockPos pos);
// The maps players hold (map_explore.cpp), after every level ran, as on both
// official servers, serially like the command phase: first the holders of
// every map who no longer carry it are forgotten; then, per level (the
// context's), each player there explores the filled map in their main hand,
// then the one in their off hand.
void forget_map_holders(RuleContext& ctx);
void explore_held_maps(RuleContext& ctx);
// One map explored by a player at `position` (the map's holder's step counted
// first); whether a pixel changed.
bool explore_map(RuleContext& ctx, MapData& map, contracts::PlayerKey player, contracts::Vec3 position);

// Rule primitives, exposed for fixtures.
bool set_block(RuleContext& ctx, BlockPos pos, BlockState state);
// Whether a block can be replaced, by a falling block that lands in it (and
// so falls into it): air, water, grass, snow and others (falling.cpp).
bool block_replaceable(BlockState s);
// How buckets and fluids treat a block (buckets.cpp; for checks against the
// official servers): whether it takes poured water, whether water and lava
// replace it, and the fluid it holds ("water", "flowing_water", "lava" or
// "flowing_lava", with its amount from 1 to 8), if any.
bool takes_poured_water(BlockState s);
// Whether flowing water (or lava) goes into a block, washing it away
// (nature.cpp; for checks against the official servers).
bool fluid_flows_into(BlockState s, bool lava);
bool fluids_replace(BlockState s);
std::optional<std::pair<std::string_view, int>> fluid_state(BlockState s);
// A block's blast resistance (its water's, 100, when waterlogged).
float blast_resistance(BlockState s);
// Whether power reaches the blocks beside a block through it.
bool redstone_conductor(BlockState s) noexcept;
// Fences, panes and walls (for checks against the official servers): whether
// the connector `connector` joins `neighbor` beside it on `side`, and the
// state a wall with sides joined as given (north, east, south, west) takes
// under `above` in `release`.
bool connector_joins(BlockState connector, BlockState neighbor, contracts::Direction side);
BlockState wall_under(BlockState wall, std::array<bool, 4> joined, BlockState above, std::string_view release);
// The shape stairs take with `around` on their north, east, south and west
// (for checks against the official servers).
BlockState stairs_with_neighbors(BlockState stairs, std::array<BlockState, 4> around);
// Whether a block that needs support (torches, lanterns, ladders, carpets,
// flowers, saplings) can stay with `around(d)` on each side (for checks
// against the official servers); blocks that need none always can.
bool block_supported(BlockState s, const std::function<BlockState(contracts::Direction)>& around, std::string_view release);
// Whether a block without rules of its own runs scheduled block ticks:
// falling blocks fall in them, sugar cane, cacti, vines and dripleaf stems
// break, big dripleaves tilt, farmland turns to dirt and fire burns.
bool block_ticks_scheduled(BlockState s);
// The same with `near(offset)` the block at any offset from it (sugar cane
// reads the blocks beside its ground, fire its flammable neighbors) and the
// light at its cell (mushrooms).
bool block_supported_near(BlockState s, const std::function<BlockState(contracts::BlockPos offset)>& near, std::string_view release,
                          std::uint8_t light = 15);
// Whether a door a player places takes its hinge on the right (for checks
// against the official servers): `sides` are the blocks to its left, above
// them, to its right and above them (as one faces the door's way), `along`
// how far the click hit right of the middle. A door of its kind on one side
// only puts the hinge on the other; else the side with more full blocks
// gets it; else the click's side, left for the middle.
bool door_right_hinge(BlockState door, std::array<BlockState, 4> sides, double along);
std::uint8_t received_power(RuleContext& ctx, BlockPos pos);

// Positions outside the level's height or the border are never loaded.
constexpr bool in_world(const contracts::WorldShape& shape, BlockPos pos) noexcept {
    return shape.contains_y(pos.y) && contracts::within_border(pos);
}
// In the overworld.
constexpr bool in_world(const contracts::VersionBundle& bundle, BlockPos pos) noexcept { return in_world(bundle.world, pos); }

} // namespace circuit::gameplay
