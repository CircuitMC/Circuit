// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/gameplay/block.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

// Maps as both official servers keep and show them (client/maps;
// minecraft.wiki "Map" and "Map item format").
namespace circuit::gameplay {

// A map shows 128 by 128 pixels; its scale (0 to 4) makes each pixel
// 1 << scale blocks across.
inline constexpr std::size_t map_side = 128;
inline constexpr std::size_t map_pixels = map_side * map_side;
inline constexpr std::uint8_t max_map_scale = 4;

// A player who held a map, with the number of ticks they explored it (its
// step); forgotten once they no longer carry it, as on both official
// servers, which do not save it.
struct MapHolder {
    contracts::PlayerKey player;
    std::uint32_t step{};
    bool operator==(const MapHolder&) const = default;
};

// A map's data (data/map_<id>.dat): the center of the area it shows, its
// scale and dimension, whether it is locked, whether it shows the players
// carrying it and also far off it, and its pixels' colors row by row (0:
// unexplored; else a base color times 4 plus its shade). Its holders are not
// saved in map files.
struct MapData {
    std::int32_t id{};
    std::int32_t x_center{}, z_center{};
    std::uint8_t scale{};
    contracts::DimensionId dimension{};
    bool locked{};
    bool tracking_position{true};
    bool unlimited_tracking{};
    std::vector<std::uint8_t> colors = std::vector<std::uint8_t>(map_pixels);
    std::vector<MapHolder> holders; // in the order they first explored it
    bool operator==(const MapData&) const = default;
};

// The center of the map of a scale whose area holds a coordinate: the areas
// make a grid of 128 << scale blocks with a corner at -64.
[[nodiscard]] std::int32_t map_center(double coordinate, std::uint8_t scale) noexcept;

// A marker on a map as its data packet carries it: the type (0 a player, 6 a
// player off the map), the position in half pixels from the center (-128 to
// 127) and the direction in sixteenths of a turn.
struct MapMarker {
    std::uint8_t type{};
    std::int8_t x{}, z{};
    std::uint8_t direction{};
    bool operator==(const MapMarker&) const = default;
};
inline constexpr std::uint8_t player_marker = 0;
inline constexpr std::uint8_t player_off_map_marker = 6;

// The marker of a player at `x`, `z` facing `yaw` on a map of their
// dimension: on the map within 63 pixels of its center; beside it, on its
// edge, while within 320 pixels on both axes; else none.
[[nodiscard]] std::optional<MapMarker> player_marker_on(const MapData& map, double x, double z, float yaw) noexcept;

// The data a cartography table makes: a map of the next scale over the area
// around the old one's center (unexplored), and a locked copy.
[[nodiscard]] MapData zoomed_map(const MapData& map);
[[nodiscard]] MapData locked_map(const MapData& map);

// Map colors (minecraft.wiki "Map item format"): base colors 0 to 61, each
// drawn in four shades; 0 (NONE) shows what lies beneath.
inline constexpr std::uint8_t map_none = 0;
inline constexpr std::uint8_t map_stone = 11;
inline constexpr std::uint8_t map_water = 12;

// How a held map draws a block state (tests/fixtures/maps/colors-*.txt, as
// both official servers draw every state): its base color, and whether it
// holds water (the depth of water under a surface counts it).
struct MapLook {
    std::uint8_t color{};
    bool water{};
    bool operator==(const MapLook&) const = default;
};
// The looks of every state of a release: a block's color, by one property
// for some (logs by their axis, beds by their part, wheat by its age). A
// state that holds water draws WATER unless its top bears a block whole (top
// slabs and trapdoors, copper grates); barriers always do, and rails,
// ladders, iron bars, chains and glass panes never.
class MapLooks {
public:
    explicit MapLooks(std::string_view release);
    [[nodiscard]] MapLook of(BlockState s) const;
    // A registry state's (registry::state_of), for checks against the observations.
    [[nodiscard]] MapLook of_registry(std::uint32_t state) const;

private:
    std::vector<MapLook> registry_;                       // by registry state
    std::unordered_map<std::uint32_t, MapLook> modeled_; // by raw state
};
// Built once per release.
const MapLooks& map_looks(std::string_view release);

} // namespace circuit::gameplay
