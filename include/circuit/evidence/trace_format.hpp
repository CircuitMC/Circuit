// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/contracts/time.hpp>
#include <circuit/contracts/version.hpp>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// circuit-trace v1: a line-oriented behavior fixture shared by Circuit and an
// external reference harness. A fixture with `?` placeholders is a skeleton:
// it specifies a scenario (area, actions, requested observations) that either
// side can record. Block states and items use target-version notation
// (`minecraft:repeater[delay=1,...]`); the content mapping translates them.
namespace circuit::evidence {
using contracts::BlockPos;
using contracts::Tick;

enum class ChangeMode : std::uint8_t { none, ordered, unordered };

// A value that may still be a `?` placeholder.
struct Observed {
    std::optional<std::string> value;
    [[nodiscard]] bool recorded() const noexcept { return value.has_value(); }
    bool operator==(const Observed&) const = default;
};

struct Action {
    enum class Kind : std::uint8_t { set, use, insert };
    Tick tick{};
    Kind kind{};
    BlockPos pos;
    std::string state;           // set
    std::uint8_t slot{};         // insert
    std::string item;            // insert
    std::uint8_t count{};        // insert
    std::size_t line{};
    bool operator==(const Action&) const = default;
};
struct Change {
    Tick tick{};
    BlockPos pos;
    std::string before, after;
    std::size_t line{};
    bool operator==(const Change&) const = default;
};
struct BlockProbe {
    Tick tick{};
    BlockPos pos;
    Observed state;
    std::size_t line{};
    bool operator==(const BlockProbe&) const = default;
};
struct SlotProbe {
    Tick tick{};
    BlockPos pos;
    std::uint8_t slot{};
    Observed item;               // "empty" or an item id
    std::uint8_t count{};
    std::size_t line{};
    bool operator==(const SlotProbe&) const = default;
};

struct Fixture {
    std::string title;
    std::string game_version;
    contracts::EvidenceSource source{};
    std::string harness;         // Build identity of the recording harness, or "-".
    std::vector<std::string> mechanisms;
    BlockPos area_min, area_max; // Inclusive.
    Tick ticks{};                // Scenario ticks after the setup tick 0.
    std::uint32_t random_tick_speed{};
    std::uint32_t neighbor_update_limit{}; // Chained neighbor update limit; 0 keeps the bundle's limit.
    ChangeMode changes{ChangeMode::none};
    std::vector<Action> actions;
    std::vector<Change> recorded_changes;
    std::vector<BlockProbe> block_probes;
    std::vector<SlotProbe> slot_probes;

    [[nodiscard]] bool contains(BlockPos p) const noexcept {
        return p.x >= area_min.x && p.x <= area_max.x && p.y >= area_min.y && p.y <= area_max.y && p.z >= area_min.z
            && p.z <= area_max.z;
    }
    [[nodiscard]] bool is_skeleton() const noexcept;
    bool operator==(const Fixture&) const = default;
};

struct FormatLimits {
    std::size_t max_bytes = 16u << 20;
    std::size_t max_lines = 200'000;
    std::size_t max_line_length = 4096;
    Tick max_ticks = 10'000;
    std::int32_t max_horizontal_extent = 128;
    std::int32_t max_vertical_extent = 384;
};

struct FormatError {
    std::size_t line{}; // 1-based; 0 for whole-file errors
    std::string message;
};

std::expected<Fixture, FormatError> parse_fixture(std::string_view text, const FormatLimits& limits = {});
// Canonical text: header in fixed order, then actions, changes and probes by tick.
std::string write_fixture(const Fixture& fixture);

std::string_view source_name(contracts::EvidenceSource source) noexcept;

// Block-state notation `namespace:id[key=value,...]`.
struct StateSpec {
    std::string id;
    std::vector<std::pair<std::string, std::string>> properties;
};
std::expected<StateSpec, std::string> parse_state(std::string_view text);

} // namespace circuit::evidence
