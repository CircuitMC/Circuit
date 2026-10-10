// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/errors.hpp>
#include <circuit/contracts/ids.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// A version bundle binds everything that must agree for one server process:
// protocol candidate, content schema and registries, rules, RNG policy,
// generator identity, world shape, limits and save conversion. Selecting a
// bundle for simulation is distinct from listing a candidate, and neither
// claims vanilla compatibility.
namespace circuit::contracts {

enum class CompatibilityTier : std::uint8_t {
    l0_exact,               // state, event order and RNG consumption match the target version
    l1_region_equivalent,   // exact within a region; cross-region invariants only
    l2_declared_deviation,  // Circuit's own definition; known or uncompared differences are listed
};
enum class EvidenceSource : std::uint8_t {
    none = 0,
    documented_behavior = 1,   // public descriptions; not checked against the target version
    harness_trace = 2,         // checked against traces from an instrumented target-version server
    controlled_experiment = 3,
    circuit_reference = 4,     // recorded from Circuit's own reference rules; never compatibility evidence
};

// One row of the compatibility table. `target` is the intended tier; `claimed`
// is what the evidence currently supports (nullopt: nothing is claimed).
struct MechanismEntry {
    std::string_view id;
    CompatibilityTier target;
    std::optional<CompatibilityTier> claimed;
    EvidenceSource evidence;
    std::string_view limitation;
};

struct WorldShape {
    std::int32_t min_y{};
    std::int32_t height{}; // Multiple of 16.
    [[nodiscard]] constexpr std::int32_t sections() const noexcept { return height / 16; }
    [[nodiscard]] constexpr bool contains_y(std::int32_t y) const noexcept {
        return y >= min_y && y < min_y + height;
    }
    // Full-width check of a fixed-point entity coordinate.
    [[nodiscard]] constexpr bool contains_fixed_y(std::int64_t y) const noexcept {
        return y >= std::int64_t{min_y} * fixed_one && y < (std::int64_t{min_y} + height) * fixed_one;
    }
};

struct RuleLimits {
    std::uint32_t max_chained_neighbor_updates{};  // version rule: truncation is behavior
    std::uint32_t max_scheduled_ticks_per_list{};  // version rule: excess stays scheduled
    std::uint32_t random_tick_speed{};             // version rule (default game rule value)
    std::uint32_t max_block_events_per_tick{};     // Circuit capacity: exceeding it fails the tick
    std::uint8_t max_ticket_radius{};              // Circuit capacity: ticking radius
    std::uint8_t max_load_radius{};                // Circuit capacity: loaded, non-ticking radius
};

enum class RngPolicy : std::uint8_t {
    // Random ticks draw from a stream derived from (seed, tick, chunk, section).
    // Declared L1 relative to a level-wide stream; see the compatibility table.
    per_section_derived_stream,
};

struct RuleSet {
    std::string_view id;
    RngPolicy rng;
    std::span<const MechanismEntry> mechanisms;
    // Block registry whose encoding the rules' block states use (gameplay/block_registry.hpp).
    std::string_view block_registry;
    // Why this version runs these rules: the evidence that the version's
    // behavior matches the cases the rules were checked against, and the
    // differences the bundle handles itself.
    std::string_view basis;
};

enum class GameRuleType : std::uint8_t { boolean, integer };
struct GameRule {
    std::string_view name;
    GameRuleType type;
    std::int32_t default_value; // booleans: 0 or 1
    bool experimental;          // only with an experimental profile
};

// What a version's content and data look like.
struct VersionSchema {
    // Release whose blocks and items the version has (gameplay/block_registry.hpp).
    std::string_view content_release;
    // Protocol whose item component schema stacks use (protocol/components.hpp).
    std::int32_t component_schema{};
    std::int32_t data_pack_format{};
    std::int32_t resource_pack_format{};
    // The synchronized registry set the server sends (server registries).
    std::string_view registries;
    // SHA-1 of the official server jar, whose built-in data pack an operator
    // may load (compatibility/versions/ records where it was verified).
    std::string_view server_jar_sha1;
    std::span<const GameRule> game_rules;
    [[nodiscard]] const GameRule* game_rule(std::string_view name) const noexcept {
        for (const auto& r : game_rules)
            if (r.name == name) return &r;
        return nullptr;
    }
};

struct VersionBundle {
    std::string_view id;
    std::string_view game_version;
    std::string_view protocol_candidate;
    std::optional<RuleSet> rules; // Absent: listed candidate that cannot execute.
    VersionSchema schema;
    WorldShape world; // the overworld
    // The Nether and the End: their dimension types' build height in both
    // target versions (minecraft.wiki "Dimension type").
    WorldShape nether{0, 256};
    WorldShape end{0, 256};
    [[nodiscard]] constexpr const WorldShape& shape(DimensionId d) const noexcept {
        return d == DimensionId::the_nether ? nether : d == DimensionId::the_end ? end : world;
    }
    RuleLimits limits;
    std::string_view generator;   // Identity of the chunk generator.
    std::int32_t protocol;        // Play protocol number; 0: no play codec.
    std::int32_t data_version;    // Vanilla save DataVersion of the game version; 0: unknown.
    bool save_conversion;         // Vanilla import/export available.
    bool play_supported;          // A real client can play.
};

class BundleRegistry {
public:
    static std::span<const VersionBundle> candidates() noexcept;
    // Succeeds only for a bundle with an executable rule set.
    static Result<const VersionBundle*> select_for_simulation(std::string_view id) noexcept;
    static std::optional<const VersionBundle*> find(std::string_view id) noexcept;
};

constexpr std::string_view tier_name(CompatibilityTier t) noexcept {
    switch (t) {
    case CompatibilityTier::l0_exact: return "L0";
    case CompatibilityTier::l1_region_equivalent: return "L1";
    case CompatibilityTier::l2_declared_deviation: return "L2";
    }
    return "?";
}

} // namespace circuit::contracts
