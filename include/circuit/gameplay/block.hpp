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
// Blocks of several kinds that share a BlockType and keep the kind as their variant.
constexpr bool has_variant(BlockType t) noexcept {
    return t == BlockType::button || t == BlockType::pressure_plate || t == BlockType::weighted_plate || t == BlockType::door
        || t == BlockType::trapdoor || t == BlockType::fence_gate;
}
constexpr std::uint8_t variant_count(BlockType t) noexcept {
    switch (t) {
    case BlockType::weighted_plate: return 2;
    case BlockType::door: case BlockType::trapdoor: return blocks::door_kinds;
    case BlockType::fence_gate: return blocks::fence_gate_kinds;
    default: return has_variant(t) ? blocks::button_kinds : 1;
    }
}
constexpr bool is_opening(BlockType t) noexcept { return t == BlockType::door || t == BlockType::trapdoor || t == BlockType::fence_gate; }
constexpr bool is_torch(BlockType t) noexcept { return t == BlockType::redstone_torch || t == BlockType::redstone_wall_torch; }

// Whether an internal registry block has rules of its own (a BlockType other
// than `other`); its states never use the registry encoding.
bool modeled_block(std::uint32_t internal_block) noexcept;

// Coarse data of an `other` state, from the registry: whether the block has a
// collision shape, blocks all light, and covers a face completely.
bool other_has_collision(BlockState s) noexcept;
bool other_opaque(BlockState s) noexcept;
bool other_face_covered(BlockState s, Direction face) noexcept;

// Whether two states are of the same block (for `other`, the same registry block).
bool same_block(BlockState a, BlockState b) noexcept;

constexpr bool is_piston(BlockType t) noexcept { return t == BlockType::piston || t == BlockType::sticky_piston; }
constexpr bool repeater_locked(BlockState s) noexcept { return s.is(BlockType::repeater) && s.level() == 1; }
// Every valid state: the modeled states in raw order, then the `other` states
// in internal order.
template <class F>
void for_each_block_state(F&& f);
constexpr bool is_container(BlockType t) noexcept { return t == BlockType::hopper || t == BlockType::chest; }
// Block entities hold container slots, hopper cooldowns, comparator output signals and moving blocks.
constexpr bool has_block_entity(BlockType t) noexcept {
    return is_container(t) || t == BlockType::comparator || t == BlockType::moving_piston;
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
    return t == BlockType::hopper ? 5 : t == BlockType::chest ? 27 : 0;
}
// Blocks whose random tick can change something (see run_random_ticks).
constexpr bool reacts_to_random_ticks(BlockType t) noexcept { return t == BlockType::wheat; }
// Full collision cell for item entities. Blocks without rules of their own
// are solid when they have any collision shape (cell-level approximation).
inline bool is_solid(BlockState s) noexcept {
    switch (s.type()) {
    case BlockType::stone: case BlockType::glass: case BlockType::obsidian: case BlockType::redstone_block:
    case BlockType::redstone_lamp: case BlockType::observer: case BlockType::piston: case BlockType::sticky_piston:
    case BlockType::piston_head: case BlockType::slime_block: case BlockType::hopper: case BlockType::chest:
    case BlockType::moving_piston: case BlockType::target: case BlockType::honey_block:
        return true;
    case BlockType::other: return other_has_collision(s);
    default:
        return false;
    }
}
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
    case BlockType::fence_gate: case BlockType::honey_block:
        return true;
    case BlockType::piston: case BlockType::sticky_piston:
        return !s.flag();
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
// The same for any state, registry blocks by their push class.
inline bool destroyed_by_piston(BlockState s) noexcept {
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
    for (std::uint32_t raw = 0; raw < (1u << 18); ++raw)
        if (const auto s = BlockState::decode(raw)) {
            if (s->is(BlockType::other)) continue;
            if (s->is(BlockType::redstone_wire)) {
                for (std::uint32_t shape = 0; shape < 256; ++shape)
                    if (const auto w = BlockState::decode(raw | (shape << 18))) f(*w);
            } else if (has_variant(s->type())) {
                for (std::uint32_t kind = 0; kind < variant_count(s->type()); ++kind) f(s->with_variant(static_cast<std::uint8_t>(kind)));
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
