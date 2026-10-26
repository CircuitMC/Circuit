// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/errors.hpp>
#include <circuit/contracts/ids.hpp>
#include <circuit/gameplay/items.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Block states of the reference rules. Blocks with rules of their own have a
// BlockType and a packed state; every other block state of the registry
// (block_registry.hpp) is an `other` state that carries its internal state id.
namespace circuit::gameplay {
using contracts::Direction;

enum class BlockType : std::uint8_t {
    air = 0,        // level: 0 air, 1 cave air, 2 void air
    stone,
    glass,
    obsidian,
    redstone_block,
    lever,          // flag: powered; facing (horizontal) and level (LeverFace): mounting, display only
    redstone_wire,  // level: power 0-15; wire sides (WireSide per horizontal side, stored as the target version stores them)
    repeater,       // facing: output side (horizontal); delay 1-4; flag: powered; level: 1 locked
    redstone_lamp,  // flag: lit
    observer,       // facing: observed side; flag: powered (output at the back)
    piston,         // facing: push side; flag: extended
    sticky_piston,  // facing: push side; flag: extended
    piston_head,    // facing: same as its piston; flag: sticky; level: 1 short
    slime_block,
    hopper,         // facing: output side (not up); flag: enabled
    chest,          // facing: front (horizontal); level: ChestType; flag: waterlogged (not simulated)
    water,          // level 0: source, 1-7: flowing distance; flag: falling (level: stored level - 8, display only)
    wheat,          // level: age 0-7
    comparator,     // facing: output side (horizontal); level: 0 compare, 1 subtract; flag: powered
    moving_piston,  // facing: facing of the moving piston; flag: sticky. Block entity holds the moving block.
    other,          // Any other registry block: bits 8-31 hold its internal state. Inert (see below).
    redstone_torch,      // flag: lit
    redstone_wall_torch, // facing: away from the block it hangs on (horizontal); flag: lit
    button,              // facing (horizontal) and level (LeverFace): mounting, as for levers; flag: powered; variant: ButtonKind
    pressure_plate,      // flag: powered; variant: ButtonKind (the same kinds)
    weighted_plate,      // level: power 0-15; variant: 0 light, 1 heavy
    target,              // level: power 0-15
    door,                // facing (horizontal); level: bit 0 upper half, bit 1 right hinge; flag: open; extra: powered; variant: DoorKind
    trapdoor,            // facing (horizontal); level: bit 0 top half, bit 1 waterlogged (stored); flag: open; extra: powered; variant: DoorKind
    fence_gate,          // facing (horizontal); level: 1 in a wall; flag: open; extra: powered; variant: DoorKind (wooden kinds)
    honey_block,
    daylight_detector,   // level: power 0-15; flag: inverted. Block entity: ticks it every 20 game ticks
    note_block,          // level and extra bit 0: note 0-24 (note_of); flag: powered; variant: registry::Instrument
    rail,                // level: RailShape; flag: powered (not plain rails); extra: waterlogged (stored); variant: RailKind
    coral_fan,           // flag: waterlogged; variant: CoralKind, + 5 when dead
    coral_wall_fan,      // facing: away from the block it hangs on (horizontal); flag: waterlogged; variant: as coral_fan
    dispenser,           // facing: front (any side); flag: triggered; variant: 0 dispenser, 1 dropper. Block entity: nine slots
    furnace,             // facing: front (horizontal); flag: lit; variant: FurnaceKind. Block entity: input, fuel and result slots, its times
    crafter,             // facing: front (any side); variant: the top's direction (up for a horizontal front); flag: triggered;
                         // extra: crafting. Block entity: nine grid slots, the disabled ones, its crafting ticks
    brewing_stand,       // level: its bottles (bit k: has_bottle_k). Block entity: three bottles, the ingredient, the fuel; its
                         // fuel, brew time and the ingredient it brews with
    count_,
    unloaded = 255, // Read sentinel for unloaded chunks and positions outside the world height.
};

// How dust meets one horizontal side: not at all, on its level, or climbing the
// block on that side.
enum class WireSide : std::uint8_t { none = 0, side = 1, up = 2 };

// Packed state: bits 0-7 type, 8-10 facing, 11-14 level, 15 flag, 16-17 delay-1
// (doors, trapdoors and fence gates: extra, their powered state),
// 18-25 the four wire sides (two bits each: north, south, west, east) or,
// for blocks of several kinds, the variant. An `other` state keeps its internal registry
// state in bits 8-31 instead.
class BlockState {
public:
    constexpr BlockState() noexcept = default;
    static constexpr BlockState of(BlockType type) noexcept { return BlockState(static_cast<std::uint32_t>(type)); }
    static constexpr BlockState unloaded_sentinel() noexcept { return of(BlockType::unloaded); }
    // A registry state of a block without rules of its own (not validated; see decode).
    static constexpr BlockState registered(std::uint32_t internal_state) noexcept {
        return BlockState(static_cast<std::uint32_t>(BlockType::other) | internal_state << 8);
    }
    // Validates a registry value received from a command or a record.
    static contracts::Result<BlockState> decode(std::uint32_t raw) noexcept;

    [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return raw_; }
    [[nodiscard]] constexpr BlockType type() const noexcept { return static_cast<BlockType>(raw_ & 0xFF); }
    [[nodiscard]] constexpr bool is(BlockType t) const noexcept { return type() == t; }
    [[nodiscard]] constexpr bool is_air() const noexcept { return type() == BlockType::air; }
    [[nodiscard]] constexpr bool is_unloaded() const noexcept { return type() == BlockType::unloaded; }
    // Internal registry state of an `other` state.
    [[nodiscard]] constexpr std::uint32_t registry_state() const noexcept { return raw_ >> 8; }
    [[nodiscard]] constexpr Direction facing() const noexcept { return static_cast<Direction>((raw_ >> 8) & 7); }
    [[nodiscard]] constexpr std::uint8_t level() const noexcept { return static_cast<std::uint8_t>((raw_ >> 11) & 15); }
    [[nodiscard]] constexpr bool flag() const noexcept { return (raw_ >> 15) & 1; }
    [[nodiscard]] constexpr std::uint8_t delay() const noexcept { return static_cast<std::uint8_t>(((raw_ >> 16) & 3) + 1); }
    // The same two bits as they are, for blocks without a delay.
    [[nodiscard]] constexpr std::uint8_t extra() const noexcept { return static_cast<std::uint8_t>((raw_ >> 16) & 3); }
    [[nodiscard]] constexpr WireSide wire_side(Direction d) const noexcept {
        return static_cast<WireSide>((raw_ >> wire_shift(d)) & 3);
    }
    [[nodiscard]] constexpr bool wire_connected(Direction d) const noexcept { return wire_side(d) != WireSide::none; }
    // Which block of a family the state is (buttons and pressure plates: ButtonKind).
    [[nodiscard]] constexpr std::uint8_t variant() const noexcept { return static_cast<std::uint8_t>((raw_ >> 18) & 0xFF); }

    [[nodiscard]] constexpr BlockState with_facing(Direction d) const noexcept {
        return BlockState((raw_ & ~(7u << 8)) | (static_cast<std::uint32_t>(d) << 8));
    }
    [[nodiscard]] constexpr BlockState with_level(std::uint8_t level) const noexcept {
        return BlockState((raw_ & ~(15u << 11)) | (static_cast<std::uint32_t>(level & 15) << 11));
    }
    [[nodiscard]] constexpr BlockState with_flag(bool value) const noexcept {
        return BlockState((raw_ & ~(1u << 15)) | (static_cast<std::uint32_t>(value) << 15));
    }
    [[nodiscard]] constexpr BlockState with_extra(std::uint8_t extra) const noexcept {
        return BlockState((raw_ & ~(3u << 16)) | (static_cast<std::uint32_t>(extra & 3) << 16));
    }
    [[nodiscard]] constexpr BlockState with_delay(std::uint8_t delay) const noexcept {
        return BlockState((raw_ & ~(3u << 16)) | (static_cast<std::uint32_t>((delay - 1) & 3) << 16));
    }
    [[nodiscard]] constexpr BlockState with_wire_side(Direction d, WireSide side) const noexcept {
        return BlockState((raw_ & ~(3u << wire_shift(d))) | (static_cast<std::uint32_t>(side) << wire_shift(d)));
    }
    [[nodiscard]] constexpr BlockState with_variant(std::uint8_t variant) const noexcept {
        return BlockState((raw_ & ~(0xFFu << 18)) | (static_cast<std::uint32_t>(variant) << 18));
    }
    // All four wire sides at once (used to compare shapes).
    [[nodiscard]] constexpr std::uint32_t wire_shape() const noexcept { return (raw_ >> 18) & 0xFF; }
    [[nodiscard]] constexpr BlockState with_wire_shape(std::uint32_t shape) const noexcept {
        return BlockState((raw_ & ~(0xFFu << 18)) | ((shape & 0xFF) << 18));
    }
    constexpr bool operator==(const BlockState&) const noexcept = default;

private:
    explicit constexpr BlockState(std::uint32_t raw) noexcept : raw_(raw) {}
    static constexpr unsigned wire_shift(Direction d) noexcept {
        switch (d) {
        case Direction::north: return 18;
        case Direction::south: return 20;
        case Direction::west: return 22;
        case Direction::east: return 24;
        default: return 26; // vertical directions have no wire side; bits beyond the state are zero
        }
    }
    std::uint32_t raw_{};
};

// Convenience constructors used by rules and fixtures.
namespace blocks {
constexpr BlockState air() { return BlockState::of(BlockType::air); }
constexpr BlockState cave_air() { return air().with_level(1); }
constexpr BlockState void_air() { return air().with_level(2); }
constexpr BlockState stone() { return BlockState::of(BlockType::stone); }
constexpr BlockState glass() { return BlockState::of(BlockType::glass); }
constexpr BlockState obsidian() { return BlockState::of(BlockType::obsidian); }
constexpr BlockState redstone_block() { return BlockState::of(BlockType::redstone_block); }
constexpr BlockState slime() { return BlockState::of(BlockType::slime_block); }
constexpr BlockState honey() { return BlockState::of(BlockType::honey_block); }
// Lever mounting is display data: the rules ignore it.
enum class LeverFace : std::uint8_t { floor = 0, wall = 1, ceiling = 2 };
constexpr BlockState lever(bool on = false, Direction facing = Direction::north, LeverFace face = LeverFace::floor) {
    return BlockState::of(BlockType::lever).with_flag(on).with_facing(facing).with_level(static_cast<std::uint8_t>(face));
}
// Dust with no stored sides: a dot, as the target version stores dust placed
// without a shape. Players place a cross, which the rules compute.
constexpr BlockState wire(std::uint8_t power = 0) { return BlockState::of(BlockType::redstone_wire).with_level(power); }
constexpr BlockState wire(std::uint8_t power, WireSide north, WireSide south, WireSide west, WireSide east) {
    return wire(power)
        .with_wire_side(Direction::north, north)
        .with_wire_side(Direction::south, south)
        .with_wire_side(Direction::west, west)
        .with_wire_side(Direction::east, east);
}
constexpr BlockState repeater(Direction output, std::uint8_t delay = 1, bool powered = false, bool locked = false) {
    return BlockState::of(BlockType::repeater).with_facing(output).with_delay(delay).with_flag(powered).with_level(locked ? 1 : 0);
}
constexpr BlockState lamp(bool lit = false) { return BlockState::of(BlockType::redstone_lamp).with_flag(lit); }
constexpr BlockState observer(Direction observed, bool powered = false) {
    return BlockState::of(BlockType::observer).with_facing(observed).with_flag(powered);
}
constexpr BlockState piston(Direction push, bool sticky = false) {
    return BlockState::of(sticky ? BlockType::sticky_piston : BlockType::piston).with_facing(push);
}
constexpr BlockState hopper(Direction output = Direction::down, bool enabled = true) {
    return BlockState::of(BlockType::hopper).with_facing(output).with_flag(enabled);
}
enum class ChestType : std::uint8_t { single = 0, left = 1, right = 2 };
constexpr BlockState chest(Direction facing = Direction::north, ChestType type = ChestType::single) {
    return BlockState::of(BlockType::chest).with_facing(facing).with_level(static_cast<std::uint8_t>(type));
}
constexpr BlockState moving_piston(Direction facing, bool sticky) {
    return BlockState::of(BlockType::moving_piston).with_facing(facing).with_flag(sticky);
}
constexpr BlockState comparator(Direction output, bool subtract = false, bool powered = false) {
    return BlockState::of(BlockType::comparator).with_facing(output).with_level(subtract ? 1 : 0).with_flag(powered);
}
constexpr BlockState water_source() { return BlockState::of(BlockType::water); }
constexpr BlockState water_flowing(std::uint8_t distance) { return BlockState::of(BlockType::water).with_level(distance); }
constexpr BlockState water_falling() { return BlockState::of(BlockType::water).with_flag(true); }
constexpr BlockState wheat(std::uint8_t age = 0) { return BlockState::of(BlockType::wheat).with_level(age); }
constexpr BlockState torch(bool lit = true) { return BlockState::of(BlockType::redstone_torch).with_flag(lit); }
// A wall torch faces away from the block it hangs on.
constexpr BlockState wall_torch(Direction facing, bool lit = true) {
    return BlockState::of(BlockType::redstone_wall_torch).with_facing(facing).with_flag(lit);
}
// The buttons and pressure plates, wooden ones first (registry names:
// "<kind>_button", "<kind>_pressure_plate").
enum class ButtonKind : std::uint8_t {
    oak, spruce, birch, jungle, acacia, cherry, dark_oak, pale_oak, mangrove, bamboo, crimson, warped,
    stone, polished_blackstone,
};
inline constexpr std::uint8_t button_kinds = 14;
// Mounted as levers are: wall buttons face away from their block.
constexpr BlockState button(ButtonKind kind, bool powered = false, Direction facing = Direction::north,
                            LeverFace face = LeverFace::wall) {
    return BlockState::of(BlockType::button).with_variant(static_cast<std::uint8_t>(kind)).with_flag(powered).with_facing(facing)
        .with_level(static_cast<std::uint8_t>(face));
}
constexpr BlockState pressure_plate(ButtonKind kind, bool powered = false) {
    return BlockState::of(BlockType::pressure_plate).with_variant(static_cast<std::uint8_t>(kind)).with_flag(powered);
}
constexpr BlockState weighted_plate(bool heavy, std::uint8_t power = 0) {
    return BlockState::of(BlockType::weighted_plate).with_variant(heavy ? 1 : 0).with_level(power);
}
constexpr BlockState target(std::uint8_t power = 0) { return BlockState::of(BlockType::target).with_level(power); }
constexpr BlockState daylight_detector(bool inverted = false, std::uint8_t power = 0) {
    return BlockState::of(BlockType::daylight_detector).with_flag(inverted).with_level(power);
}
// Rails: plain ones take every shape, the others only the straight and
// ascending ones (the first six). Registry names: "rail", "<kind>_rail".
enum class RailKind : std::uint8_t { rail, powered, activator, detector };
inline constexpr std::uint8_t rail_kinds = 4;
enum class RailShape : std::uint8_t {
    north_south, east_west, ascending_east, ascending_west, ascending_north, ascending_south,
    south_east, south_west, north_west, north_east,
};
constexpr BlockState rail(RailKind kind = RailKind::rail, RailShape shape = RailShape::north_south, bool powered = false) {
    return BlockState::of(BlockType::rail).with_variant(static_cast<std::uint8_t>(kind)).with_level(static_cast<std::uint8_t>(shape))
        .with_flag(powered);
}
// Coral fans: the kinds, living (variant 0-4) and dead (5-9). Registry names:
// "<kind>_coral_fan", "dead_<kind>_coral_fan", "<kind>_coral_wall_fan".
enum class CoralKind : std::uint8_t { tube, brain, bubble, fire, horn };
inline constexpr std::uint8_t coral_kinds = 5;
constexpr BlockState coral_fan(CoralKind kind, bool dead = false, bool waterlogged = false) {
    return BlockState::of(BlockType::coral_fan).with_variant(static_cast<std::uint8_t>(static_cast<std::uint8_t>(kind) + (dead ? coral_kinds : 0)))
        .with_flag(waterlogged);
}
constexpr BlockState coral_wall_fan(CoralKind kind, Direction facing, bool dead = false, bool waterlogged = false) {
    return BlockState::of(BlockType::coral_wall_fan)
        .with_variant(static_cast<std::uint8_t>(static_cast<std::uint8_t>(kind) + (dead ? coral_kinds : 0)))
        .with_facing(facing).with_flag(waterlogged);
}
// A dispenser, or with `dropper` a dropper, facing `front`.
constexpr BlockState dispenser(Direction front, bool dropper = false, bool triggered = false) {
    return BlockState::of(BlockType::dispenser).with_facing(front).with_variant(dropper ? 1 : 0).with_flag(triggered);
}
// Furnaces, smokers and blast furnaces: one type, the kind as its variant.
enum class FurnaceKind : std::uint8_t { furnace, smoker, blast_furnace };
constexpr BlockState furnace(FurnaceKind kind, Direction front, bool lit = false) {
    return BlockState::of(BlockType::furnace).with_facing(front).with_variant(static_cast<std::uint8_t>(kind)).with_flag(lit);
}
// A crafter: its front, the direction its top faces (up unless the front
// is up or down), triggered and crafting.
constexpr BlockState crafter(Direction front, Direction top = Direction::up, bool triggered = false, bool crafting = false) {
    return BlockState::of(BlockType::crafter).with_facing(front).with_variant(static_cast<std::uint8_t>(top)).with_flag(triggered)
        .with_extra(crafting ? 1 : 0);
}
// A brewing stand showing the bottles of `bottles` (bit k: slot k).
constexpr BlockState brewing_stand(std::uint8_t bottles = 0) {
    return BlockState::of(BlockType::brewing_stand).with_level(static_cast<std::uint8_t>(bottles & 7));
}
// A note block; `instrument` is a registry::Instrument.
constexpr BlockState note_block(std::uint8_t instrument = 0, std::uint8_t note = 0, bool powered = false) {
    return BlockState::of(BlockType::note_block).with_variant(instrument).with_level(static_cast<std::uint8_t>(note & 15))
        .with_extra(static_cast<std::uint8_t>(note >> 4)).with_flag(powered);
}
// Doors and trapdoors, wooden ones first, then iron and the copper ones;
// fence gates come in the wooden kinds (registry names: "<kind>_door",
// "<kind>_trapdoor", "<kind>_fence_gate").
enum class DoorKind : std::uint8_t {
    oak, spruce, birch, jungle, acacia, cherry, dark_oak, pale_oak, mangrove, bamboo, crimson, warped,
    iron, copper, exposed_copper, weathered_copper, oxidized_copper,
    waxed_copper, waxed_exposed_copper, waxed_weathered_copper, waxed_oxidized_copper,
};
inline constexpr std::uint8_t door_kinds = 21;
inline constexpr std::uint8_t fence_gate_kinds = 12;
constexpr BlockState door(DoorKind kind, Direction facing, bool upper, bool right_hinge = false, bool open = false, bool powered = false) {
    return BlockState::of(BlockType::door).with_variant(static_cast<std::uint8_t>(kind)).with_facing(facing)
        .with_level(static_cast<std::uint8_t>((upper ? 1 : 0) | (right_hinge ? 2 : 0))).with_flag(open).with_extra(powered ? 1 : 0);
}
constexpr BlockState trapdoor(DoorKind kind, Direction facing, bool top = false, bool open = false, bool powered = false) {
    return BlockState::of(BlockType::trapdoor).with_variant(static_cast<std::uint8_t>(kind)).with_facing(facing)
        .with_level(top ? 1 : 0).with_flag(open).with_extra(powered ? 1 : 0);
}
constexpr BlockState fence_gate(DoorKind kind, Direction facing, bool open = false, bool powered = false, bool in_wall = false) {
    return BlockState::of(BlockType::fence_gate).with_variant(static_cast<std::uint8_t>(kind)).with_facing(facing)
        .with_level(in_wall ? 1 : 0).with_flag(open).with_extra(powered ? 1 : 0);
}
} // namespace blocks
// "dead_tube_coral_fan" for a dead tube coral fan's variant; with `wall`, its wall fan.
std::string coral_fan_name(std::uint8_t variant, bool wall);
// "north_south" for RailShape::north_south.
std::string_view rail_shape_name(std::uint8_t shape) noexcept;
// "harp" for a note block's instrument 0 (registry::Instrument).
std::string_view instrument_name(std::uint8_t instrument) noexcept;
// "oak" for an oak door, "waxed_copper" for a waxed copper one.
std::string_view door_kind_name(std::uint8_t variant) noexcept;
// Iron doors and trapdoors open only by power.
constexpr bool opens_by_hand(BlockState s) noexcept { return s.variant() != static_cast<std::uint8_t>(blocks::DoorKind::iron); }
constexpr bool upper_half(BlockState door) noexcept { return (door.level() & 1) != 0; }
// "oak" for an oak button.
std::string_view button_kind_name(std::uint8_t variant) noexcept;
// Wooden buttons stay pressed longer and arrows press them.
constexpr bool wooden_button(BlockState s) noexcept {
    return s.is(BlockType::button) && s.variant() < static_cast<std::uint8_t>(blocks::ButtonKind::stone);
}
// The note block's instruments (registry::instrument_count), its variant.
inline constexpr std::uint8_t note_instruments = 23;
// Blocks of several kinds that share a BlockType and keep the kind as their variant.
constexpr bool has_variant(BlockType t) noexcept {
    return t == BlockType::button || t == BlockType::pressure_plate || t == BlockType::weighted_plate || t == BlockType::door
        || t == BlockType::trapdoor || t == BlockType::fence_gate || t == BlockType::rail || t == BlockType::coral_fan
        || t == BlockType::coral_wall_fan || t == BlockType::dispenser || t == BlockType::furnace || t == BlockType::crafter;
}
constexpr bool is_coral_fan(BlockType t) noexcept { return t == BlockType::coral_fan || t == BlockType::coral_wall_fan; }
constexpr bool dead_coral(BlockState s) noexcept { return s.variant() >= blocks::coral_kinds; }
constexpr std::uint8_t variant_count(BlockType t) noexcept {
    switch (t) {
    case BlockType::weighted_plate: return 2;
    case BlockType::door: case BlockType::trapdoor: return blocks::door_kinds;
    case BlockType::fence_gate: return blocks::fence_gate_kinds;
    case BlockType::rail: return blocks::rail_kinds;
    case BlockType::coral_fan: case BlockType::coral_wall_fan: return 2 * blocks::coral_kinds;
    case BlockType::dispenser: return 2;
    case BlockType::furnace: return 3;
    case BlockType::crafter: return 6;
    default: return has_variant(t) ? blocks::button_kinds : 1;
    }
}
// A note block's note (0-24) and the state with another one.
constexpr std::uint8_t note_of(BlockState s) noexcept { return static_cast<std::uint8_t>(s.level() | (s.extra() << 4)); }
constexpr BlockState with_note(BlockState s, std::uint8_t note) noexcept {
    return s.with_level(static_cast<std::uint8_t>(note & 15)).with_extra(static_cast<std::uint8_t>(note >> 4));
}
constexpr bool is_opening(BlockType t) noexcept { return t == BlockType::door || t == BlockType::trapdoor || t == BlockType::fence_gate; }
constexpr bool is_torch(BlockType t) noexcept { return t == BlockType::redstone_torch || t == BlockType::redstone_wall_torch; }

// Whether an internal registry block has rules of its own (a BlockType other
// than `other`); its states never use the registry encoding.
bool modeled_block(std::uint32_t internal_block) noexcept;

// Whether an `other` state has a collision shape (from the registry).
bool other_has_collision(BlockState s) noexcept;

// Whether two states are of the same block (for `other`, the same registry block).
bool same_block(BlockState a, BlockState b) noexcept;

constexpr bool is_piston(BlockType t) noexcept { return t == BlockType::piston || t == BlockType::sticky_piston; }
constexpr bool repeater_locked(BlockState s) noexcept { return s.is(BlockType::repeater) && s.level() == 1; }
// Every valid state: the modeled states in raw order, then the `other` states
// in internal order.
template <class F>
void for_each_block_state(F&& f);
constexpr bool is_container(BlockType t) noexcept {
    return t == BlockType::hopper || t == BlockType::chest || t == BlockType::dispenser || t == BlockType::furnace || t == BlockType::crafter
        || t == BlockType::brewing_stand;
}
// Block entities hold container slots, hopper cooldowns, comparator output
// signals and moving blocks; a daylight detector's ticks it.
constexpr bool has_block_entity(BlockType t) noexcept {
    return is_container(t) || t == BlockType::comparator || t == BlockType::moving_piston || t == BlockType::daylight_detector;
}
// Direction from a chest half to its partner (reference convention: left pairs
// clockwise, right counter-clockwise, seen from above); nullopt for single chests.
constexpr std::optional<Direction> chest_partner(BlockState s) noexcept {
    if (!s.is(BlockType::chest)) return std::nullopt;
    if (s.level() == static_cast<std::uint8_t>(blocks::ChestType::left)) return contracts::clockwise(s.facing());
    if (s.level() == static_cast<std::uint8_t>(blocks::ChestType::right)) return contracts::counter_clockwise(s.facing());
    return std::nullopt;
}
constexpr std::uint8_t container_size(BlockType t) noexcept {
    return t == BlockType::hopper ? 5 : t == BlockType::chest ? 27 : t == BlockType::dispenser || t == BlockType::crafter ? 9
         : t == BlockType::furnace ? 3 : t == BlockType::brewing_stand ? 5 : 0;
}
// Whether a block state takes random ticks (random_ticks.cpp; checked
// against both official servers for every state): sections without such
// states are not random-ticked.
bool takes_random_ticks(BlockState s);
// Whether a state makes its section random-ticked: those that take random
// ticks, and any that holds water, which does nothing in its own (both
// official servers, 1.21.8: the picks advance through sections that hold
// only water, waterlogged blocks included).
bool ticks_section_randomly(BlockState s);
// Whether lava sets fire in the air beside a state (weathering.cpp; checked
// against both official servers for every state).
bool ignited_by_lava(BlockState s);
// Whether a state is a crafting table, which opens a crafting window, a
// stonecutter or a smithing table (menus.cpp).
bool is_crafting_table(BlockState s);
bool is_stonecutter(BlockState s);
bool is_smithing_table(BlockState s);
bool is_grindstone(BlockState s);
bool is_loom(BlockState s);
// Full collision cell for item entities. Blocks without rules of their own
// are solid when they have any collision shape (cell-level approximation).
inline bool is_solid(BlockState s) noexcept {
    switch (s.type()) {
    case BlockType::stone: case BlockType::glass: case BlockType::obsidian: case BlockType::redstone_block:
    case BlockType::redstone_lamp: case BlockType::observer: case BlockType::piston: case BlockType::sticky_piston:
    case BlockType::piston_head: case BlockType::slime_block: case BlockType::hopper: case BlockType::chest:
    case BlockType::moving_piston: case BlockType::target: case BlockType::honey_block: case BlockType::daylight_detector:
    case BlockType::note_block: case BlockType::dispenser: case BlockType::furnace: case BlockType::crafter:
    case BlockType::brewing_stand:
        return true;
    case BlockType::other: return other_has_collision(s);
    default:
        return false;
    }
}
// Lava, a registry block with fluid rules (nature.cpp), and whether a state
// is its source.
bool is_lava(BlockState s);
bool is_lava_source(BlockState s);
BlockState lava_source();
// Whether a block other than water holds water as a property (waterlogged):
// its water has fluid ticks of the block's type (nature.cpp).
bool waterlogged_block(BlockState s);
// How pistons treat a registry block without rules of its own: its push
// class (block_registry.hpp; observed on both official servers).
bool other_movable(BlockState s) noexcept;
bool other_destroyed_by_piston(BlockState s) noexcept;
bool other_push_only(BlockState s) noexcept;
// Whether a piston may move this state. Air is handled by the structure resolver.
inline bool is_movable(BlockState s) noexcept {
    switch (s.type()) {
    case BlockType::stone: case BlockType::glass: case BlockType::redstone_block: case BlockType::redstone_lamp:
    case BlockType::observer: case BlockType::slime_block: case BlockType::target: case BlockType::trapdoor:
    case BlockType::fence_gate: case BlockType::honey_block: case BlockType::note_block: case BlockType::rail:
        return true;
    case BlockType::piston: case BlockType::sticky_piston:
        return !s.flag();
    case BlockType::coral_fan: case BlockType::coral_wall_fan:
        return dead_coral(s);
    case BlockType::other:
        return other_movable(s);
    default:
        return false;
    }
}

// Blocks a piston destroys (with their drops) instead of moving or being blocked by.
constexpr bool destroyed_by_piston(BlockType t) noexcept {
    switch (t) {
    case BlockType::redstone_wire: case BlockType::lever: case BlockType::repeater: case BlockType::comparator:
    case BlockType::water: case BlockType::wheat: case BlockType::redstone_torch: case BlockType::redstone_wall_torch:
    case BlockType::button: case BlockType::pressure_plate: case BlockType::weighted_plate: case BlockType::door:
        return true;
    default:
        return false;
    }
}
// The same for any state, registry blocks by their push class; living coral
// fans break, dead ones move.
inline bool destroyed_by_piston(BlockState s) noexcept {
    if (is_coral_fan(s.type())) return !dead_coral(s);
    return s.is(BlockType::other) ? other_destroyed_by_piston(s) : destroyed_by_piston(s.type());
}
// Blocks pistons push but never pull (glazed terracotta).
inline bool push_only(BlockState s) noexcept { return s.is(BlockType::other) && other_push_only(s); }

// Number of internal registry states (block_registry.hpp), for iteration.
std::uint32_t registry_state_count() noexcept;
// The target versions' notation of a state ("minecraft:name[key=value,...]"
// with every property, in key order), and the internal registry state it
// names; nullopt for a state no version has.
std::string block_notation(BlockState s);
std::optional<std::uint32_t> registry_equivalent(BlockState s);

template <class F>
void for_each_modeled_state(F&& f) {
    for (std::uint32_t raw = 0; raw < (1u << 18); ++raw) {
        const auto type = static_cast<BlockType>(raw & 0xFF);
        if (type == BlockType::other || static_cast<std::uint8_t>(type) >= static_cast<std::uint8_t>(BlockType::count_)) continue;
        // Not every kind takes every state (powered rails do not curve; only they are powered).
        if (has_variant(type)) {
            for (std::uint32_t kind = 0; kind < variant_count(type); ++kind)
                if (const auto v = BlockState::decode(raw | kind << 18)) f(*v);
            continue;
        }
        const auto s = BlockState::decode(raw);
        if (!s) continue;
        if (s->is(BlockType::redstone_wire)) {
            for (std::uint32_t shape = 0; shape < 256; ++shape)
                if (const auto w = BlockState::decode(raw | (shape << 18))) f(*w);
        } else if (s->is(BlockType::note_block)) {
            for (std::uint8_t instrument = 0; instrument < note_instruments; ++instrument) f(s->with_variant(instrument));
        } else {
            f(*s);
        }
    }
}

template <class F>
void for_each_block_state(F&& f) {
    for_each_modeled_state(f);
    for (std::uint32_t id = 1; id <= registry_state_count(); ++id)
        if (const auto s = BlockState::decode(BlockState::registered(id).raw())) f(*s);
}

} // namespace circuit::gameplay
