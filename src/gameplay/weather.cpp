// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/gameplay/weather.hpp>
#include <algorithm>
#include <cmath>

namespace circuit::gameplay {
namespace {

std::int32_t draw(Rng& stream, std::int32_t min, std::int32_t max) {
    return min + static_cast<std::int32_t>(stream.bounded(static_cast<std::uint32_t>(max - min + 1)));
}

// A flag's timer: counts down and toggles the flag at 0; at 0 it draws the
// flag's next duration instead.
void step(std::int32_t& time, bool& flag, Rng& stream, std::int32_t on_min, std::int32_t on_max) {
    if (time > 0) {
        if (--time == 0) flag = !flag;
    } else {
        time = flag ? draw(stream, on_min, on_max) : draw(stream, clear_min, clear_max);
    }
}

// A level moves toward its flag by 0.01 in single precision, within 0-1.
void follow(float& level, bool flag) { level = std::clamp(level + (flag ? 0.01f : -0.01f), 0.0f, 1.0f); }

} // namespace

bool valid_weather(const Weather& w) noexcept {
    const auto level = [](float v) { return std::isfinite(v) && v >= 0.0f && v <= 1.0f; };
    return w.clear_time >= 0 && w.rain_time >= 0 && w.thunder_time >= 0 && level(w.rain_level) && level(w.thunder_level);
}

void set_weather(Weather& w, const contracts::SetWeather& command, Rng& stream) {
    using contracts::WeatherKind;
    auto duration = command.duration;
    if (duration == 0) {
        duration = command.kind == WeatherKind::clear  ? draw(stream, clear_min, clear_max)
                 : command.kind == WeatherKind::rain   ? draw(stream, rain_min, rain_max)
                                                       : draw(stream, thunder_min, thunder_max);
    }
    if (command.kind == WeatherKind::clear) {
        w.clear_time = duration;
        w.rain_time = w.thunder_time = 0;
        w.raining = w.thundering = false;
    } else {
        w.clear_time = 0;
        w.rain_time = w.thunder_time = duration;
        w.raining = true;
        w.thundering = command.kind == WeatherKind::thunder;
    }
}

void advance_weather(Weather& w, bool cycle, Rng& stream) {
    if (cycle) {
        if (w.clear_time > 0) {
            --w.clear_time;
            w.rain_time = w.thunder_time = 1;
            w.raining = w.thundering = false;
        } else {
            step(w.thunder_time, w.thundering, stream, thunder_min, thunder_max);
            step(w.rain_time, w.raining, stream, rain_min, rain_max);
        }
    }
    follow(w.rain_level, w.raining);
    follow(w.thunder_level, w.thundering);
}

void restart_weather(Weather& w) noexcept {
    w.rain_level = w.raining ? 1.0f : 0.0f;
    w.thunder_level = w.thundering ? 1.0f : 0.0f;
}

} // namespace circuit::gameplay
