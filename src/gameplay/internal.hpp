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
// clicked block, or the cell beside it); the state placed at `target`, if it
// can stay there.
std::optional<std::uint32_t> building_block(RuleContext& ctx, std::uint16_t item);
bool doubles_slab(std::uint32_t block, BlockState s, const contracts::PlayerUse& c, bool clicked);
// Whether placing `block` replaces the snow layer `s` (the clicked block, or
// the cell beside it): snow adds a layer to fewer than eight (when clicked, on
// its top face); any other block replaces a single layer (place_snow).
bool snow_replaced(std::uint32_t block, BlockState s, Direction face, bool clicked);
// A snow layer of one layer: any block a player places replaces it.
bool single_snow_layer(BlockState s);
std::optional<BlockState> placed_building(RuleContext& ctx, std::uint32_t block, const contracts::PlayerUse& c, BlockPos target);
// The other block a placed block brings: a tall flower's upper half above
// it, a bed's head ahead of its foot.
std::optional<std::pair<BlockPos, BlockState>> second_part(BlockState placed, BlockPos pos);
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
contracts::Result<std::function<void()>> plan_button(RuleContext& ctx, const contracts::ContainerButton& c);
contracts::Result<std::function<void()>> plan_slot_enabled(RuleContext& ctx, const contracts::SetSlotEnabled& c);
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
BlockState placed_opening(RuleContext& ctx, BlockPos pos, BlockState s, const contracts::PlayerUse& use);
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
// (nothing for blocks that need a tool when none is given), `by_entity` when
// a player or an explosion's entity broke it (loot tables that need one).
void drop_items(RuleContext& ctx, BlockPos pos, BlockState destroyed, ItemStack tool = {}, bool by_entity = false);
// The stacks a block drops (loot tables roll from their random sequences):
// what `tool` harvests, or, when it `exploded`, what it drops without a tool.
std::vector<ItemStack> block_drops(RuleContext& ctx, BlockPos pos, BlockState destroyed, const ItemStack& tool, bool exploded,
                                   bool by_entity = false);
// Spawns one stack as a block drop at `pos` (drawing its offset and velocity).
void pop_resource(RuleContext& ctx, BlockPos pos, ItemStack stack);
// Scatters a removed container's contents around its cell, as both target
// versions do, drawing from the level random for every slot, empty or not.
void drop_contents(RuleContext& ctx, BlockPos pos, const std::vector<ItemStack>& slots);
// An entity's own random (Entity::own_random: seeded with its id when first
// used), and the state to keep after drawing from it.
LevelRandom own_random(const Entity& e);
void keep_random(Entity& e, const LevelRandom& r);
// Experience at a point as experience orbs (entities.cpp). Called in the
// command phase: a new orb's first motion depends on its id.
void award_experience(RuleContext& ctx, contracts::Vec3 at, std::int32_t amount);
// The experience a block a player harvests gives (drops.cpp; drawn from the
// level random after its drops), dropped as orbs at its center.
void drop_experience(RuleContext& ctx, BlockPos pos, BlockState destroyed, const ItemStack& tool);
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

// ---- blocks that need support (support.cpp) ----
// Torches, wall torches, lanterns, ladders, carpets, flowers and saplings.
bool needs_support(BlockState s);
// Whether it can stay with `near(offset)` the block at an offset from it and
// `light()` the light at its cell (mushrooms; none: full light).
using Near = std::function<BlockState(BlockPos offset)>;
bool supported(BlockState s, const Near& near, std::string_view release, const std::function<std::uint8_t()>& light = {});
bool supported_at(RuleContext& ctx, BlockPos pos, BlockState s);
// A shape update from `d`: unsupported, it breaks with its drops (sugar cane
// and cacti a tick later, in their block tick).
void support_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d);
// Sugar cane and cacti: whether they break in a block tick, its scheduling
// when unsupported (as after an update), and the tick.
bool checks_later(BlockState s);
void check_support_later(RuleContext& ctx, BlockPos pos, BlockState s);
void support_tick(RuleContext& ctx, BlockPos pos, BlockState s);
// Its own item, its drop when no data packs give its loot table.
std::optional<ItemStack> supported_drop(BlockState s);

// ---- cacti (farmland.cpp) ----
// An item loses `amount` health; true when that leaves none and it is gone.
bool hurt_item(RuleContext& ctx, EntityId id, int amount);
// After an item's move: each cactus whose cell its box reaches takes a point
// of its health; true when that leaves none and it is gone.
bool cactus_hurts(RuleContext& ctx, EntityId id);

// ---- farmland (farmland.cpp) ----
bool is_farmland(BlockState s);
// After an entity's move to `position` by `moved`: its fall, and the block it
// lands on taking it.
void entity_fell(RuleContext& ctx, Entity& e, Vec3 position, Vec3 moved, bool on_ground);
// The block an entity whose box is `box` stands on after a move by `moved`:
// the nearest block whose collision shape meets the box's bottom, or, with
// none, the one it met before the horizontal part of the move.
std::optional<BlockPos> supporting_block(RuleContext& ctx, const physics::Aabb& box, Vec3 position, Vec3 moved);
void farmland_shape_update(RuleContext& ctx, BlockPos pos, Direction d);
void tick_farmland(RuleContext& ctx, BlockPos pos);
void farmland_random_tick(RuleContext& ctx, BlockPos pos, BlockState s);

// ---- snow and ice (snow.cpp) ----
// Grass blocks, podzol and mycelium, and their snowy state from the block
// above on a shape update from it.
bool is_snowy_dirt(BlockState s);
void snowy_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d);
bool is_frosted_ice(BlockState s);
// A neighbor update from the block `cause`: frosted ice melts by frosted ice.
void frosted_ice_neighbor_changed(RuleContext& ctx, BlockPos pos, BlockState cause);
// A state of frosted ice set: its block tick's delay. Its block tick.
void frosted_ice_placed(RuleContext& ctx, BlockPos pos);
void tick_frosted_ice(RuleContext& ctx, BlockPos pos, BlockState s);
// Ice or frosted ice a player broke with `tool` (its block already gone).
void ice_broken(RuleContext& ctx, BlockPos pos, BlockState s, const ItemStack& tool);

// ---- fire (fire.cpp) ----
bool is_fire(BlockState s);
bool is_soul_fire(BlockState s);
// Whether fire or soul fire can stay with `near(offset)` the block at an offset from it.
bool fire_supported(BlockState s, const Near& near, std::string_view release);
// A state of fire set (`previous` was there before): its block tick's delay,
// and its removal where it cannot stay.
void fire_placed(RuleContext& ctx, BlockPos pos, BlockState s, BlockState previous);
// A shape update: gone unsupported, else its faces again.
void fire_shape_update(RuleContext& ctx, BlockPos pos, BlockState s);
void tick_fire(RuleContext& ctx, BlockPos pos, BlockState s);
void lava_random_tick(RuleContext& ctx, BlockPos pos);
// The damage of the fire cells an entity's box reaches (fire 1, soul fire
// 2), or -1 when it reaches none.
int fire_contact(RuleContext& ctx, const physics::Aabb& entity);
// Whether an entity's box is in lava, and how deep its bottom is.
bool in_lava(RuleContext& ctx, const physics::Aabb& entity, double* depth = nullptr);
// Netherite items and ancient debris: fire and lava leave them alone.
bool fire_resistant(const ItemStack& stack);

// ---- big dripleaves tilting (dripleaf.cpp) ----
bool is_big_dripleaf(BlockState s);
// An entity in the cell: on the leaf, it tilts it.
void dripleaf_entity_inside(RuleContext& ctx, BlockPos pos, BlockState s, const Entity& e);
// Its block tick: the next tilt, or back when powered.
void tick_dripleaf(RuleContext& ctx, BlockPos pos, BlockState s);
// A neighbor update: back when powered.
void dripleaf_neighbor_changed(RuleContext& ctx, BlockPos pos, BlockState s);
// A projectile hit it: tilted to full at once.
void dripleaf_hit(RuleContext& ctx, BlockPos pos);

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
// One of `stack` into the container at `pos`, coming in by `face` (which
// picks a furnace's slots).
bool insert_one(RuleContext& ctx, BlockPos pos, const ItemStack& stack, std::optional<Direction> face = std::nullopt);
// As much of `stack` as the container takes, slot by slot in its order (a
// crafter's result); returns what is left.
ItemStack insert_stack(RuleContext& ctx, BlockPos pos, ItemStack stack, std::optional<Direction> face = std::nullopt);
// Furnaces (furnace.cpp).
std::int32_t burn_time(std::uint16_t item);
// The cooking time of an input; without a recipe 200, or 0 with `recipe_only`.
std::int32_t cooking_time(RuleContext& ctx, BlockState furnace, const ItemStack& input, bool recipe_only = false);
void furnace_input_set(RuleContext& ctx, BlockPos pos, const ItemStack& before);
void tick_furnace(RuleContext& ctx, BlockPos pos, BlockState s);
std::vector<std::size_t> furnace_slots(Direction face);
bool furnace_accepts(const BlockEntity& be, std::size_t slot, const ItemStack& stack);
bool furnace_gives(std::size_t slot, const ItemStack& stack, Direction face);
// Brewing stands (brewing.cpp): the block-entity tick; the slots a face
// reaches, in order; whether a slot takes an item (`fuel`: what fuels them,
// GameData::brewing_fuel, or blaze powder without data) and gives one.
void tick_brewing_stand(RuleContext& ctx, BlockPos pos, BlockState s);
std::vector<std::size_t> brewing_slots(Direction face);
bool brewing_accepts(const GameData* data, const BlockEntity& be, std::size_t slot, const ItemStack& stack);
// Whether a slot of a brewing stand's (bottles 0-2, the ingredient, the
// fuel) takes an item, full or not: what its window lets players put there.
bool brewing_slot_takes(const GameData* data, std::size_t slot, std::uint16_t item);
bool brewing_gives(std::size_t slot, const ItemStack& stack);

// ---- dispensers and droppers (dispenser.cpp) ----
// A neighbor update: triggered by power at the block or above it.
void check_dispenser(RuleContext& ctx, BlockPos pos);
// Its scheduled tick: one item from a random slot.
void tick_dispenser(RuleContext& ctx, BlockPos pos, BlockState s);
// A stack thrown out through a front as dispensers throw items (`whole`:
// the stack as it is, else one item of it).
void eject_from(RuleContext& ctx, BlockPos pos, Direction facing, ItemStack stack, bool whole);

// ---- crafters (crafter.cpp) ----
// A neighbor update: triggered by power at the block.
void check_crafter(RuleContext& ctx, BlockPos pos);
// Its scheduled tick: crafts its grid, or fails.
void tick_crafter(RuleContext& ctx, BlockPos pos, BlockState s);
// Its block-entity tick: how long it still shows that it crafted.
void tick_crafter_entity(RuleContext& ctx, BlockPos pos, BlockState s);
// Whether a crafter's slot takes one more item of `stack` (hoppers, droppers).
bool crafter_accepts(const BlockEntity& be, std::size_t slot, const ItemStack& stack);
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
// TNT fire burned (its block already replaced): primed as by redstone.
void prime_burned(RuleContext& ctx, BlockPos pos);
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
// landed (hit something while falling), and in `moved` the movement the
// collisions left (made or not: what a fall counts).
// `self`, the entity that moves, also collides with the boxes of the
// entities it cannot pass (vehicle_collides).
bool move_entity(RuleContext& ctx, contracts::Vec3& position, contracts::Vec3& velocity, double width, double height,
                 const Entity* self = nullptr, contracts::Vec3* moved = nullptr);
// The same moving by `movement`, which collisions stop in `velocity`.
bool move_entity(RuleContext& ctx, contracts::Vec3& position, contracts::Vec3& velocity, contracts::Vec3 movement, double width,
                 double height, const Entity* self = nullptr, contracts::Vec3* moved = nullptr);
// The same for a `movement` other than the velocity (a piston's push): the
// velocity loses what the movement could not make.
bool move_entity_by(RuleContext& ctx, contracts::Vec3& position, contracts::Vec3& velocity, contracts::Vec3 movement, double width,
                    double height, const Entity* self = nullptr, contracts::Vec3* moved = nullptr);
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
// Buckets (buckets.cpp): the state with water in it or without, for blocks
// that hold water as a property (nullopt for the others); a filled bucket
// emptied into a cell (whether it took it); what an empty bucket takes from
// a cell (the bucket it fills); a player's use of a bucket along their view.
std::optional<BlockState> with_water(BlockState s, bool wet);
bool pour_fluid(RuleContext& ctx, BlockPos pos, bool lava);
std::optional<std::uint16_t> take_fluid(RuleContext& ctx, BlockPos pos);
contracts::Result<std::function<void()>> plan_use_item(RuleContext& ctx, const contracts::PlayerUseItem& c);
// Lava's fluid tick, and a change of it or beside it (hardening or a fluid
// tick; nature.cpp).
void tick_lava(RuleContext& ctx, BlockPos pos, BlockState state);
void lava_changed(RuleContext& ctx, BlockPos pos, BlockState state);
// Grass blocks and mycelium, nylium (grass.cpp).
bool is_nylium(BlockState s);
void nylium_random_tick(RuleContext& ctx, BlockPos pos, BlockState s);
bool is_spreading_dirt(BlockState s);
void spreading_dirt_random_tick(RuleContext& ctx, BlockPos pos, BlockState s);
// Ice, snow, redstone ore, budding amethyst and copper in random ticks
// (weathering.cpp).
bool is_weathering(BlockState s);
void weathering_random_tick(RuleContext& ctx, BlockPos pos, BlockState s);
// Pointed dripstone (dripstone.cpp).
bool is_pointed_dripstone(BlockState s);
bool dripstone_supported(RuleContext& ctx, BlockPos pos, BlockState s);
void dripstone_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction from);
void tick_dripstone(RuleContext& ctx, BlockPos pos, BlockState s);
void dripstone_random_tick(RuleContext& ctx, BlockPos pos, BlockState s);
bool is_cauldron(BlockState s);
void tick_cauldron(RuleContext& ctx, BlockPos pos, BlockState s);
// A block turned into a falling block entity where it is (falling.cpp).
void spawn_falling_block(RuleContext& ctx, BlockPos pos, BlockState s);
// Leaves' distance and decay (leaves.cpp).
bool is_leaves(BlockState s);
void leaves_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d);
void tick_leaves(RuleContext& ctx, BlockPos pos, BlockState s);
void leaves_random_tick(RuleContext& ctx, BlockPos pos, BlockState s);
// Other plants growing in random ticks (plants.cpp).
bool is_growing_plant(BlockState s);
// An eyeblossom's block tick or random tick: it turns if it should (true).
bool turn_eyeblossom(RuleContext& ctx, BlockPos pos, BlockState s);
bool is_bamboo(BlockState s);
// A bamboo stalk `height` tall grows a block above its top `pos` (plants.cpp).
void grow_bamboo_stalk(RuleContext& ctx, BlockPos pos, BlockState s, int height);
// Bone meal used on the block at `pos`; whether it acted (bone_meal.cpp).
bool apply_bone_meal(RuleContext& ctx, BlockPos pos);
// Bamboo's update from above (true when it changed).
bool bamboo_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d);
void plant_random_tick(RuleContext& ctx, BlockPos pos, BlockState s);
// Crops and stems (crops.cpp).
bool is_crop(BlockState s);
void crop_random_tick(RuleContext& ctx, BlockPos pos, BlockState s);
bool is_stem(BlockState s);
void stem_random_tick(RuleContext& ctx, BlockPos pos, BlockState s);
bool is_attached_stem(BlockState s);
// An attached stem whose fruit is gone becomes the ripe stem (true).
bool attached_stem_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d);

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
