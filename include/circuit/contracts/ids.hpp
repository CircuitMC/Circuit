// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/components.hpp>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

// Stable identities and coordinates shared by every module. Values are plain
// data: no type here owns or refers to mutable world storage.
namespace circuit::contracts {

enum class DimensionId : std::uint8_t { overworld = 0 };

// Axis convention: north = -z, south = +z, west = -x, east = +x.
enum class Direction : std::uint8_t { down, up, north, south, west, east };
inline constexpr std::array<Direction, 6> all_directions{
    Direction::down, Direction::up, Direction::north,
    Direction::south, Direction::west, Direction::east};
inline constexpr std::array<Direction, 4> horizontal_directions{
    Direction::north, Direction::south, Direction::west, Direction::east};

struct Offset { std::int32_t dx, dy, dz; };
constexpr Offset offset(Direction d) noexcept {
    switch (d) {
    case Direction::down: return {0, -1, 0};
    case Direction::up: return {0, 1, 0};
    case Direction::north: return {0, 0, -1};
    case Direction::south: return {0, 0, 1};
    case Direction::west: return {-1, 0, 0};
    case Direction::east: return {1, 0, 0};
    }
    return {0, 0, 0};
}
constexpr Direction opposite(Direction d) noexcept {
    switch (d) {
    case Direction::down: return Direction::up;
    case Direction::up: return Direction::down;
    case Direction::north: return Direction::south;
    case Direction::south: return Direction::north;
    case Direction::west: return Direction::east;
    case Direction::east: return Direction::west;
    }
    return d;
}
constexpr bool is_horizontal(Direction d) noexcept {
    return d != Direction::down && d != Direction::up;
}
// Rotation seen from above; vertical directions are unchanged.
constexpr Direction clockwise(Direction d) noexcept {
    switch (d) {
    case Direction::north: return Direction::east;
    case Direction::east: return Direction::south;
    case Direction::south: return Direction::west;
    case Direction::west: return Direction::north;
    default: return d;
    }
}
constexpr Direction counter_clockwise(Direction d) noexcept {
    switch (d) {
    case Direction::north: return Direction::west;
    case Direction::west: return Direction::south;
    case Direction::south: return Direction::east;
    case Direction::east: return Direction::north;
    default: return d;
    }
}

// Horizontal positions are bounded by the world border; derived neighbor
// coordinates therefore cannot overflow 32-bit arithmetic.
inline constexpr std::int32_t world_border = 30'000'000;

struct BlockPos {
    std::int32_t x{}, y{}, z{};
    [[nodiscard]] constexpr BlockPos relative(Direction d, std::int32_t n = 1) const noexcept {
        const auto o = offset(d);
        return {x + o.dx * n, y + o.dy * n, z + o.dz * n};
    }
    auto operator<=>(const BlockPos&) const = default;
};

struct ChunkPos {
    std::int32_t x{}, z{};
    auto operator<=>(const ChunkPos&) const = default;
};

// Arithmetic right shift is floor division for negative coordinates (C++20).
constexpr ChunkPos chunk_of(BlockPos p) noexcept { return {p.x >> 4, p.z >> 4}; }
constexpr bool within_border(BlockPos p) noexcept {
    return p.x >= -world_border && p.x < world_border && p.z >= -world_border && p.z < world_border;
}
// Index inside a 16x16x16 section: y-major, then z, then x.
constexpr std::uint32_t section_local_index(BlockPos p) noexcept {
    return (static_cast<std::uint32_t>(p.y & 15) << 8) | (static_cast<std::uint32_t>(p.z & 15) << 4)
        | static_cast<std::uint32_t>(p.x & 15);
}
constexpr BlockPos chunk_origin(ChunkPos c, std::int32_t y = 0) noexcept {
    return {c.x * 16, y, c.z * 16};
}
constexpr std::int32_t chebyshev_distance(ChunkPos a, ChunkPos b) noexcept {
    const auto dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    const auto dz = a.z > b.z ? a.z - b.z : b.z - a.z;
    return dx > dz ? dx : dz;
}

struct EntityId {
    std::uint64_t value{};
    auto operator<=>(const EntityId&) const = default;
};

// Lifetime counter for an object reachable through a reused key, such as a
// chunk column that unloads and reloads. Exhaustion is an explicit failure.
struct Generation {
    std::uint32_t value{};
    [[nodiscard]] constexpr std::optional<Generation> next() const noexcept {
        if (value == UINT32_MAX) return std::nullopt;
        return Generation{value + 1};
    }
    auto operator<=>(const Generation&) const = default;
};

// A generation-checked reference. Holding a handle never keeps the object
// alive; resolving a handle with a different generation must fail.
template <class Tag, class Key>
struct Handle {
    Key key{};
    Generation generation{};
    auto operator<=>(const Handle&) const = default;
};
struct ChunkTag {};
using ChunkHandle = Handle<ChunkTag, ChunkPos>;

// Fixed-point entity coordinates: one block is 4096 units. Integer arithmetic
// keeps entity motion identical across schedules, threads and compilers.
inline constexpr std::int64_t fixed_one = 4096;
struct FixedVec {
    std::int64_t x{}, y{}, z{};
    auto operator<=>(const FixedVec&) const = default;
};
// Precondition: the block coordinate fits in 32 bits. Untrusted positions are
// checked at full width (fixed_within_border, WorldShape::contains_fixed_y) first;
// narrowing an unchecked value would alias distant positions.
constexpr std::int32_t floor_block(std::int64_t fixed) noexcept {
    const auto q = fixed / fixed_one;
    return static_cast<std::int32_t>((fixed % fixed_one != 0 && fixed < 0) ? q - 1 : q);
}
constexpr BlockPos block_of(FixedVec v) noexcept {
    return {floor_block(v.x), floor_block(v.y), floor_block(v.z)};
}
inline constexpr std::int64_t fixed_border = std::int64_t{world_border} * fixed_one;
constexpr bool fixed_within_border(FixedVec v) noexcept {
    return v.x >= -fixed_border && v.x < fixed_border && v.z >= -fixed_border && v.z < fixed_border;
}

struct ItemStack {
    std::uint16_t item{};
    std::uint8_t count{};
    std::uint16_t damage{}; // durability used, for items that have durability
    ComponentPatch components{}; // every other data component (see components.hpp)
    [[nodiscard]] bool empty() const noexcept { return item == 0 || count == 0; }
    // Stacks of the same item, damage and components merge (up to their limit).
    [[nodiscard]] bool same_kind(const ItemStack& o) const noexcept {
        return item == o.item && damage == o.damage && components == o.components;
    }
    bool operator==(const ItemStack&) const = default;
    std::strong_ordering operator<=>(const ItemStack&) const = default;
};

// A player in the simulation: the profile UUID's two halves.
struct PlayerKey {
    std::uint64_t hi{}, lo{};
    auto operator<=>(const PlayerKey&) const = default;
};
// Per container slot, as in the target versions (since 1.20.5); items stack
// to their own limit, usually 64 or lower, unless a component raises it.
inline constexpr std::uint8_t max_stack_size = 99;

} // namespace circuit::contracts

template <>
struct std::hash<circuit::contracts::ChunkPos> {
    std::size_t operator()(const circuit::contracts::ChunkPos& c) const noexcept {
        return std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x)) << 32)
            | static_cast<std::uint32_t>(c.z));
    }
};
template <>
struct std::hash<circuit::contracts::BlockPos> {
    std::size_t operator()(const circuit::contracts::BlockPos& p) const noexcept {
        std::uint64_t h = static_cast<std::uint32_t>(p.x);
        h = h * 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint32_t>(p.y);
        h = h * 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint32_t>(p.z);
        return std::hash<std::uint64_t>{}(h);
    }
};
template <>
struct std::hash<circuit::contracts::EntityId> {
    std::size_t operator()(const circuit::contracts::EntityId& e) const noexcept {
        return std::hash<std::uint64_t>{}(e.value);
    }
};
