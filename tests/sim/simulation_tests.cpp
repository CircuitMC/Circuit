// SPDX-License-Identifier: AGPL-3.0-only
// Differential tests: the region-parallel runtime against the independent
// sequential oracle, across worker counts, region layouts, seeded schedule
// exploration, injected aborts and replay.
#include "../oracle/oracle.hpp"
#include "../support/check.hpp"
#include "scenarios.hpp"
#include <circuit/contracts/replay.hpp>
#include <circuit/runtime/engine.hpp>
#include <chrono>
#include <format>
#include <map>

using namespace circuit::test;
using circuit::oracle::Oracle;
using circuit::runtime::Engine;
using circuit::runtime::EngineOptions;
using circuit::runtime::ExplorationOptions;
using circuit::runtime::LayoutPolicy;
using circuit::runtime::PoolRunner;
using circuit::runtime::SeededRunner;

namespace {
const VersionBundle& bundle() { return **BundleRegistry::select_for_simulation("java-1.21.8"); }

struct Inputs {
    std::vector<std::vector<Command>> per_tick; // index t-1
};
Inputs materialize(const Scenario& s) {
    Inputs in;
    Admissions admit;
    for (Tick t = 1; t <= s.ticks; ++t) in.per_tick.push_back(admit(s.inputs(t)));
    return in;
}

struct Reference {
    std::vector<std::vector<TraceRecord>> traces;
    std::map<Tick, WorldDump> dumps;
};
Reference run_oracle(const Scenario& s, const Inputs& in, const VersionBundle& b = bundle()) {
    Reference ref;
    Oracle oracle(b, s.seed);
    for (Tick t = 1; t <= s.ticks; ++t) {
        oracle.advance(in.per_tick[t - 1]);
        ref.traces.push_back(oracle.trace());
        if (t % s.dump_every == 0 || t == s.ticks) ref.dumps[t] = oracle.dump();
    }
    return ref;
}

// Returns an empty string when the engine matches the reference exactly.
std::string compare(const Scenario& s, const Inputs& in, const Reference& ref, EngineOptions options, Engine** keep = nullptr) {
    Engine engine(bundle(), s.seed, std::move(options));
    for (Tick t = 1; t <= s.ticks; ++t) {
        engine.advance(in.per_tick[t - 1]);
        const auto& expected = ref.traces[t - 1];
        if (const auto at = first_divergence(engine.trace(), expected)) {
            const auto show = [&](const std::vector<TraceRecord>& v) {
                return *at < v.size() ? describe(v[*at]) : std::string("<end>");
            };
            return std::format("{}: tick {} trace record {}: runtime {} | oracle {}", s.name, t, *at, show(engine.trace()),
                               show(expected));
        }
        if (const auto it = ref.dumps.find(t); it != ref.dumps.end()) {
            const auto diff = first_difference(engine.dump(), it->second);
            if (!diff.empty()) return std::format("{}: tick {} state: {}", s.name, t, diff);
        }
    }
    (void)keep;
    return {};
}

EngineOptions pool(unsigned workers, LayoutPolicy layout = LayoutPolicy::adaptive, std::uint64_t layout_seed = 1) {
    EngineOptions o;
    o.workers = workers;
    o.layout = layout;
    o.layout_seed = layout_seed;
    return o;
}
EngineOptions seeded(ExplorationOptions exploration, LayoutPolicy layout = LayoutPolicy::adaptive, std::uint64_t layout_seed = 1) {
    EngineOptions o;
    o.runner = std::make_shared<SeededRunner>(exploration);
    o.layout = layout;
    o.layout_seed = layout_seed;
    return o;
}
} // namespace

int main() {
    Suite suite("simulation_tests");
    const auto scenarios = all_scenarios();
    std::vector<Inputs> inputs;
    std::vector<Reference> references;
    for (const auto& s : scenarios) {
        inputs.push_back(materialize(s));
        references.push_back(run_oracle(s, inputs.back()));
    }

    suite.run("references are non-trivial", [&] {
        for (std::size_t i = 0; i < scenarios.size(); ++i) {
            std::size_t records = 0, changes = 0;
            for (const auto& t : references[i].traces) {
                records += t.size();
                changes += of_kind(t, TraceKind::block_changed).size();
            }
            const auto& last = references[i].dumps.rbegin()->second;
            std::cout << std::format("  {}: {} trace records, {} block changes, {} blocks, {} entities\n",
                                     scenarios[i].name, records, changes, last.blocks.size(), last.entities.size());
            require(changes > 20 && !last.blocks.empty(), scenarios[i].name);
        }
    });

    suite.run("scenarios exercise cross-chunk entities", [&] {
        std::size_t migrations = 0, pickups = 0;
        for (const auto& t : references[3].traces) {
            migrations += of_kind(t, TraceKind::entity_changed_chunk).size();
            for (const auto& r : of_kind(t, TraceKind::entity_removed))
                pickups += r.b == static_cast<std::uint64_t>(RemovalReason::picked_up);
        }
        require(migrations > 3 && pickups > 3, std::format("migrations {} pickups {}", migrations, pickups));
    });

    suite.run("block events queue events in the same phase", [&] {
        // Events queued while the block-event phase drains run in that phase; the
        // runtime must rebuild their global order from per-region queues.
        std::size_t chained = 0;
        for (const auto& t : references[8].traces)
            for (const auto& r : of_kind(t, TraceKind::block_event_queued)) chained += r.key.phase == Phase::block_events;
        require(chained >= 50, std::format("events queued during the block-event phase: {}", chained));
    });

    suite.run("comparators and double chests are exercised", [&] {
        std::size_t signals = 0, inventory = 0, chest_types = 0;
        for (const auto& t : references[9].traces) {
            signals += of_kind(t, TraceKind::signal_changed).size();
            inventory += of_kind(t, TraceKind::inventory_changed).size();
            for (const auto& r : of_kind(t, TraceKind::block_changed))
                chest_types += BlockState::decode(static_cast<std::uint32_t>(r.b)).value().is(BlockType::chest);
        }
        require(signals >= 10 && inventory >= 100 && chest_types >= 3,
                std::format("signals {} inventory {} chest states {}", signals, inventory, chest_types));
    });

    suite.run("piston pulses drop, pull and destroy", [&] {
        std::size_t moving = 0, executed = 0;
        for (const auto& t : references[10].traces) {
            executed += of_kind(t, TraceKind::block_event_executed).size();
            for (const auto& r : of_kind(t, TraceKind::block_changed))
                moving += BlockState::decode(static_cast<std::uint32_t>(r.b)).value().is(BlockType::moving_piston);
        }
        require(moving >= 60 && executed >= 40, std::format("moving pistons {} events {}", moving, executed));
    });

    suite.run("oracle is deterministic", [&] {
        for (std::size_t i = 0; i < scenarios.size(); ++i) {
            const auto again = run_oracle(scenarios[i], inputs[i]);
            require(again.traces == references[i].traces && again.dumps == references[i].dumps, scenarios[i].name);
        }
    });

    suite.run("pool runner at 1/2/4/8/16 workers", [&] {
        for (std::size_t i = 0; i < scenarios.size(); ++i)
            for (unsigned w : {1u, 2u, 4u, 8u, 16u}) {
                const auto diff = compare(scenarios[i], inputs[i], references[i], pool(w));
                require(diff.empty(), std::format("workers={} {}", w, diff));
            }
    });

    suite.run("single region is equivalent", [&] {
        for (std::size_t i = 0; i < scenarios.size(); ++i) {
            const auto diff = compare(scenarios[i], inputs[i], references[i], pool(1, LayoutPolicy::single_region));
            require(diff.empty(), diff);
        }
    });

    suite.run("seeded random and priority interleavings", [&] {
        for (std::size_t i = 0; i < scenarios.size(); ++i)
            for (std::uint64_t seed = 1; seed <= 6; ++seed) {
                const auto strategy = seed % 2 ? ExplorationOptions::Strategy::random : ExplorationOptions::Strategy::priority;
                const auto diff = compare(scenarios[i], inputs[i], references[i], seeded({.seed = seed, .strategy = strategy}));
                require(diff.empty(), std::format("exploration seed={} {}", seed, diff));
            }
    });

    suite.run("injected aborts roll back and retry", [&] {
        std::uint64_t injected = 0;
        for (std::size_t i = 0; i < scenarios.size(); ++i)
            for (std::uint64_t seed = 1; seed <= 3; ++seed) {
                auto runner = std::make_shared<SeededRunner>(ExplorationOptions{.seed = seed * 101, .abort_probability = 0.05});
                EngineOptions options;
                options.runner = runner;
                const auto diff = compare(scenarios[i], inputs[i], references[i], options);
                require(diff.empty(), std::format("abort seed={} {}", seed, diff));
                injected += runner->injected();
            }
        std::cout << std::format("  injected aborts: {}\n", injected);
        require(injected > 100, "aborts were actually injected");
    });

    suite.run("region layout does not change results", [&] {
        for (std::size_t i = 0; i < scenarios.size(); ++i)
            for (std::uint64_t seed = 1; seed <= 3; ++seed) {
                auto diff = compare(scenarios[i], inputs[i], references[i], pool(4, LayoutPolicy::perturbed, seed));
                require(diff.empty(), std::format("perturbed pool seed={} {}", seed, diff));
                diff = compare(scenarios[i], inputs[i], references[i],
                               seeded({.seed = seed + 7, .abort_probability = 0.02}, LayoutPolicy::perturbed, seed + 50));
                require(diff.empty(), std::format("perturbed seeded seed={} {}", seed, diff));
            }
    });

    suite.run("missing isolation is detected", [&] {
        // Two hoppers in different columns compete for one chest slot.
        const Scenario race{"unchecked_race", 1, 3, [](Tick t) -> std::vector<CommandBody> {
            if (t == 1) return {ticket(1, {0, 0}, 1)};
            if (t != 2) return {};
            std::vector<CommandBody> b{place({16, 0, 0}, blocks::chest())};
            for (std::uint8_t i = 0; i < 26; ++i) b.push_back(insert({16, 0, 0}, i, items::stone, 64));
            b.push_back(insert({16, 0, 0}, 26, items::stone, 63));
            b.push_back(place({15, 0, 0}, blocks::hopper(D::east)));
            b.push_back(insert({15, 0, 0}, 0, items::stone, 1));
            b.push_back(place({17, 0, 0}, blocks::hopper(D::west)));
            b.push_back(insert({17, 0, 0}, 0, items::stone, 1));
            return b;
        }, 1};
        const auto in = materialize(race);
        const auto ref = run_oracle(race, in);
        unsigned detected = 0;
        for (std::uint64_t seed = 1; seed <= 16; ++seed) {
            auto unchecked = seeded({.seed = seed});
            unchecked.unchecked_ownership_for_testing = true;
            if (!compare(race, in, ref, unchecked).empty()) ++detected;
            require(compare(race, in, ref, seeded({.seed = seed})).empty(), "checked engine is exact");
        }
        require(detected > 0, "exploration exposes the unchecked race");
        bool refused = false;
        try {
            auto bad = pool(2);
            bad.unchecked_ownership_for_testing = true;
            Engine e(bundle(), 1, bad);
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        require(refused, "unchecked mode is refused with a concurrent runner");
    });

    suite.run("conflicts merge regions once and quiet regions split", [&] {
        const auto s = clock_scenario();
        Engine engine(bundle(), s.seed, pool(4));
        const auto in = materialize(s);
        std::uint64_t aborts_after_warmup = 0;
        std::size_t regions_while_running = 0;
        for (Tick t = 1; t <= s.ticks; ++t) {
            engine.advance(in.per_tick[t - 1]);
            if (t == 10) aborts_after_warmup = engine.stats().conflict_aborts;
            if (t == 100) regions_while_running = engine.region_count();
        }
        require(engine.stats().conflict_aborts == aborts_after_warmup, "an active clock does not keep aborting");
        require(engine.stats().merges > 0 && engine.stats().splits > 0, "merged on conflict, split after going quiet");
        require(engine.region_count() > regions_while_running, "stopped clock region split");
    });

    suite.run("independent machines run as separate tasks", [&] {
        const auto s = many_machines_scenario();
        const auto in = materialize(s);
        Engine engine(bundle(), s.seed, pool(8));
        for (Tick t = 1; t <= s.ticks; ++t) engine.advance(in.per_tick[t - 1]);
        require(engine.stats().max_tasks_in_phase >= 40, std::format("parallel tasks {}", engine.stats().max_tasks_in_phase));
        require(engine.region_count() >= 40, std::format("regions {}", engine.region_count()));
    });

    suite.run("replay record reproduces the run", [&] {
        const auto& s = scenarios[2];
        ReplayLog log{"java-1.21.8", s.seed, {}};
        for (Tick t = 1; t <= s.ticks; ++t) log.ticks.push_back({t, inputs[2].per_tick[t - 1]});
        const auto bytes = encode_replay(log);
        require(bytes.has_value(), "encode");
        const auto decoded = decode_replay(*bytes);
        require(decoded.has_value() && *decoded == log, "decode");
        const auto replay_bundle = BundleRegistry::select_for_simulation(decoded->bundle_id);
        require(replay_bundle.has_value(), "bundle from record");
        Engine engine(**replay_bundle, decoded->seed, pool(4));
        for (const auto& t : decoded->ticks) engine.advance(t.commands);
        require(engine.dump() == references[2].dumps.at(s.ticks), "replayed final state");
    });

    suite.run("query views and failure handling", [&] {
        Engine engine(bundle(), 1, pool(2));
        Admissions admit;
        engine.advance(admit({ticket(1, {0, 0}, 0)}));
        engine.advance(admit({place({1, 2, 3}, blocks::stone()), place({4, 0, 4}, blocks::chest()),
                              SpawnItem{at_block(8.5, 3, 8.5), {}, {items::stone, 1}}}));
        const auto view = engine.view();
        require(view.tick() == 2 && view.block({1, 2, 3}).value() == blocks::stone(), "view reads committed state");
        require(view.block({100, 0, 0}).error() == ErrorCode::unloaded_chunk, "unloaded");
        require(view.block({0, 999, 0}).error() == ErrorCode::out_of_bounds, "height");
        require(view.block_entity({4, 0, 4}).value().has_value(), "block entity copy");
        require(!view.block_entity({1, 2, 3}).value().has_value(), "no block entity");
        require(view.entity(EntityId{1}).value().has_value(), "entity copy");
        engine.advance({});
        require(view.block({1, 2, 3}).error() == ErrorCode::stale_view, "view expires after a tick");

        bool rejected = false;
        try {
            Engine e(**BundleRegistry::find("java-1.20.4"), 1, pool(1));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "candidate without rules cannot run");

        auto limited = bundle();
        limited.limits.max_block_events_per_tick = 1;
        Engine small(limited, 1, pool(2));
        Admissions a2;
        small.advance(a2({ticket(1, {0, 0}, 1)}));
        small.advance(a2({place({0, 0, 0}, blocks::piston(D::north)), place({20, 0, 0}, blocks::piston(D::north))}));
        bool failed = false;
        try {
            small.advance(a2({place({1, 0, 0}, blocks::redstone_block()), place({21, 0, 0}, blocks::redstone_block())}));
        } catch (const std::runtime_error&) {
            failed = true;
        }
        bool refused = false;
        try { small.advance({}); } catch (const std::logic_error&) { refused = true; }
        require(failed && refused, "capacity failure is explicit and final");
    });

    suite.run("timing sample (informational)", [&] {
        const auto s = many_machines_scenario();
        const auto in = materialize(s);
        for (unsigned w : {1u, 4u, 8u}) {
            auto options = pool(w);
            options.record_trace = false;
            Engine engine(bundle(), s.seed, options);
            const auto start = std::chrono::steady_clock::now();
            for (Tick t = 1; t <= s.ticks; ++t) engine.advance(in.per_tick[t - 1]);
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::cout << std::format("  many_machines {} ticks, {} workers: {:.1f} ms, {} regions\n", s.ticks, w, ms,
                                     engine.region_count());
        }
    });

    return suite.finish();
}
