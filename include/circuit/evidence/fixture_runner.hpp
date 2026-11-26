// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/commands.hpp>
#include <circuit/contracts/trace.hpp>
#include <circuit/evidence/trace_format.hpp>
#include <circuit/gameplay/block.hpp>
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <circuit/runtime/engine.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>

// Runs circuit-trace fixtures against the reference rules. Fixture tick 0 is
// the setup tick; Circuit tick 1 loads the chunks covering the area plus one
// chunk of margin, and fixture tick t runs as Circuit tick t + 2. Actions run
// at the start of their tick, before scheduled ticks. Observations compare
// block changes inside the area and end-of-tick probes.
namespace circuit::evidence {

// Minimal executor surface: a fixture runs on any executor that implements it.
class FixtureExecutor {
public:
    virtual ~FixtureExecutor() = default;
    virtual void advance(const std::vector<contracts::Command>& commands) = 0;
    [[nodiscard]] virtual const std::vector<contracts::TraceRecord>& trace() const = 0;
    [[nodiscard]] virtual gameplay::BlockState block(BlockPos pos) const = 0;
    // nullopt when there is no container slot at the position.
    [[nodiscard]] virtual std::optional<contracts::ItemStack> slot(BlockPos pos, std::uint8_t slot) const = 0;
    // Live entities, for circuit-trace 2 observations.
    [[nodiscard]] virtual std::vector<gameplay::Entity> entities() const = 0;
    // Block light and sky light at the end of the last tick; nullopt when the
    // executor does not compute light.
    [[nodiscard]] virtual std::optional<std::pair<std::uint8_t, std::uint8_t>> light(BlockPos) const { return std::nullopt; }
    // The data packs' data the rules run with (loot tables, enchantments);
    // null without. Scenarios of the `loot` mechanism and enchanted tools
    // need it.
    [[nodiscard]] virtual const gameplay::GameData* data() const { return nullptr; }
};
using ExecutorFactory =
    std::function<std::unique_ptr<FixtureExecutor>(const contracts::VersionBundle& bundle, std::uint64_t seed)>;
// The data packs' data for a bundle (null: none).
using DataSource = std::function<std::shared_ptr<const gameplay::GameData>(const contracts::VersionBundle& bundle)>;

// Fixtures on any engine (runtime::Engine): the overworld's level random
// starts from 0 and random sequences from the world seed 0, as on the
// reference harness's worlds; `data` gives the data packs' data per bundle.
ExecutorFactory engine_executor(runtime::EngineFactory engine, DataSource data = {});

struct FixtureReport {
    enum class Outcome : std::uint8_t {
        passed,
        failed,      // the rules produced different observations
        unsupported, // the fixture uses content or setups the reference rules cannot represent
        invalid,     // the fixture cannot be evaluated (skeleton, unknown mechanism)
    };
    Outcome outcome{};
    std::string detail;
    std::size_t changes_compared{};
    std::size_t probes_compared{};
    std::size_t events_compared{}; // v2 entity and chunk events, entity orders and random draws
};
std::string_view outcome_name(FixtureReport::Outcome outcome) noexcept;

// The commands a fixture's actions translate to, by Circuit tick (tick 1 loads
// the area, fixture tick t runs as Circuit tick t + 2), and the bundle to run
// them on (random tick speed from the fixture).
struct FixtureInputs {
    contracts::VersionBundle bundle;
    std::map<Tick, std::vector<contracts::Command>> by_tick;
};
std::expected<FixtureInputs, std::string> fixture_inputs(const Fixture& fixture);
// `summon <type> <x> <y> <z> [{...}]` for the entities the rules summon, as
// fixtures and the server's command read it (absolute coordinates).
std::expected<contracts::SummonEntity, std::string> parse_summon(const std::string& command);

FixtureReport evaluate_fixture(const Fixture& fixture, const ExecutorFactory& executor);

// Runs a fixture (usually a skeleton) and fills every observation from the
// reference rules: all area changes when `changes` is enabled, and every block
// and slot probe. The result's source is circuit_reference: it documents
// Circuit's behavior and is never compatibility evidence.
std::expected<Fixture, std::string> record_fixture(const Fixture& skeleton, const ExecutorFactory& executor);

} // namespace circuit::evidence
