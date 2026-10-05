// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/errors.hpp>
#include <circuit/contracts/ids.hpp>
#include <cstdint>
#include <optional>

// Block registry of circuit-reference-rules-v0. These are Circuit's own rule
// definitions, written from documented behavior; they are not a vanilla registry.
namespace circuit::gameplay {
using contracts::Direction;

enum class BlockType : std::uint8_t {
    air = 0,
    stone,
    glass,
    obsidian,
    redstone_block,
    lever,          // flag: powered
    redstone_wire,  // level: power 0-15
    repeater,       // facing: output side (horizontal); delay 1-4; flag: powered
    redstone_lamp,  // flag: lit
    observer,       // facing: observed side; flag: powered (output at the back)
    piston,         // facing: push side; flag: extended
    sticky_piston,  // facing: push side; flag: extended
    piston_head,    // facing: same as its piston; flag: sticky
    slime_block,
    hopper,         // facing: output side (not up); flag: enabled
    chest,          // facing: front (horizontal); level: ChestType
    water,          // level 0: source, 1-7: flowing distance; flag: falling (level 0)
    wheat,          // level: age 0-7
    comparator,     // facing: output side (horizontal); level: 0 compare, 1 subtract; flag: powered
    moving_piston,  // facing: facing of the moving piston; flag: sticky. Block entity holds the moving block.
    count_,
    unloaded = 255, // Read sentinel for unloaded chunks and positions outside the world height.
};

// Packed state: bits 0-7 type, 8-10 facing, 11-14 level, 15 flag, 16-17 delay-1.
class BlockState {
public:
    constexpr BlockState() noexcept = default;
    static constexpr BlockState of(BlockType type) noexcept { return BlockState(static_cast<std::uint32_t>(type)); }
    static constexpr BlockState unloaded_sentinel() noexcept { return of(BlockType::unloaded); }
    // Validates a registry value received from a command or a record.
    static contracts::Result<BlockState> decode(std::uint32_t raw) noexcept;

    [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return raw_; }
    [[nodiscard]] constexpr BlockType type() const noexcept { return static_cast<BlockType>(raw_ & 0xFF); }
    [[nodiscard]] constexpr bool is(BlockType t) const noexcept { return type() == t; }
    [[nodiscard]] constexpr bool is_air() const noexcept { return type() == BlockType::air; }
    [[nodiscard]] constexpr bool is_unloaded() const noexcept { return type() == BlockType::unloaded; }
    [[nodiscard]] constexpr Direction facing() const noexcept { return static_cast<Direction>((raw_ >> 8) & 7); }
    [[nodiscard]] constexpr std::uint8_t level() const noexcept { return static_cast<std::uint8_t>((raw_ >> 11) & 15); }
    [[nodiscard]] constexpr bool flag() const noexcept { return (raw_ >> 15) & 1; }
    [[nodiscard]] constexpr std::uint8_t delay() const noexcept { return static_cast<std::uint8_t>(((raw_ >> 16) & 3) + 1); }

    [[nodiscard]] constexpr BlockState with_facing(Direction d) const noexcept {
        return BlockState((raw_ & ~(7u << 8)) | (static_cast<std::uint32_t>(d) << 8));
    }
    [[nodiscard]] constexpr BlockState with_level(std::uint8_t level) const noexcept {
        return BlockState((raw_ & ~(15u << 11)) | (static_cast<std::uint32_t>(level & 15) << 11));
    }
    [[nodiscard]] constexpr BlockState with_flag(bool value) const noexcept {
        return BlockState((raw_ & ~(1u << 15)) | (static_cast<std::uint32_t>(value) << 15));
    }
    [[nodiscard]] constexpr BlockState with_delay(std::uint8_t delay) const noexcept {
        return BlockState((raw_ & ~(3u << 16)) | (static_cast<std::uint32_t>((delay - 1) & 3) << 16));
    }
    constexpr bool operator==(const BlockState&) const noexcept = default;

private:
    explicit constexpr BlockState(std::uint32_t raw) noexcept : raw_(raw) {}
    std::uint32_t raw_{};
};

// Convenience constructors used by rules and fixtures.
namespace blocks {
constexpr BlockState air() { return BlockState::of(BlockType::air); }
constexpr BlockState stone() { return BlockState::of(BlockType::stone); }
constexpr BlockState glass() { return BlockState::of(BlockType::glass); }
constexpr BlockState obsidian() { return BlockState::of(BlockType::obsidian); }
constexpr BlockState redstone_block() { return BlockState::of(BlockType::redstone_block); }
constexpr BlockState slime() { return BlockState::of(BlockType::slime_block); }
constexpr BlockState lever(bool on = false) { return BlockState::of(BlockType::lever).with_flag(on); }
constexpr BlockState wire(std::uint8_t power = 0) { return BlockState::of(BlockType::redstone_wire).with_level(power); }
constexpr BlockState repeater(Direction output, std::uint8_t delay = 1, bool powered = false) {
    return BlockState::of(BlockType::repeater).with_facing(output).with_delay(delay).with_flag(powered);
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
} // namespace blocks

constexpr bool is_piston(BlockType t) noexcept { return t == BlockType::piston || t == BlockType::sticky_piston; }
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
// Full collision cell for item entities.
constexpr bool is_solid(BlockType t) noexcept {
    switch (t) {
    case BlockType::stone: case BlockType::glass: case BlockType::obsidian: case BlockType::redstone_block:
    case BlockType::redstone_lamp: case BlockType::observer: case BlockType::piston: case BlockType::sticky_piston:
    case BlockType::piston_head: case BlockType::slime_block: case BlockType::hopper: case BlockType::chest:
    case BlockType::moving_piston:
        return true;
    default:
        return false;
    }
}
// Whether a piston may move this state. Air is handled by the structure resolver.
constexpr bool is_movable(BlockState s) noexcept {
    switch (s.type()) {
    case BlockType::stone: case BlockType::glass: case BlockType::redstone_block: case BlockType::redstone_lamp:
    case BlockType::observer: case BlockType::slime_block:
        return true;
    case BlockType::piston: case BlockType::sticky_piston:
        return !s.flag();
    default:
        return false;
    }
}

// Blocks a piston destroys (without drops) instead of moving or being blocked by.
constexpr bool destroyed_by_piston(BlockType t) noexcept {
    switch (t) {
    case BlockType::redstone_wire: case BlockType::lever: case BlockType::repeater: case BlockType::comparator:
    case BlockType::water: case BlockType::wheat:
        return true;
    default:
        return false;
    }
}

// Item registry of the reference rules.
namespace items {
inline constexpr std::uint16_t stone = 1;
inline constexpr std::uint16_t redstone = 2;
inline constexpr std::uint16_t wheat = 3;
inline constexpr std::uint16_t iron_ingot = 4;
inline constexpr std::uint16_t lever = 5;
inline constexpr std::uint16_t repeater = 6;
inline constexpr std::uint16_t comparator = 7;
inline constexpr std::uint16_t wheat_seeds = 8;
inline constexpr std::uint16_t max_id = 8;
} // namespace items

} // namespace circuit::gameplay
