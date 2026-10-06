// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/rules.hpp>
#include <array>
#include <optional>
#include <span>
#include <vector>

// Internal rule helpers shared by the mechanism files of the reference rules.
namespace circuit::gameplay::detail {
using contracts::Direction;

// Neighbor-update delivery order of the reference rules (unverified against
// the target version; see the compatibility table).
inline constexpr std::array<Direction, 6> update_order{
    Direction::west, Direction::east, Direction::down, Direction::up, Direction::north, Direction::south};
inline constexpr std::array<Direction, 4> spread_order{
    Direction::west, Direction::east, Direction::north, Direction::south};

// Neighbor updates to the six neighbors of `source` (except one direction).
void notify_neighbors(RuleContext& ctx, BlockPos source, std::optional<Direction> except = std::nullopt);
// One neighbor update delivered to `target`.
void notify_single(RuleContext& ctx, BlockPos target, BlockPos from);
// Shape updates to the six neighbors of `source` (west, east, north, south,
// down, up), queued like neighbor updates.
void notify_shapes(RuleContext& ctx, BlockPos source);
// A state change that sends shape updates but no neighbor updates.
void set_block_quiet(RuleContext& ctx, BlockPos pos, BlockState state);
// Output update of a diode-like component: its front block, then the front
// block's neighbors except the component itself.
void update_front(RuleContext& ctx, BlockPos pos, Direction output);
// `replaced` is the block type that was at `pos` before.
void on_placed(RuleContext& ctx, BlockPos pos, BlockState state, BlockType replaced);
void on_neighbor_changed(RuleContext& ctx, BlockPos target, BlockPos from);
void on_shape_changed(RuleContext& ctx, BlockPos target, BlockPos from);
// Power the block at `pos` emits toward the neighbor in direction `toward`.
// Comparators read their output signal from their block entity.
std::uint8_t emitted_power(RuleContext& ctx, BlockPos pos, BlockState state, Direction toward);
bool schedule(RuleContext& ctx, TickList list, BlockPos pos, BlockType block, std::uint32_t delay, std::int8_t priority);
bool queue_block_event(RuleContext& ctx, const BlockEvent& event);

void update_wire(RuleContext& ctx, BlockPos pos);
// True when the block in front of a repeater or comparator is another diode
// that does not face back into it.
bool feeds_diode(RuleContext& ctx, BlockPos pos, BlockState state);
void check_repeater(RuleContext& ctx, BlockPos pos);
void tick_repeater(RuleContext& ctx, BlockPos pos, BlockState state);
void check_lamp(RuleContext& ctx, BlockPos pos);
void tick_lamp(RuleContext& ctx, BlockPos pos, BlockState state);
void tick_observer(RuleContext& ctx, BlockPos pos, BlockState state);

bool piston_powered(RuleContext& ctx, BlockPos pos, Direction facing);
void check_piston(RuleContext& ctx, BlockPos pos);
void run_piston_event(RuleContext& ctx, const BlockEvent& event, BlockState state);
// Spawns the items a destroyed block drops, at the block center without velocity.
void drop_items(RuleContext& ctx, BlockPos pos, BlockState destroyed);
// Block-entity tick of a moving piston: completes the move after two ticks.
void tick_moving_piston(RuleContext& ctx, BlockPos pos);

void check_hopper(RuleContext& ctx, BlockPos pos);
void tick_hopper(RuleContext& ctx, BlockPos pos);

// Positions of the container at `pos` in combined slot order (right chest half
// first); empty if `pos` holds no container.
std::vector<BlockPos> container_parts(RuleContext& ctx, BlockPos pos);
// Comparator reading of a container: 0 when empty, else 1 + 14 * fullness.
std::uint8_t container_signal(RuleContext& ctx, BlockPos pos);
// Notifies comparators that read a container whose contents changed.
void container_changed(RuleContext& ctx, BlockPos pos);
void check_chest(RuleContext& ctx, BlockPos pos);

void check_comparator(RuleContext& ctx, BlockPos pos);
void tick_comparator(RuleContext& ctx, BlockPos pos, BlockState state);
// Recomputes the output at once (mode changes do not wait for the delay).
void refresh_comparator(RuleContext& ctx, BlockPos pos, BlockState state);

void tick_water(RuleContext& ctx, BlockPos pos, BlockState state);
void wheat_random_tick(RuleContext& ctx, BlockPos pos, BlockState state, Rng& stream);

constexpr std::uint64_t tick_code(TickList list, BlockType block) noexcept {
    return (static_cast<std::uint64_t>(list) << 8) | static_cast<std::uint64_t>(block);
}

} // namespace circuit::gameplay::detail

namespace circuit::gameplay::detail {
// Iteration order of a hash set of block positions in the target version, as
// observed in harness traces: positions are bucketed by a spread position
// hash over a power-of-two table (16 buckets, doubled while more than three
// quarters full) and keep insertion order within a bucket. `positions` is the
// insertion order of distinct positions; `inserted` is how many were ever
// inserted (removals do not shrink the table).
std::vector<BlockPos> hash_order(std::span<const BlockPos> positions, std::size_t inserted);
} // namespace circuit::gameplay::detail
