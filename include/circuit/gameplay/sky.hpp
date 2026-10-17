// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/weather.hpp>
#include <cstdint>

// The overworld sky as daylight detectors read it (minecraft.wiki "Daylight
// cycle", "Light" and "Daylight Detector"; checked against both official
// servers' traces through a day, rain and thunder).
//
// The time of day runs from 0 to 1 over the 24000 ticks of a day, slower
// around noon and midnight: with d the fraction of the day since 6000,
// (2d + (1 - cos(pi d)) / 2) / 3 in single precision. The sun's angle is that
// fraction of a full turn. The sky darkens (0 to 11) as the sun sets and
// further by rain and thunder: with c the cosine of the sun's angle held to
// -0.25..0.25, 11 (1 - (0.5 + 2c)(1 - 5 rain / 16)(1 - 5 thunder rain / 16)),
// rounded toward zero. Cosines come from a table of 65536 single-precision
// sines, as the official servers compute them.
namespace circuit::gameplay {

[[nodiscard]] float time_of_day(std::int64_t day_time) noexcept;
[[nodiscard]] float sun_angle(std::int64_t day_time) noexcept;
// The cosine of a single-precision angle, from the sine table.
[[nodiscard]] float table_cos(float angle) noexcept;
[[nodiscard]] std::uint8_t sky_darken(const Weather& weather, std::int64_t day_time) noexcept;

} // namespace circuit::gameplay
