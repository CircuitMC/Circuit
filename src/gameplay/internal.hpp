// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/rules.hpp>
#include <array>
#include <functional>
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
// One shape update delivered to `target`, as from its neighbor `from`.
void notify_shape(RuleContext& ctx, BlockPos target, BlockPos from);
// Shape updates to the six neighbors of `source` (west, east, north, south,
// down, up), queued like neighbor updates.
void notify_shapes(RuleContext& ctx, BlockPos source);
// The shape updates of a state change: dust's diagonal updates for its old
// shape, the six neighbors, then dust's diagonal updates for its new shape.
void shape_updates(RuleContext& ctx, BlockPos pos, BlockState prev, BlockState next);
// A state change that sends shape updates but no neighbor updates.
void set_block_quiet(RuleContext& ctx, BlockPos pos, BlockState state);
// Breaks a block: its drops, then air with full updates.
void destroy_block(RuleContext& ctx, BlockPos pos);
// Output update of a diode-like component: its front block, then the front
// block's neighbors except the component itself.
void update_front(RuleContext& ctx, BlockPos pos, Direction output);
// Reactions to a state set at `pos` (every change, `previous` was there before)
// and to the removal of a block of another type.
void on_placed(RuleContext& ctx, BlockPos pos, BlockState state, BlockState previous);
void on_removed(RuleContext& ctx, BlockPos pos, BlockState previous);
void on_neighbor_changed(RuleContext& ctx, BlockPos target, BlockPos from);
void on_shape_changed(RuleContext& ctx, BlockPos target, BlockPos from);

// ---- power (power.cpp) ----
// Full opaque blocks of the registry: powered strongly by a source, they power
// dust and components next to them; powered by dust only, components only.
bool is_conductor(BlockState s) noexcept;
// Whether the face of `s` toward `face` is whole: dust rests on it and levers hang on it.
bool face_sturdy(BlockState s, Direction face) noexcept;
bool supports_dust(BlockState below) noexcept;
bool supports_diode(BlockState below) noexcept;
// Direction from a lever to the block it hangs on.
Direction lever_attachment(BlockState lever) noexcept;
// Whether dust, a diode or a lever still has its support.
bool can_survive(RuleContext& ctx, BlockPos pos, BlockState s);
// Power the block at `pos` gives its neighbor in direction `toward`, and the
// strong power it gives the block it powers through (`direct`). With `wires`
// false, dust gives nothing: dust is not powered through blocks by dust.
std::uint8_t emitted(RuleContext& ctx, BlockPos pos, BlockState s, Direction toward, bool wires);
std::uint8_t direct(RuleContext& ctx, BlockPos pos, BlockState s, Direction toward, bool wires);
std::uint8_t strong_into(RuleContext& ctx, BlockPos pos, bool wires);
// Power a component reads from the block at `from` in direction `toward`: what
// the block gives, or for a conductor what it receives strongly.
std::uint8_t signal_from(RuleContext& ctx, BlockPos from, Direction toward, bool wires = true);
std::uint8_t best_neighbor_signal(RuleContext& ctx, BlockPos pos, bool wires);
bool has_neighbor_signal(RuleContext& ctx, BlockPos pos);

// ---- players (placement.cpp) ----
// The block a player places with `item`, oriented by the player's facing and
// the clicked face; nullopt for items that place no block.
std::optional<BlockState> placed_by(std::uint16_t item, Direction face, Direction horizontal, Direction look);
// Whether a block obstructs players standing in its cell.
bool has_collision(BlockState s);
// Records a player slot (contracts::player_slots: the cursor) after a change.
void record_slot(RuleContext& ctx, const PlayerInventory& inventory, std::uint8_t slot);
// Adds a stack to a player's inventory in the target version's order for
// picked-up items; returns what does not fit.
ItemStack add_to_player(RuleContext& ctx, PlayerInventory& inv, ItemStack stack, std::uint8_t selected);
bool player_has_room(const PlayerInventory& inv, ItemStack stack, std::uint8_t selected);
// Container menus (menus.cpp).
contracts::Result<std::function<void()>> plan_click(RuleContext& ctx, const contracts::ContainerClick& c);
contracts::Result<std::function<void()>> plan_close(RuleContext& ctx, const contracts::CloseContainer& c);
contracts::Result<std::function<void()>> plan_death(RuleContext& ctx, const contracts::PlayerDeath& c);

// ---- dust (redstone.cpp) ----
bool is_dot(BlockState s);
bool is_cross(BlockState s);
WireSide connecting_side(RuleContext& ctx, BlockPos pos, Direction d, bool open_above);
// The shape dust has from its neighbors now. A dot (`keep_dot`) stays a dot
// while nothing connects.
BlockState fresh_wire_shape(RuleContext& ctx, BlockPos pos, BlockState s, bool keep_dot);
void wire_shape_update(RuleContext& ctx, BlockPos pos, Direction from_side);
// Shape updates dust gives the dust diagonally above or below its connected sides.
void indirect_shapes(RuleContext& ctx, BlockPos pos, BlockState s);
void wire_placed(RuleContext& ctx, BlockPos pos);
void wire_removed(RuleContext& ctx, BlockPos pos, BlockState previous);
// A player's use: a cross becomes a dot and a dot a cross.
void toggle_wire(RuleContext& ctx, BlockPos pos, BlockState s);
// Input of a diode from behind, reading dust there whatever its shape.
std::uint8_t diode_input(RuleContext& ctx, BlockPos pos, BlockState s);
bool schedule(RuleContext& ctx, TickList list, BlockPos pos, BlockType block, std::uint32_t delay, std::int8_t priority);
bool queue_block_event(RuleContext& ctx, const BlockEvent& event);

void update_wire(RuleContext& ctx, BlockPos pos);
// True when the block in front of a repeater or comparator is another diode
// that does not face back into it.
bool feeds_diode(RuleContext& ctx, BlockPos pos, BlockState state);
bool repeater_input(RuleContext& ctx, BlockPos pos, BlockState s);
// A repeater is locked while a powered diode faces into either side.
bool repeater_locked_now(RuleContext& ctx, BlockPos pos, BlockState s);
void check_repeater(RuleContext& ctx, BlockPos pos);
void tick_repeater(RuleContext& ctx, BlockPos pos, BlockState state);
void repeater_shape_update(RuleContext& ctx, BlockPos pos, Direction from_side);
void check_lamp(RuleContext& ctx, BlockPos pos);
void tick_lamp(RuleContext& ctx, BlockPos pos, BlockState state);
void tick_observer(RuleContext& ctx, BlockPos pos, BlockState state);

bool piston_powered(RuleContext& ctx, BlockPos pos, Direction facing);
void check_piston(RuleContext& ctx, BlockPos pos);
void run_piston_event(RuleContext& ctx, const BlockEvent& event, BlockState state);
// Spawns the items a destroyed block drops, at the block center without
// velocity: what `tool` harvests (nothing for blocks that need a tool when
// none is given).
void drop_items(RuleContext& ctx, BlockPos pos, BlockState destroyed, ItemStack tool = {});
// Spawns a removed container's contents, one entity per slot.
void drop_contents(RuleContext& ctx, BlockPos pos, const std::vector<ItemStack>& slots);
// Records a spawned entity: entity_spawned, then entity_spawn_state with its
// exact position and velocity.
void record_spawn(RuleContext& ctx, EntityId id, const Entity& e);
// Spawns an item a player dropped.
void spawn_dropped(RuleContext& ctx, BlockPos cell, contracts::FixedVec position, contracts::FixedVec velocity, ItemStack stack,
                   std::uint16_t pickup_delay);
// Whether a piston head still has its base (or the moving piston taking its place).
bool head_has_base(RuleContext& ctx, BlockPos pos, BlockState head);
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
// The chest a player places at `pos`, from the placed state's facing, the
// clicked face and sneaking: it joins a neighboring single chest (see
// containers.cpp).
BlockState placed_chest(RuleContext& ctx, BlockPos pos, BlockState placed, Direction face, bool sneaking);

void check_comparator(RuleContext& ctx, BlockPos pos);
// The output a comparator computes now from its inputs and mode.
std::uint8_t comparator_output(RuleContext& ctx, BlockPos pos, BlockState s);
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
