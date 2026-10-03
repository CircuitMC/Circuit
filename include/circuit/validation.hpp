// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>

namespace circuit::validation {

struct vec3 { double x; double y; double z; };

enum class error {
    invalid_configuration,
    non_finite_input,
    coordinate_out_of_bounds,
    velocity_limit_exceeded,
    displacement_limit_exceeded,
    negative_inventory_count,
    inventory_slot_limit_exceeded,
    inventory_overflow,
    inventory_not_conserved,
};

using result = std::expected<void, error>;

[[nodiscard]] inline bool is_finite(vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] inline bool valid_limit(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

// Limits are authoritative server inputs, never accepted from a client packet.
[[nodiscard]] inline result validate_position(vec3 position,
                                              double maximum_absolute_coordinate) noexcept {
    if (!valid_limit(maximum_absolute_coordinate))
        return std::unexpected(error::invalid_configuration);
    if (!is_finite(position)) return std::unexpected(error::non_finite_input);
    if (std::abs(position.x) > maximum_absolute_coordinate ||
        std::abs(position.y) > maximum_absolute_coordinate ||
        std::abs(position.z) > maximum_absolute_coordinate)
        return std::unexpected(error::coordinate_out_of_bounds);
    return {};
}

[[nodiscard]] inline result validate_velocity(vec3 velocity, double maximum_speed) noexcept {
    if (!valid_limit(maximum_speed)) return std::unexpected(error::invalid_configuration);
    if (!is_finite(velocity)) return std::unexpected(error::non_finite_input);
    const auto speed = std::hypot(velocity.x, velocity.y, velocity.z);
    if (!std::isfinite(speed) || speed > maximum_speed)
        return std::unexpected(error::velocity_limit_exceeded);
    return {};
}

// The caller computes the budget from server ticks, physics and permitted actions.
// Teleports and knockback need explicit authoritative transitions. This helper
// validates a Euclidean bound; it does not implement collision or complete anti-cheat.
[[nodiscard]] inline result validate_displacement(vec3 previous, vec3 proposed,
                                                  double maximum_displacement) noexcept {
    if (!valid_limit(maximum_displacement))
        return std::unexpected(error::invalid_configuration);
    if (!is_finite(previous) || !is_finite(proposed))
        return std::unexpected(error::non_finite_input);
    const vec3 difference{proposed.x - previous.x, proposed.y - previous.y,
                          proposed.z - previous.z};
    if (!is_finite(difference))
        return std::unexpected(error::displacement_limit_exceeded);
    const auto distance = std::hypot(difference.x, difference.y, difference.z);
    if (!std::isfinite(distance) || distance > maximum_displacement)
        return std::unexpected(error::displacement_limit_exceeded);
    return {};
}

[[nodiscard]] inline std::expected<std::int64_t, error>
inventory_total(std::span<const std::int64_t> counts, std::int64_t maximum_slot_count) noexcept {
    if (maximum_slot_count < 0) return std::unexpected(error::invalid_configuration);
    std::int64_t total = 0;
    for (const auto count : counts) {
        if (count < 0) return std::unexpected(error::negative_inventory_count);
        if (count > maximum_slot_count)
            return std::unexpected(error::inventory_slot_limit_exceeded);
        if (count > std::numeric_limits<std::int64_t>::max() - total)
            return std::unexpected(error::inventory_overflow);
        total += count;
    }
    return total;
}

// Run separately for each canonical item identity (including components/NBT),
// across all affected containers/entities. Count conservation alone cannot detect
// substituting item identities. The delta comes from server-authorized recipes,
// drops or other actions, never from a client-provided adjustment.
[[nodiscard]] inline result validate_inventory_conservation(
    std::span<const std::int64_t> before, std::span<const std::int64_t> after,
    std::int64_t maximum_slot_count, std::int64_t server_authorized_delta = 0) noexcept {
    const auto before_total = inventory_total(before, maximum_slot_count);
    if (!before_total) return std::unexpected(before_total.error());
    const auto after_total = inventory_total(after, maximum_slot_count);
    if (!after_total) return std::unexpected(after_total.error());
    // Both totals are in [0, INT64_MAX], so their difference cannot overflow.
    if (*after_total - *before_total != server_authorized_delta)
        return std::unexpected(error::inventory_not_conserved);
    return {};
}

} // namespace circuit::validation
