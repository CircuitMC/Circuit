// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/contracts/time.hpp>
#include <circuit/contracts/version.hpp>
#include <cstdint>
#include <expected>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// circuit-trace: a line-oriented behavior fixture shared by Circuit and an
// external reference harness. A fixture with `?` placeholders is a skeleton:
// it specifies a scenario (area, actions, requested observations) that either
// side can record. Block states and items use target-version notation
// (`minecraft:repeater[delay=1,...]`); the content mapping translates them.
// Version 2 adds the stage of every event, entity lifecycle and state
// observations in double precision, chunk transitions and random-number
// draws; version 1 files keep their meaning (docs/TRACE_FORMAT.md).
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

// A position or velocity in blocks (per tick), as fixtures write doubles.
using Vec3d = std::array<double, 3>;
// `key=value` observations of an entity, in the order written.
using Properties = std::vector<std::pair<std::string, std::string>>;

struct Action {
    enum class Kind : std::uint8_t { set, use, insert, spawn_item, command, break_block };
    Tick tick{};
    Kind kind{};
    BlockPos pos;                // set, use, insert; the block of `position` for spawn_item
    std::string state;           // set
    std::uint8_t slot{};         // insert
    std::string item;            // insert, spawn_item, break_block
    std::uint8_t count{};        // insert, spawn_item
    Vec3d position{}, velocity{};// spawn_item (v2)
    std::uint16_t pickup_delay{}; // spawn_item: optional `pickup-delay=<n>`, 0 to 32767
    std::int32_t age{};           // spawn_item: optional `age=<n>`, -32768 to 32767
    std::string command;         // command (v2): a server command without the slash, words separated by one space
    // break_block (v2): a survival player holding `item` ("minecraft:air" for
    // an empty hand) with these enchantments (namespaced ids, levels 1 to 255)
    // breaks the block at `pos`.
    std::vector<std::pair<std::string, std::uint8_t>> enchantments;
    std::size_t line{};
    bool operator==(const Action&) const = default;
};
struct Change {
    Tick tick{};
    std::string stage;           // v2: the stage the change happened in; empty in v1
    BlockPos pos;
    std::string before, after;
    std::size_t seq{};           // position among the tick's events (v2)
    std::size_t line{};
    bool operator==(const Change&) const = default;
};

// v2 observations. Entities are named by handles e1, e2, ... in the order
// they were added; stages are the names in `trace_stages`.
struct EntityEvent {
    enum class Kind : std::uint8_t { added, removed, changed, loaded };
    Tick tick{};
    std::string stage;
    Kind kind{};
    std::string handle;
    std::string type;            // added, loaded
    Vec3d position{}, velocity{};// added, loaded: at that moment
    Properties properties;       // added, changed, loaded
    std::string reason;          // removed
    std::size_t seq{};
    std::size_t line{};
    bool operator==(const EntityEvent&) const = default;
};
struct EntityProbe {             // a live observed entity at the end of a tick
    Tick tick{};
    std::string handle;
    Vec3d position{}, velocity{};
    Properties properties;
    std::size_t line{};
    bool operator==(const EntityProbe&) const = default;
};
struct EntityOrder {             // the order entities ticked in a stage
    Tick tick{};
    std::string stage;
    std::vector<std::string> handles;
    std::size_t line{};
    bool operator==(const EntityOrder&) const = default;
};
struct ChunkEvent {              // a chunk reaching a status
    Tick tick{};
    std::string stage;
    std::int32_t x{}, z{};
    std::string status;          // entity_ticking, block_ticking, border or unloaded
    std::size_t seq{};
    std::size_t line{};
    bool operator==(const ChunkEvent&) const = default;
};
struct RngObservation {          // draws from one random stream in one stage
    Tick tick{};
    std::string stage;
    std::string stream;
    std::uint64_t draws{};
    std::size_t line{};
    bool operator==(const RngObservation&) const = default;
};

// Stage names of v2 lines: Circuit's phases, then stages only the target
// version has. `-` marks an unknown stage.
inline constexpr std::array<std::string_view, 12> trace_stages{
    "commands", "block_ticks", "fluid_ticks", "random_ticks", "block_events", "entities",
    "block_entities", "players", "chunks", "spawning", "other", "-"};
inline constexpr std::array<std::string_view, 4> chunk_statuses{"entity_ticking", "block_ticking", "border", "unloaded"};
struct BlockProbe {
    Tick tick{};
    BlockPos pos;
    Observed state;
    std::size_t line{};
    bool operator==(const BlockProbe&) const = default;
};
struct LightProbe {             // v2: block light and sky light at the end of a tick
    Tick tick{};
    BlockPos pos;
    std::optional<std::pair<std::uint8_t, std::uint8_t>> levels; // block, sky; nullopt: `?`
    std::size_t line{};
    bool operator==(const LightProbe&) const = default;
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
    std::uint32_t format{1};     // circuit-trace version: 1 or 2
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
    // v2 header: the scenario's dimension, which observations are recorded,
    // and the tolerance for entity positions and velocities (0: exact).
    std::string dimension{"minecraft:overworld"};
    bool observe_entities{};
    bool observe_chunks{};
    bool observe_rng{};
    double tolerance{};
    // v2: game rules set before the scenario starts, by name, in file order.
    std::vector<std::pair<std::string, std::string>> gamerules;
    // v2: whether the area's chunks are force-loaded for the run (`load
    // area`, the default) or nothing is (`load none`: the actions load what
    // they need), and the chunks whose statuses are observed (`observe x0 z0
    // x1 z1`, inclusive chunk coordinates; all when absent).
    bool load_area{true};
    std::optional<std::array<std::int32_t, 4>> observe_window;
    std::vector<Action> actions;
    std::vector<Change> recorded_changes;
    std::vector<BlockProbe> block_probes;
    std::vector<LightProbe> light_probes;
    std::vector<SlotProbe> slot_probes;
    std::vector<EntityEvent> entity_events;
    std::vector<EntityProbe> entity_probes;
    std::vector<EntityOrder> entity_orders;
    std::vector<ChunkEvent> chunk_events;
    std::vector<RngObservation> rng;

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
// Canonical text: header in fixed order, then actions, events and probes by
// tick. A version-1 fixture is written exactly as before version 2 existed.
std::string write_fixture(const Fixture& fixture);
// Shortest decimal text that reads back as the same double.
std::string format_double(double value);

// The first observation on which two recordings of the same scenario differ,
// described with both files' line numbers; nullopt when they agree. Provenance
// (source, harness, game version) is not compared. A differing scenario
// (area, ticks, actions, observation headers) is reported first. Block states
// are compared by id and properties, doubles within `tolerance`.
std::optional<std::string> first_difference(const Fixture& a, const Fixture& b, double tolerance = 0.0);

std::string_view source_name(contracts::EvidenceSource source) noexcept;

// Block-state notation `namespace:id[key=value,...]`.
struct StateSpec {
    std::string id;
    std::vector<std::pair<std::string, std::string>> properties;
};
std::expected<StateSpec, std::string> parse_state(std::string_view text);

} // namespace circuit::evidence
