// SPDX-License-Identifier: AGPL-3.0-only
// Maps' areas and markers as both official servers compute them (client/maps,
// both versions identical; minecraft.wiki "Map").
#include <circuit/gameplay/maps.hpp>
#include <cmath>

namespace circuit::gameplay {

std::int32_t map_center(double coordinate, std::uint8_t scale) noexcept {
    const std::int32_t size = static_cast<std::int32_t>(map_side) << scale;
    const auto cell = static_cast<std::int32_t>(std::floor((coordinate + 64.0) / size));
    return cell * size + size / 2 - 64;
}

std::optional<MapMarker> player_marker_on(const MapData& map, double x, double z, float yaw) noexcept {
    // Pixels from the center, in single precision; a marker's position is in
    // half pixels: twice the offset plus 0.5, truncated toward zero (0.8 and
    // -0.8 pixels give 2 and -1).
    const auto factor = static_cast<float>(1 << map.scale);
    const float rx = static_cast<float>(x - map.x_center) / factor;
    const float rz = static_cast<float>(z - map.z_center) / factor;
    const auto half = [](float r) { return static_cast<std::int8_t>(static_cast<std::int32_t>(static_cast<double>(r * 2.0f) + 0.5)); };
    if (rx >= -63.0f && rz >= -63.0f && rx <= 63.0f && rz <= 63.0f) {
        // The yaw wrapped to [-180, 180), 8 degrees further from 0, in whole
        // sixteenths of a turn toward 0 (yaws 12, 34 and -12 give 0, 1, 0).
        float wrapped = std::fmod(yaw, 360.0f);
        if (wrapped >= 180.0f) wrapped -= 360.0f;
        if (wrapped < -180.0f) wrapped += 360.0f;
        wrapped += wrapped < 0.0f ? -8.0f : 8.0f;
        const auto direction = static_cast<std::int32_t>(static_cast<double>(wrapped) * 16.0 / 360.0);
        return MapMarker{player_marker, half(rx), half(rz), static_cast<std::uint8_t>(direction & 15)};
    }
    if (std::abs(rx) < 320.0f && std::abs(rz) < 320.0f) {
        const auto edge = [&](float r) { return r <= -63.0f ? std::int8_t{-128} : r >= 63.0f ? std::int8_t{127} : half(r); };
        return MapMarker{player_off_map_marker, edge(rx), edge(rz), 0};
    }
    return std::nullopt;
}

MapData zoomed_map(const MapData& map) {
    MapData out = map;
    out.scale = static_cast<std::uint8_t>(map.scale + 1);
    out.x_center = map_center(map.x_center, out.scale);
    out.z_center = map_center(map.z_center, out.scale);
    out.locked = false;
    out.colors.assign(map_pixels, 0);
    return out;
}

MapData locked_map(const MapData& map) {
    MapData out = map;
    out.locked = true;
    return out;
}

} // namespace circuit::gameplay
