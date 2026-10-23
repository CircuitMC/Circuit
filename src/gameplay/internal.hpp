// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/physics.hpp>
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

// Neighbor updates to the six neighbors of `source` (except one direction),
// from the block `cause` (default: the block now at `source`).
void notify_neighbors(RuleContext& ctx, BlockPos source, std::optional<Direction> except = std::nullopt,
                      std::optional<BlockState> cause = std::nullopt);
// One neighbor update delivered to `target`.
void notify_single(RuleContext& ctx, BlockPos target, BlockPos from, std::optional<BlockState> cause = std::nullopt);
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
void update_front(RuleContext& ctx, BlockPos pos, Direction output, std::optional<BlockState> cause = std::nullopt);
// Reactions to a state set at `pos` (every change, `previous` was there before)
// and to the removal of a block of another type.
void on_placed(RuleContext& ctx, BlockPos pos, BlockState state, BlockState previous);
void on_removed(RuleContext& ctx, BlockPos pos, BlockState previous);
void on_neighbor_changed(RuleContext& ctx, BlockPos target, BlockPos from, BlockState cause);
void on_shape_changed(RuleContext& ctx, BlockPos target, BlockPos from);

// ---- power (power.cpp) ----
// Full opaque blocks of the registry: powered strongly by a source, they power
// dust and components next to them; powered by dust only, components only.
bool is_conductor(BlockState s) noexcept;
// Whether the face of `s` toward `face` is whole: dust rests on it and levers hang on it.
bool face_sturdy(BlockState s, Direction face) noexcept;
// Whether water can stand in the block: air, water and the blocks flowing
// water washes away.
bool holds_water(BlockState s);
bool supports_dust(BlockState below) noexcept;
bool supports_diode(BlockState below) noexcept;
// Direction from a lever to the block it hangs on.
Direction lever_attachment(BlockState lever) noexcept;
// Direction from a lever, button or redstone torch to the block it stands or hangs on.
Direction attachment(BlockState s) noexcept;
// Whether dust, a diode, a lever, a button or a torch still has its support.
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
std::optional<BlockState> placed_by(std::uint16_t item, const contracts::PlayerUse& use);
// The item of a button's or a pressure plate's kind.
std::uint16_t button_item(BlockState s);
std::uint16_t plate_item(BlockState s);
// Building blocks without rules of their own (placement.cpp): the block an
// item places, if it is one; whether a slab click doubles the slab `s` (the
// clicked block, or the cell beside it); the state placed at `target`.
std::optional<std::uint32_t> building_block(RuleContext& ctx, std::uint16_t item);
bool doubles_slab(std::uint32_t block, BlockState s, const contracts::PlayerUse& c, bool clicked);
BlockState placed_building(RuleContext& ctx, std::uint32_t block, const contracts::PlayerUse& c, BlockPos target);
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
// Sets the crafting result (slot 0) from the player's 2x2 grid (slots 1-4)
// and the data packs' recipes. Without data packs crafting is not modeled.
void update_crafting(RuleContext& ctx, PlayerInventory& inv);

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

// ---- redstone torches (torch.cpp) and buttons (button.cpp) ----
// The updates of every state a torch takes and of its removal: the
// neighbors of each of its neighbors.
void torch_updates(RuleContext& ctx, BlockPos pos, BlockState torch);
// Schedules the torch when its state disagrees with the power in its block.
void check_torch(RuleContext& ctx, BlockPos pos, BlockState s);
void tick_torch(RuleContext& ctx, BlockPos pos, BlockState s);
// A player or command uses the button.
void press_button(RuleContext& ctx, BlockPos pos, BlockState s);
void tick_button(RuleContext& ctx, BlockPos pos, BlockState s);
void button_removed(RuleContext& ctx, BlockPos pos, BlockState previous);
// An entity whose box ends a move in the cell of a wooden button makes it
// check for arrows.
void entity_at_button(RuleContext& ctx, BlockPos pos, BlockState s);

// ---- pressure plates and target blocks (plate.cpp) ----
bool is_plate(BlockType t) noexcept;
std::uint8_t plate_output(BlockState s) noexcept;
void tick_plate(RuleContext& ctx, BlockPos pos, BlockState s);
void plate_removed(RuleContext& ctx, BlockPos pos, BlockState previous);
// An entity at the end of its move: the plates and buttons in the cells its
// box reaches react.
void entity_inside(RuleContext& ctx, const Entity& entity);
// A projectile hit the face along `axis` (0 x, 1 y, 2 z) of the block at
// `pos` at `at`: a target block gives a signal for 20 ticks after an arrow,
// 8 after another projectile.
void target_hit(RuleContext& ctx, BlockPos pos, contracts::Vec3 at, int axis, bool arrow);
void tick_target(RuleContext& ctx, BlockPos pos, BlockState s);

// ---- doors, trapdoors and fence gates (door.cpp) ----
// A neighbor update from `from`: opens or closes it to match the power.
void check_opening(RuleContext& ctx, BlockPos pos, BlockPos from);
// Used by hand by a player facing `player` (horizontal).
void use_opening(RuleContext& ctx, BlockPos pos, BlockState s, Direction player);
// A shape update from `d`: a door half follows or loses its other half, a
// fence gate notices walls at its sides.
void opening_shape_update(RuleContext& ctx, BlockPos pos, Direction d);
// The state a player places at `pos`: powered and open where power reaches
// it, and a fence gate in a wall between walls.
BlockState placed_opening(RuleContext& ctx, BlockPos pos, BlockState s);
// The item of a door, trapdoor or fence gate, and the one an item places.
std::uint16_t opening_item(BlockState s);
std::optional<BlockState> placed_opening_by(std::uint16_t item, Direction face, Direction horizontal, bool upper);
// Registry data of the state a modeled block of several kinds stands for.
std::optional<registry::BlockInfo> modeled_info(BlockState s);

bool piston_powered(RuleContext& ctx, BlockPos pos, Direction facing);
void check_piston(RuleContext& ctx, BlockPos pos);
void run_piston_event(RuleContext& ctx, const BlockEvent& event, BlockState state);
// Spawns the items a destroyed block drops, around the block center with
// random offsets and velocities from the level random: what `tool` harvests
// (nothing for blocks that need a tool when none is given).
void drop_items(RuleContext& ctx, BlockPos pos, BlockState destroyed, ItemStack tool = {});
// The stacks a block drops without random quantities: what `tool` harvests,
// or, when it `exploded`, what it drops without a tool.
std::vector<ItemStack> block_drops(RuleContext& ctx, BlockState destroyed, const ItemStack& tool, bool exploded);
// Spawns one stack as a block drop at `pos` (drawing its offset and velocity).
void pop_resource(RuleContext& ctx, BlockPos pos, ItemStack stack);
// Scatters a removed container's contents around its cell, as both target
// versions do, drawing from the level random for every slot, empty or not.
void drop_contents(RuleContext& ctx, BlockPos pos, const std::vector<ItemStack>& slots);
// Records a spawned entity: entity_spawned (b: its stack, c: its kind), then
// entity_spawn_position and entity_spawn_velocity with their exact values.
void record_spawn(RuleContext& ctx, EntityId id, const Entity& e);
// Spawns an item a player dropped.
void spawn_dropped(RuleContext& ctx, BlockPos cell, contracts::Vec3 position, contracts::Vec3 velocity, ItemStack stack,
                   std::uint16_t pickup_delay);
// The block at `p`, or the block a moving piston there carries once its move
// is complete (its shape by then).
BlockState as_settled(RuleContext& ctx, BlockPos p);
// Whether a piston head still has its base (or the moving piston taking its place).
bool head_has_base(RuleContext& ctx, BlockPos pos, BlockState head);
// Block-entity tick of a moving piston: completes the move after two ticks.
void tick_moving_piston(RuleContext& ctx, BlockPos pos);

// ---- coral fans (coral.cpp) ----
bool coral_supported(RuleContext& ctx, BlockPos pos, BlockState s);
// A living fan placed, or a shape update from `d`: its death without water, or its fall.
void coral_placed(RuleContext& ctx, BlockPos pos, BlockState s);
void coral_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d);
void tick_coral(RuleContext& ctx, BlockPos pos, BlockState s);
// Whether a state holds water: waterlogged, or water itself, kelp, seagrass
// and bubble columns.
bool waterlogged(BlockState s);
bool holds_water_fluid(BlockState s);
// Blocks flowing water replaces, dropping their items (nature.cpp).
bool washed_away(BlockType t) noexcept;

// ---- rails (rail.cpp) ----
// A rail appeared (placed, set or moved in): its shape and its neighbors'.
void rail_placed(RuleContext& ctx, BlockPos pos);
// A neighbor update from a block `cause`.
void check_rail(RuleContext& ctx, BlockPos pos, BlockState cause);
void rail_removed(RuleContext& ctx, BlockPos pos, BlockState previous);
bool rail_supported(RuleContext& ctx, BlockPos pos);
std::uint16_t rail_item(BlockState s);
// An entity in a detector rail's cell: an unpowered one looks for minecarts.
void detector_entity_inside(RuleContext& ctx, BlockPos pos, BlockState s);
// A rail's scheduled tick: a powered detector rail looks again.
void tick_rail(RuleContext& ctx, BlockPos pos, BlockState s);
bool is_detector_rail(BlockState s) noexcept;

// ---- fences, panes and walls (connect.cpp) ----
bool is_connector(BlockState s);
// The state a fence, pane or wall takes from all its neighbors (a block a
// piston sets down).
BlockState joined_state(RuleContext& ctx, BlockPos pos, BlockState s);
// A shape update from `d`: a fence or pane joins or leaves the neighbor on
// that side, a wall also takes its heights and post from the block above.
void connector_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d);

// ---- stairs (stairs.cpp) ----
bool is_stairs(BlockState s);
// The shape stairs take from the stairs beside them.
BlockState shaped_stairs(RuleContext& ctx, BlockPos pos, BlockState s);
// A shape update from `d`: from a side, their shape again.
void stairs_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d);

// ---- note blocks (note.cpp) ----
// The state with the instrument its neighbors give it.
BlockState tuned_note_block(RuleContext& ctx, BlockPos pos, BlockState s);
// A neighbor update: sets it to the power around it, playing on a rise.
void check_note_block(RuleContext& ctx, BlockPos pos, BlockState s);
// Used: the next note, played.
void use_note_block(RuleContext& ctx, BlockPos pos, BlockState s);
// A shape update from `d`: from above or below, the instrument.
void note_block_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d);
// Its block event: the sound's draw.
void run_note_event(RuleContext& ctx, BlockState s);

// ---- daylight detectors (daylight.cpp) ----
// Block-entity tick: the output from the sky every 20 game ticks.
void tick_daylight_detector(RuleContext& ctx, BlockPos pos, BlockState s);
// Used: inverts it and sets its output.
void use_daylight_detector(RuleContext& ctx, BlockPos pos, BlockState s);

void check_hopper(RuleContext& ctx, BlockPos pos);
void tick_hopper(RuleContext& ctx, BlockPos pos);
// One item of the kind of `stack` into the container at `pos`, at the first
// slot that takes it (in combined order); an empty hopper that receives it
// waits 8 ticks. Returns whether it went in.
bool insert_one(RuleContext& ctx, BlockPos pos, const ItemStack& stack);

// ---- dispensers and droppers (dispenser.cpp) ----
// A neighbor update: triggered by power at the block or above it.
void check_dispenser(RuleContext& ctx, BlockPos pos);
// Its scheduled tick: one item from a random slot.
void tick_dispenser(RuleContext& ctx, BlockPos pos, BlockState s);
// An item that moved inside a hopper's cell: the hopper takes what it can of
// it when it is enabled, not cooling down, and the item reaches the space it
// collects from. Returns whether the item was taken whole (it is removed).
bool hopper_takes_item(RuleContext& ctx, BlockPos hopper, EntityId item);

// Falling blocks (falling.cpp): whether a block can be replaced by one that
// lands (and so falls into it), whether a block falls, the check two ticks
// after it is placed or changes, the block tick, and the entity's tick.
bool falls(BlockState s);
void schedule_fall(RuleContext& ctx, BlockPos pos);
void tick_falling_block(RuleContext& ctx, BlockPos pos, BlockState s);
void run_falling_block(RuleContext& ctx, EntityId id, Entity& e);

// Primed TNT (tnt.cpp): whether a block is TNT, what a TNT block does when
// placed or updated (redstone ignites it), and the entity's tick.
bool is_tnt(BlockState s);
// The state a placed TNT item becomes (not lit).
std::optional<BlockState> tnt_block(RuleContext& ctx);
void check_tnt(RuleContext& ctx, BlockPos pos);
// TNT an explosion reached (its block already gone): primed TNT with a fuse
// of 10 to 29 ticks.
void prime_by_explosion(RuleContext& ctx, BlockPos pos);
// TNT a burning projectile hit: primed as by redstone.
void prime_by_fire(RuleContext& ctx, BlockPos pos);
// TNT a dispenser primes in the cell `pos`, as by redstone; false when the
// game rule tntExplodes is off (the dispenser keeps it).
bool prime_dispensed(RuleContext& ctx, BlockPos pos);
// Arrows and snowballs (projectile.cpp).
void run_arrow(RuleContext& ctx, EntityId id, Entity& e);
void run_snowball(RuleContext& ctx, EntityId id, Entity& e);
void run_primed_tnt(RuleContext& ctx, EntityId id, Entity& e);

// Explosions (explosion.cpp): one of `power` centered at `center`.
void explode(RuleContext& ctx, contracts::Vec3 center, float power);
// `ids` in the order the target versions find entities in an area: by
// chunk section, then by when each entered its section.
std::vector<EntityId> entities_in_section_order(RuleContext& ctx, std::vector<EntityId> ids);

// What the water an entity's box touches does to it: whether it is in water,
// how deep its bottom is, and the push of the currents (0.014 a time).
struct InWater {
    bool inside{};
    double depth{};
    contracts::Vec3 push{};
};
InWater water_at(RuleContext& ctx, const physics::Aabb& entity);
// How slippery a block is to entities on it (0.6; ice 0.98, blue ice 0.989,
// slime 0.8).
float slipperiness(BlockState s);
// Minecarts off rails and boats (vehicle.cpp).
void run_minecart(RuleContext& ctx, EntityId id, Entity& e);
void run_boat(RuleContext& ctx, EntityId id, Entity& e);
// Moves an entity's box by its velocity against the blocks around it; the
// velocity loses the components that collided, and the vertical one too
// when it hit something. A move that a collision shortened to a squared
// length below 1e-7 is not made (both official servers leave such entities
// in place, while longer moves, from 1.8e-7, are made). Returns whether it
// landed (hit something while falling).
// `self`, the entity that moves, also collides with the boxes of the
// entities it cannot pass (vehicle_collides).
bool move_entity(RuleContext& ctx, contracts::Vec3& position, contracts::Vec3& velocity, double width, double height,
                 const Entity* self = nullptr);
// The same moving by `movement`, which collisions stop in `velocity`.
bool move_entity(RuleContext& ctx, contracts::Vec3& position, contracts::Vec3& velocity, contracts::Vec3 movement, double width,
                 double height, const Entity* self = nullptr);
// The same for a `movement` other than the velocity (a piston's push): the
// velocity loses what the movement could not make.
bool move_entity_by(RuleContext& ctx, contracts::Vec3& position, contracts::Vec3& velocity, contracts::Vec3 movement, double width,
                    double height, const Entity* self = nullptr);
// Whether a moving entity collides with another's box (both official
// servers): boats and minecarts with boats and minecarts, items, primed TNT
// and falling blocks with boats; minecarts are no obstacle to the others.
bool vehicle_collides(const Entity& mover, const Entity& other) noexcept;
// An entity whose box ends a move beside honey, below its top, falling,
// and outside its middle, slides down it (items and primed TNT, observed):
// with w = vy / 0.98f + 0.08, a w below -0.13 scales the horizontal velocity
// by -0.05 / w, and the vertical one becomes -0.13 * 0.98f, before drag.
// Returns whether it slid.
bool honey_slide(RuleContext& ctx, contracts::Vec3 position, contracts::Vec3& velocity, double width, double height, bool on_ground);
// The collision boxes of the block a moving piston at `pos` carries, where
// it stands in its move, in the cell's coordinates.
std::vector<registry::Box> moving_block_boxes(RuleContext& ctx, BlockPos pos);

// The box of an entity (items and snowballs: 0.25 wide and tall; arrows
// 0.5; falling blocks and primed TNT 0.98).
[[nodiscard]] physics::Aabb entity_box(const Entity& e);
// Ids of entities whose box intersects `area` (faces that only touch do
// not), ascending.
std::vector<EntityId> entities_intersecting(RuleContext& ctx, const physics::Aabb& area);

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
