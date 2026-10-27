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
    // Whether an entity's height lies within the world.
    [[nodiscard]] constexpr bool contains_position_y(double y) const noexcept {
        return y >= min_y && y < static_cast<double>(min_y) + height;
    }
};

struct RuleLimits {
    std::uint32_t max_chained_neighbor_updates{};  // version rule: truncation is behavior
    std::uint32_t max_scheduled_ticks_per_list{};  // version rule: excess stays scheduled
    std::uint32_t max_block_events_per_tick{};     // Circuit capacity: exceeding it fails the tick
    std::uint8_t max_ticket_radius{};              // Circuit capacity: a ticket's square (a view distance)
    std::uint8_t max_load_radius{};                // Circuit capacity: how far a ticket's chunks reach
    std::uint8_t max_simulation_distance{};        // Circuit capacity: a player's simulation distance
};

enum class RngPolicy : std::uint8_t {
    // Random ticks draw from the level random and the level's position
    // generator, chunk by chunk in the target versions' order (fitted to the
    // official 1.21.8 server; gameplay::run_random_ticks).
    level_random_in_target_order,
};

// Which chunks random ticks run in (observed on both official servers,
// 2026-10-11).
enum class RandomTickRange : std::uint8_t {
    // Simulation level 31 or lower, whoever is near (1.21.8).
    entity_ticking,
    // Simulation level 32 or lower, with a non-spectator player less than 128
    // blocks horizontally from the chunk's center (1.21.4).
    block_ticking_near_player,
};

// When lava reaches an entity (observed on both official servers,
// 2026-10-25): an item in lava is hurt and an arrow set burning.
enum class LavaContact : std::uint8_t {
    // After each move, where its box is then, before an arrow's fire counts
    // down (1.21.8).
    after_move,
    // At the start of each of an item's ticks but its first, where it is
    // then; an arrow after its move and its fire's count (1.21.4).
    tick_start,
};

// How experience orbs move (observed on both official servers, 2026-10-26).
enum class OrbMotion : std::uint8_t {
    // Their vertical drag is in single precision, and an orb on the ground
    // bounces back at 0.4 of its speed only when it fell faster than gravity
    // alone makes it fall in a tick; else it rests (1.21.8).
    settling,
    // Their vertical drag is in double precision, and an orb on the ground
    // always bounces back at 0.4 of its speed (1.21.4).
    bouncing,
};

// The fall a falling anvil wears by (observed on both official servers,
// 2026-10-26, anvils_falling_far).
enum class FallDistance : std::uint8_t {
    // Every downward move counts, the landing one too, in double precision
    // (1.21.8).
    with_landing,
    // The downward moves before the landing tick count, in single precision
    // (1.21.4).
    before_landing,
};

// A level with neither players nor forced chunks stops ticking its entities
// and block entities after this many ticks (both versions; minecraft.wiki
// "Chunk", observed on both official servers).
inline constexpr std::uint32_t idle_level_ticks = 300;

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
    RandomTickRange random_ticks{};
    LavaContact lava{};
    OrbMotion orbs{};
    FallDistance falls{};
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
