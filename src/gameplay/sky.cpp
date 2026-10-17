// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/gameplay/sky.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace circuit::gameplay {
namespace {
const std::array<float, 65536>& sines() {
    static const auto table = [] {
        std::array<float, 65536> out{};
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = static_cast<float>(std::sin(static_cast<double>(i) * std::numbers::pi * 2.0 / 65536.0));
        return out;
    }();
    return table;
}
constexpr float two_pi = static_cast<float>(std::numbers::pi) * 2.0f;
} // namespace

float time_of_day(std::int64_t day_time) noexcept {
    double d = static_cast<double>(day_time) / 24000.0 - 0.25;
    d -= std::floor(d);
    const double eased = 0.5 - std::cos(d * std::numbers::pi) / 2.0;
    return static_cast<float>(d * 2.0 + eased) / 3.0f;
}

float sun_angle(std::int64_t day_time) noexcept { return time_of_day(day_time) * two_pi; }

float table_cos(float angle) noexcept {
    const auto index = static_cast<std::int64_t>(angle * 10430.378f + 16384.0f);
    return sines()[static_cast<std::size_t>(index & 65535)];
}

float table_sin(float angle) noexcept {
    const auto index = static_cast<std::int64_t>(angle * 10430.378f);
    return sines()[static_cast<std::size_t>(index & 65535)];
}

std::uint8_t sky_darken(const Weather& weather, std::int64_t day_time) noexcept {
    const float rain = weather.rain_level, thunder = weather.thunder_level * rain;
    const double by_rain = 1.0 - static_cast<double>(rain * 5.0f) / 16.0;
    const double by_thunder = 1.0 - static_cast<double>(thunder * 5.0f) / 16.0;
    const double sun = 0.5 + 2.0 * std::clamp(static_cast<double>(table_cos(sun_angle(day_time))), -0.25, 0.25);
    return static_cast<std::uint8_t>((1.0 - sun * by_rain * by_thunder) * 11.0);
}

} // namespace circuit::gameplay
