// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/rules.hpp>
#include <array>

// Internal rule helpers shared by the mechanism files of the reference rules.
namespace circuit::gameplay::detail {
using contracts::Direction;

// Neighbor-update delivery order of the reference rules (unverified against
// the target version; see the compatibility table).
inline constexpr std::array<Direction, 6> update_order{
    Direction::west, Direction::east, Direction::down, Direction::up, Direction::north, Direction::south};
inline constexpr std::array<Direction, 4> spread_order{
    Direction::west, Direction::east, Direction::north, Direction::south};

void notify_neighbors(RuleContext& ctx, BlockPos source);
void on_placed(RuleContext& ctx, BlockPos pos, BlockState state);
void on_neighbor_changed(RuleContext& ctx, BlockPos target, BlockPos from);
std::uint8_t emitted_power(BlockState state, Direction toward) noexcept;
bool schedule(RuleContext& ctx, TickList list, BlockPos pos, BlockType block, std::uint32_t delay, std::int8_t priority);
bool queue_block_event(RuleContext& ctx, const BlockEvent& event);

void update_wire(RuleContext& ctx, BlockPos pos);
void check_repeater(RuleContext& ctx, BlockPos pos);
void tick_repeater(RuleContext& ctx, BlockPos pos, BlockState state);
void check_lamp(RuleContext& ctx, BlockPos pos);
void tick_lamp(RuleContext& ctx, BlockPos pos, BlockState state);
void tick_observer(RuleContext& ctx, BlockPos pos, BlockState state);

bool piston_powered(RuleContext& ctx, BlockPos pos, Direction facing);
void check_piston(RuleContext& ctx, BlockPos pos);
void run_piston_event(RuleContext& ctx, const BlockEvent& event, BlockState state);

void check_hopper(RuleContext& ctx, BlockPos pos);
void tick_hopper(RuleContext& ctx, BlockPos pos);

void tick_water(RuleContext& ctx, BlockPos pos, BlockState state);
void wheat_random_tick(RuleContext& ctx, BlockPos pos, BlockState state, Rng& stream);

constexpr std::uint64_t tick_code(TickList list, BlockType block) noexcept {
    return (static_cast<std::uint64_t>(list) << 8) | static_cast<std::uint64_t>(block);
}

} // namespace circuit::gameplay::detail
