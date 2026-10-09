// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/block.hpp>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace circuit::evidence {

// Translation between target-version content notation and Circuit block
// states. Every block state of the target version has a state: the blocks
// with rules of their own map to their packed states, every other block to
// its registry state (gameplay/block_registry.hpp). Unknown blocks,
// properties and values are errors; unknown blocks are reported as
// unsupported.
class ContentMapping {
public:
    // nullptr when no mapping exists for the game version.
    static const ContentMapping* for_game_version(std::string_view game_version) noexcept;

    [[nodiscard]] std::string_view game_version() const noexcept { return game_version_; }
    [[nodiscard]] std::expected<gameplay::BlockState, std::string> block(std::string_view notation) const;
    // Canonical notation with every property, in key order.
    [[nodiscard]] std::string notation(gameplay::BlockState state) const;
    [[nodiscard]] std::expected<std::uint16_t, std::string> item(std::string_view id) const;
    [[nodiscard]] std::string item_name(std::uint16_t item) const;

    explicit constexpr ContentMapping(std::string_view game_version) noexcept : game_version_(game_version) {}

private:
    std::string_view game_version_;
};

} // namespace circuit::evidence
