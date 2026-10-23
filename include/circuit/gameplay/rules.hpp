// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/commands.hpp>
#include <circuit/gameplay/context.hpp>
#include <circuit/gameplay/rng.hpp>
#include <array>
#include <string_view>

// Entry points of circuit-reference-rules-v0. Each function is one root: an
// executor calls it in the phase's serial order with a context whose updater
// is idle. Immediate reactions complete before the function returns.
namespace circuit::gameplay {

// Applies one non-ticket command and records command_applied or
// command_rejected. Ticket commands are executor-owned (see TicketSet).
void apply_command(RuleContext& ctx, const contracts::Command& command);

void run_scheduled_tick(RuleContext& ctx, TickList list, const ScheduledTick& tick);
// Performs randomTickSpeed random ticks in one section using `stream`.
void run_random_ticks(RuleContext& ctx, ChunkPos chunk, std::int32_t section, Rng& stream);
void run_block_event(RuleContext& ctx, const BlockEvent& event);
void run_entity(RuleContext& ctx, EntityId id);
void run_block_entity(RuleContext& ctx, BlockPos pos);

// Rule primitives, exposed for fixtures.
bool set_block(RuleContext& ctx, BlockPos pos, BlockState state);
// Whether a block can be replaced, by a falling block that lands in it (and
// so falls into it): air, water, grass, snow and others (falling.cpp).
bool block_replaceable(BlockState s);
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
std::uint8_t received_power(RuleContext& ctx, BlockPos pos);

// Positions outside the level's height or the border are never loaded.
constexpr bool in_world(const contracts::WorldShape& shape, BlockPos pos) noexcept {
    return shape.contains_y(pos.y) && contracts::within_border(pos);
}
// In the overworld.
constexpr bool in_world(const contracts::VersionBundle& bundle, BlockPos pos) noexcept { return in_world(bundle.world, pos); }

} // namespace circuit::gameplay
