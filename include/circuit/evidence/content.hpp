// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/block.hpp>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace circuit::evidence {

// Translation between target-version content notation and the reference
// registry. Properties the reference rules do not model (dust shape, lever
// mounting, chest facing) are accepted and ignored; values that the rules
// cannot represent (locked repeaters, double chests, moving pistons, unknown
// blocks) are reported as unsupported. The conventions below are unverified
// until harness traces confirm them; a wrong convention makes fixtures fail
// loudly rather than pass silently.
class ContentMapping {
public:
    // nullptr when no mapping exists for the game version.
    static const ContentMapping* for_game_version(std::string_view game_version) noexcept;

    [[nodiscard]] std::string_view game_version() const noexcept { return game_version_; }
    [[nodiscard]] std::expected<gameplay::BlockState, std::string> block(std::string_view notation) const;
    // Canonical notation with modeled properties only, in key order.
    [[nodiscard]] std::string notation(gameplay::BlockState state) const;
    [[nodiscard]] std::expected<std::uint16_t, std::string> item(std::string_view id) const;
    [[nodiscard]] std::string item_name(std::uint16_t item) const;

    explicit constexpr ContentMapping(std::string_view game_version) noexcept : game_version_(game_version) {}

private:
    std::string_view game_version_;
};

} // namespace circuit::evidence
