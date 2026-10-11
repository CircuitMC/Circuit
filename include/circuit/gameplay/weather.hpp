// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/commands.hpp>
#include <circuit/contracts/time.hpp>
#include <circuit/gameplay/rng.hpp>
#include <cstdint>

// The weather (minecraft.wiki "Weather" and "Commands/weather"; observed on
// both official servers with protocol clients and their saved level data).
//
// A rain flag and a thunder flag each have a timer. While the clear timer
// runs, both flags are off and their timers wait at 1; when it ends, both
// flags turn on (a thunderstorm). Otherwise a timer counts down and its flag
// toggles when it reaches 0; a timer at 0 draws its flag's next duration:
// rain on 12000-24000 ticks, off 12000-180000; thunder on 3600-15600, off
// 12000-180000. The timers run only while doWeatherCycle is on.
//
// The rain and thunder levels (0 to 1) follow their flags by 0.01 a tick in
// single precision, whatever the game rule; clients see rain while the rain
// level is above 0.2. Levels are not part of the official servers' saved
// data: every start sets them to their flags.
//
// The official servers draw durations from the level's random sequence;
// Circuit draws them from its own stream per tick (weather_stream), so the
// durations differ while their ranges are the same.
namespace circuit::gameplay {

struct Weather {
    std::int32_t clear_time{}, rain_time{}, thunder_time{};
    bool raining{}, thundering{};
    float rain_level{}, thunder_level{};

    // Whether clients see rain.
    [[nodiscard]] bool rain_visible() const noexcept { return rain_level > 0.2f; }
    bool operator==(const Weather&) const = default;
};

// The duration ranges, inclusive.
inline constexpr std::int32_t clear_min = 12000, clear_max = 180000;
inline constexpr std::int32_t rain_min = 12000, rain_max = 24000;
inline constexpr std::int32_t thunder_min = 3600, thunder_max = 15600;

// The stream the weather draws from in one tick: commands first, then the cycle.
constexpr Rng weather_stream(std::uint64_t seed, contracts::Tick tick) noexcept {
    return Rng::derive({seed, tick, 0x5745415448455221ULL});
}

// Valid state: timers not negative, levels within 0-1.
[[nodiscard]] bool valid_weather(const Weather& w) noexcept;

// /weather: clear sets the clear timer and turns both flags off with their
// timers at 0; rain and thunder set both timers and turn rain on, thunder
// only for thunder. A duration of 0 draws the kind's default.
void set_weather(Weather& w, const contracts::SetWeather& command, Rng& stream);

// One tick: the timers when `cycle`, then the levels.
void advance_weather(Weather& w, bool cycle, Rng& stream);

// A start: the levels take their flags' values.
void restart_weather(Weather& w) noexcept;

} // namespace circuit::gameplay
