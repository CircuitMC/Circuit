// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/validation.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using namespace circuit::validation;
int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}

void expect_error(result value, error expected, const char* name) {
    check(!value && value.error() == expected, name);
}
} // namespace

int main() {
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto infinity = std::numeric_limits<double>::infinity();
    const auto huge = std::numeric_limits<double>::max();
    check(validate_position({-30, 0, 30}, 30).has_value(), "coordinate inclusive bounds");
    expect_error(validate_position({30.1, 0, 0}, 30), error::coordinate_out_of_bounds,
                 "coordinate outside configured bound");
    expect_error(validate_position({0, nan, 0}, 30), error::non_finite_input, "NaN coordinate");
    expect_error(validate_position({0, 0, infinity}, 30), error::non_finite_input,
                 "infinite coordinate");
    expect_error(validate_position({0, 0, 0}, nan), error::invalid_configuration,
                 "NaN coordinate configuration");
    expect_error(validate_position({0, 0, 0}, -1), error::invalid_configuration,
                 "negative coordinate configuration");
    check(validate_velocity({3, 4, 0}, 5).has_value(), "velocity magnitude boundary");
    expect_error(validate_velocity({4, 4, 0}, 5), error::velocity_limit_exceeded,
                 "diagonal velocity exceeds magnitude budget");
    expect_error(validate_velocity({0, nan, 0}, 5), error::non_finite_input,
                 "NaN velocity");
    expect_error(validate_velocity({0, 0, 0}, infinity), error::invalid_configuration,
                 "unbounded speed configuration rejected");
    expect_error(validate_velocity({huge, huge, huge}, huge), error::velocity_limit_exceeded,
                 "velocity norm overflow fails closed");
    check(validate_displacement({1, 2, 3}, {4, 6, 3}, 5).has_value(),
          "movement boundary");
    check(validate_displacement({1, 2, 3}, {1, 2, 3}, 0).has_value(), "zero movement budget");
    expect_error(validate_displacement({0, 0, 0}, {0, 0, 0.1}, 0),
                 error::displacement_limit_exceeded, "movement without budget");
    expect_error(validate_displacement({-huge, 0, 0}, {huge, 0, 0}, huge),
                 error::displacement_limit_exceeded, "subtraction overflow fails closed");
    expect_error(validate_displacement({nan, 0, 0}, {0, 0, 0}, 5),
                 error::non_finite_input, "invalid previous authoritative position");
    expect_error(validate_displacement({0, 0, 0}, {infinity, 0, 0}, 5),
                 error::non_finite_input, "invalid proposed position");
    expect_error(validate_displacement({0, 0, 0}, {0, 0, 0}, -1),
                 error::invalid_configuration, "negative movement budget");

    constexpr std::array<std::int64_t, 2> original{32, 32};
    constexpr std::array<std::int64_t, 2> moved{0, 64};
    constexpr std::array<std::int64_t, 2> created{1, 64};
    constexpr std::array<std::int64_t, 2> removed{0, 63};
    constexpr std::array<std::int64_t, 1> negative{-1};
    constexpr std::array<std::int64_t, 1> oversized{65};
    constexpr auto max_count = std::numeric_limits<std::int64_t>::max();
    constexpr std::array<std::int64_t, 2> overflowing{max_count, 1};
    constexpr std::array<std::int64_t, 1> maximum{max_count};
    constexpr std::array<std::int64_t, 0> empty{};
    check(validate_inventory_conservation(original, moved, 64).has_value(), "item transfer");
    expect_error(validate_inventory_conservation(original, created, 64),
                 error::inventory_not_conserved, "unauthorized creation");
    expect_error(validate_inventory_conservation(original, removed, 64),
                 error::inventory_not_conserved, "unauthorized removal");
    check(validate_inventory_conservation(original, created, 64, 1).has_value(),
          "authorized creation");
    check(validate_inventory_conservation(original, removed, 64, -1).has_value(),
          "authorized consumption");
    expect_error(validate_inventory_conservation(original, negative, 64),
                 error::negative_inventory_count, "negative slot count");
    expect_error(validate_inventory_conservation(original, oversized, 64),
                 error::inventory_slot_limit_exceeded, "slot overflow");
    expect_error(validate_inventory_conservation(overflowing, overflowing, max_count),
                 error::inventory_overflow, "aggregate overflow");
    expect_error(validate_inventory_conservation(original, moved, -1),
                 error::invalid_configuration, "negative slot limit");
    check(validate_inventory_conservation(empty, empty, 0).has_value(), "empty inventory");
    check(validate_inventory_conservation(maximum, empty, max_count, -max_count).has_value(),
          "largest safe negative delta");
    expect_error(validate_inventory_conservation(empty, maximum, max_count,
                                                std::numeric_limits<std::int64_t>::min()),
                 error::inventory_not_conserved, "extreme delta cannot wrap");

    if (failures != 0) return EXIT_FAILURE;
    std::cout << "validation tests passed\n";
    return EXIT_SUCCESS;
}
