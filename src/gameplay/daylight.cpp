// SPDX-License-Identifier: AGPL-3.0-only
// Daylight detectors, as both official servers run them (checked against
// their traces; minecraft.wiki "Daylight Detector").
//
// In the overworld a detector's block entity sets its output on every tick
// whose game time is a multiple of 20: from the sky light in its cell less
// the sky's darkening (gameplay/sky.hpp), an inverted detector gives 15 less
// that; a normal one, when it is positive, that times the cosine of the sun's
// angle (at the current day time; the darkening comes from the day time
// before this tick advanced it) moved a fifth of the way toward noon,
// rounded. Using a detector inverts it quietly and sets its output at once.
// An output change updates the detector's neighbors; the detector powers
// them weakly.
#include "internal.hpp"
#include <circuit/gameplay/sky.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace circuit::gameplay::detail {
namespace {
void update_output(RuleContext& ctx, BlockPos pos, BlockState s) {
    // Other levels have no sky light, and their own sky.
    const bool overworld = ctx.dimension() == contracts::DimensionId::overworld;
    const auto sky = ctx.sky();
    int signal = (overworld ? ctx.sky_light(pos) : 0) - (overworld ? sky.darken : 0);
    if (s.flag()) {
        signal = 15 - signal;
    } else if (signal > 0) {
        constexpr float pi = static_cast<float>(std::numbers::pi), two_pi = pi * 2.0f;
        float angle = sun_angle(sky.day_time);
        const float noon = angle < pi ? 0.0f : two_pi;
        angle += (noon - angle) * 0.2f;
        // Rounded half up, exactly (the product is a float).
        const float scaled = static_cast<float>(signal) * table_cos(angle);
        signal = static_cast<int>(std::floor(static_cast<double>(scaled) + 0.5));
    }
    const auto output = static_cast<std::uint8_t>(std::clamp(signal, 0, 15));
    if (output != s.level()) set_block(ctx, pos, s.with_level(output));
}
} // namespace

void tick_daylight_detector(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (ctx.dimension() == contracts::DimensionId::overworld && ctx.now() % 20 == 0) update_output(ctx, pos, s);
}

void use_daylight_detector(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto inverted = s.with_flag(!s.flag());
    set_block_quiet(ctx, pos, inverted);
    update_output(ctx, pos, inverted);
}

} // namespace circuit::gameplay::detail
