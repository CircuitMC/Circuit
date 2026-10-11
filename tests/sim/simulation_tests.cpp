// SPDX-License-Identifier: AGPL-3.0-only
// Differential tests: the region-parallel runtime against the independent
// sequential oracle, across worker counts, region layouts, seeded schedule
// exploration, injected aborts and replay.
#include "../oracle/oracle.hpp"
#include "../support/check.hpp"
#include "../support/light_reference.hpp"
#include "scenarios.hpp"
#include <circuit/contracts/replay.hpp>
#include <circuit/gameplay/rng.hpp>
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
    for (Tick t = 1; t <= s.ticks; ++t) in.per_tick.push_back(admitted(admit, s, t));
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

// Every phase goes through the pool, however small: the parallel path is
// what these tests check.
EngineOptions pool(unsigned workers, LayoutPolicy layout = LayoutPolicy::adaptive, std::uint64_t layout_seed = 1) {
    EngineOptions o;
    o.workers = workers;
    o.layout = layout;
    o.layout_seed = layout_seed;
    o.inline_roots = 0;
    return o;
}
EngineOptions seeded(ExplorationOptions exploration, LayoutPolicy layout = LayoutPolicy::adaptive, std::uint64_t layout_seed = 1) {
    EngineOptions o;
    o.runner = std::make_shared<SeededRunner>(exploration);
    o.layout = layout;
    o.layout_seed = layout_seed;
    return o;
}
// Every command in one level.
Scenario in_level(Scenario s, DimensionId level) {
    auto inputs = s.inputs;
    s.levels = [inputs, level](Tick t) { return std::vector<DimensionId>(inputs(t).size(), level); };
    s.name += std::format("@{}", dimension_name(level));
    return s;
}

// Scenarios in several levels, raised so that every position is inside the
// Nether and the End (y from 0).
std::vector<Scenario> level_scenarios() {
    const auto drops = raised(drops_scenario(), 8);
    return {in_levels(drops, DimensionId::overworld, drops, DimensionId::the_nether),
            in_levels(raised(hopper_scenario(), 8), DimensionId::overworld, raised(clock_scenario(), 8), DimensionId::the_end),
            in_levels(raised(piston_chain_scenario(), 8), DimensionId::the_nether, raised(comparator_chest_scenario(), 8),
                      DimensionId::the_end)};
}

// A level's state with its entities' ids cleared and the entities in a
// canonical order: entity ids are counted over all levels, so they depend
// on what the other levels spawn.
LevelDump without_entity_ids(LevelDump l) {
    for (auto& e : l.entities) e.id = {};
    std::ranges::sort(l.entities, [](const Entity& a, const Entity& b) {
        return std::tie(a.position, a.velocity, a.stack, a.age, a.last_tick, a.pickup_delay, a.on_ground)
             < std::tie(b.position, b.velocity, b.stack, b.age, b.last_tick, b.pickup_delay, b.on_ground);
    });
    return l;
}

// The records of one level that name no entity id, without their order keys.
std::vector<std::tuple<TraceKind, BlockPos, std::uint64_t, std::uint64_t, std::uint64_t>> level_records(
    const std::vector<std::vector<TraceRecord>>& traces, DimensionId level) {
    std::vector<std::tuple<TraceKind, BlockPos, std::uint64_t, std::uint64_t, std::uint64_t>> out;
    for (const auto& tick : traces)
        for (const auto& r : tick) {
            if (r.dimension != level) continue;
            switch (r.kind) {
            case TraceKind::block_changed: case TraceKind::tick_scheduled: case TraceKind::tick_executed:
            case TraceKind::block_event_queued: case TraceKind::block_event_executed: case TraceKind::chunk_loaded:
            case TraceKind::chunk_unloaded: case TraceKind::chunk_level_changed: case TraceKind::inventory_changed:
            case TraceKind::signal_changed: case TraceKind::piston_blocked:
                out.emplace_back(r.kind, r.pos, r.a, r.b, r.c);
                break;
            default: break;
            }
        }
    return out;
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
                                     scenarios[i].name, records, changes, last.overworld().blocks.size(), last.overworld().entities.size());
            require(changes > 20 && !last.overworld().blocks.empty(), scenarios[i].name);
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

    suite.run("observers fire on piston writes across the edge", [&] {
        std::size_t fired = 0;
        for (const auto& t : references[12].traces)
            for (const auto& r : of_kind(t, TraceKind::tick_executed))
                fired += r.pos == BlockPos{16, 1, 0} || r.pos == BlockPos{15, 0, 1} || r.pos == BlockPos{16, 0, 13};
        require(scenarios[12].name == "observed_pistons" && fired >= 20, std::format("{} observer ticks", fired));
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

    suite.run("drops spawn in parallel phases and items cross edges", [&] {
        std::size_t parallel_spawns = 0, pushed_across = 0;
        for (const auto& t : references[11].traces) {
            for (const auto& r : of_kind(t, TraceKind::entity_spawned)) parallel_spawns += r.key.phase == Phase::block_events;
            for (const auto& r : of_kind(t, TraceKind::entity_changed_chunk)) pushed_across += r.key.phase == Phase::block_entities;
        }
        require(parallel_spawns >= 60 && pushed_across >= 25,
                std::format("drops in block events {}, pushes across edges {}", parallel_spawns, pushed_across));
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

    suite.run("small phases run inline with the default options", [&] {
        // The default runs phase attempts of few roots on the coordinator and
        // larger ones on the pool; both kinds occur in these scenarios.
        for (std::size_t i = 0; i < scenarios.size(); ++i)
            for (unsigned w : {1u, 4u, 16u}) {
                EngineOptions o;
                o.workers = w;
                o.inline_roots = 8;
                const auto diff = compare(scenarios[i], inputs[i], references[i], o);
                require(diff.empty(), std::format("inline workers={} {}", w, diff));
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

    suite.run("restored engines continue exactly", [&] {
        // A checkpoint is a canonical dump: an engine restored from it must
        // continue exactly like the original, whatever its workers and layout.
        for (std::size_t i = 0; i < scenarios.size(); ++i) {
            const auto& s = scenarios[i];
            for (const Tick split : {Tick{1}, s.ticks / 3, 2 * s.ticks / 3, s.ticks - 1}) {
                Engine original(bundle(), s.seed, pool(4));
                for (Tick t = 1; t <= split; ++t) original.advance(inputs[i].per_tick[t - 1]);
                Engine restored(bundle(), s.seed, original.dump(), seeded({.seed = split}, LayoutPolicy::perturbed, split));
                require(restored.dump() == original.dump(), std::format("{}: restored state at {}", s.name, split));
                for (Tick t = split + 1; t <= s.ticks; ++t) {
                    restored.advance(inputs[i].per_tick[t - 1]);
                    const auto at = first_divergence(restored.trace(), references[i].traces[t - 1]);
                    require(!at, std::format("{}: split {} tick {} trace record {}", s.name, split, t, at.value_or(0)));
                    if (const auto it = references[i].dumps.find(t); it != references[i].dumps.end())
                        require(first_difference(restored.dump(), it->second).empty(), std::format("{}: split {} tick {} state", s.name, split, t));
                }
            }
        }
    });

    suite.run("replay record reproduces the run", [&] {
        const auto& s = scenarios[2];
        ReplayLog log{"java-1.21.8", std::string(bundle().rules->id), s.seed, {}, std::string(bundle().rules->block_registry)};
        for (Tick t = 1; t <= s.ticks; ++t) log.ticks.push_back({t, inputs[2].per_tick[t - 1]});
        const auto bytes = encode_replay(log);
        require(bytes.has_value(), "encode");
        const auto decoded = decode_replay(*bytes);
        require(decoded.has_value() && *decoded == log, "decode");
        const auto replay_bundle = BundleRegistry::select_for_simulation(decoded->bundle_id);
        require(replay_bundle.has_value() && check_replay_rules(*decoded, **replay_bundle).has_value(), "bundle and rules from record");
        Engine engine(**replay_bundle, decoded->seed, pool(4));
        for (const auto& t : decoded->ticks) engine.advance(t.commands);
        require(engine.dump() == references[2].dumps.at(s.ticks), "replayed final state");
    });

    suite.run("transient heads consume block-entity creation order", [&] {
        // Hand-derived: the first piston's head is created (0), the second piston
        // moves the power source (1) and creates its head (2), then the first
        // piston retracts, removing its head and moving its base (3). Four
        // creation values are consumed; three block entities survive.
        const std::vector<CommandBody> build{place({0, 0, 0}, blocks::piston(D::east)),
                                             place({-1, 0, -1}, blocks::piston(D::east)),
                                             place({0, 0, -1}, blocks::redstone_block()),
                                             place({-1, 0, -2}, blocks::redstone_block())};
        for (auto options : {pool(4), pool(4, LayoutPolicy::single_region),
                             seeded({.seed = 5}, LayoutPolicy::perturbed, 5)}) {
            Engine engine(bundle(), 0, std::move(options));
            Oracle oracle(bundle(), 0);
            Admissions admit;
            for (Tick t = 1; t <= 6; ++t) {
                const auto commands = admit(t == 1 ? std::vector<CommandBody>{ticket(1, {0, 0}, 1)}
                                                   : t == 2 ? build : std::vector<CommandBody>{});
                engine.advance(commands);
                oracle.advance(commands);
                const auto dump = engine.dump();
                require(!first_divergence(engine.trace(), oracle.trace()) && first_difference(dump, oracle.dump()).empty(),
                        std::format("tick {} matches the oracle", t));
                if (t == 2)
                    require(dump.overworld().next_block_entity_order == 4 && dump.overworld().block_entities.size() == 3,
                            std::format("order {} with {} block entities", dump.overworld().next_block_entity_order,
                                        dump.overworld().block_entities.size()));
            }
        }
    });

    suite.run("seeded random machines match the oracle", [&] {
        // Bounded exploration: random placements of redstone components, pistons,
        // slime, hoppers, chests and stone around the chunk corner at (0, 0),
        // under seeded interleavings and perturbed layouts, compared with the
        // oracle after every tick.
        auto b = bundle();
        b.limits.random_tick_speed = 0;
        constexpr unsigned seeds = 64;
        for (unsigned seed = 1; seed <= seeds; ++seed) {
            circuit::gameplay::Rng rng(seed);
            Engine engine(b, seed, seeded({.seed = seed}, LayoutPolicy::perturbed, seed));
            Oracle oracle(b, seed);
            Admissions admit;
            for (Tick t = 1; t <= 60; ++t) {
                std::vector<CommandBody> bodies;
                if (t == 1) bodies.push_back(ticket(1, {0, 0}, 1));
                for (unsigned j = 0; t > 1 && j < (t == 2 ? 150u : 8u); ++j) {
                    const BlockPos p{static_cast<std::int32_t>(rng.bounded(14)) - 7, static_cast<std::int32_t>(rng.bounded(3)),
                                     static_cast<std::int32_t>(rng.bounded(14)) - 7};
                    const auto d = horizontal_directions[rng.bounded(4)];
                    BlockState s;
                    switch (rng.bounded(15)) {
                    case 0: s = blocks::piston(d); break;
                    case 1: s = blocks::piston(d, true); break;
                    case 2: s = blocks::slime(); break;
                    case 3: s = blocks::observer(d); break;
                    case 4: s = blocks::wire(); break;
                    case 5: s = blocks::repeater(d, static_cast<std::uint8_t>(1 + rng.bounded(4))); break;
                    case 6: s = blocks::redstone_block(); break;
                    case 7: s = blocks::lever(rng.bounded(2) == 1); break;
                    case 8: s = blocks::stone(); break;
                    case 9: s = blocks::hopper(d); break;
                    case 10: s = blocks::comparator(d); break;
                    case 11: s = blocks::chest(d); break;
                    case 12: s = blocks::lamp(); break;
                    default: s = blocks::air(); break;
                    }
                    bodies.push_back(place(p, s));
                }
                const auto commands = admit(std::move(bodies));
                engine.advance(commands);
                oracle.advance(commands);
                const auto at = first_divergence(engine.trace(), oracle.trace());
                const auto diff = first_difference(engine.dump(), oracle.dump());
                require(!at && diff.empty(),
                        std::format("seed {} tick {}: {}{}", seed, t, diff,
                                    at && *at < engine.trace().size() ? " trace " + describe(engine.trace()[*at]) : ""));
            }
        }
    });

    suite.run("out-of-range inputs are rejected on both executors", [&] {
        const std::int64_t top = (std::int64_t{bundle().world.min_y} + bundle().world.height) * fixed_one;
        const auto spawn = [](std::int64_t x, std::int64_t y, std::int64_t z) -> CommandBody {
            return SpawnItem{{x, y, z}, {}, {items::stone, 1}};
        };
        const std::vector<CommandBody> invalid{
            spawn(std::int64_t{1} << 44, fixed_one, 0), // would alias block (0, 1, 0)
            spawn(0, INT64_MIN, 0), spawn(0, INT64_MAX, 0), spawn(INT64_MIN, 0, 0), spawn(INT64_MAX, 0, 0),
            spawn(0, 0, INT64_MIN), spawn(0, 0, INT64_MAX), spawn(fixed_border, 0, 0), spawn(-fixed_border - 1, 0, 0),
            spawn(0, 0, fixed_border), spawn(0, 0, -fixed_border - 1), spawn(0, top, 0),
            spawn(0, std::int64_t{bundle().world.min_y} * fixed_one - 1, 0),
            ticket(2, {INT32_MIN, 0}, 0), ticket(3, {INT32_MAX, 0}, 0), ticket(4, {0, INT32_MIN}, 8),
            ticket(5, {0, INT32_MAX}, 8)};
        Engine engine(bundle(), 1, pool(4));
        Oracle oracle(bundle(), 1);
        Admissions admit;
        for (const auto& bodies : {std::vector<CommandBody>{ticket(1, {0, 0}, 0)}, invalid}) {
            const auto commands = admit(bodies);
            engine.advance(commands);
            oracle.advance(commands);
        }
        for (const auto* trace : {&engine.trace(), &oracle.trace()}) {
            const auto rejected = of_kind(*trace, TraceKind::command_rejected);
            require(rejected.size() == invalid.size(), std::format("{} of {} rejected", rejected.size(), invalid.size()));
            for (const auto& r : rejected)
                require(r.b == static_cast<std::uint64_t>(ErrorCode::out_of_bounds), "rejected as out of bounds");
        }
        const auto dump = engine.dump();
        require(dump.overworld().entities.empty() && dump.tickets.size() == 1 && dump == oracle.dump(), "no effect on either executor");
    });

    suite.run("query views and failure handling", [&] {
        Engine engine(bundle(), 1, pool(2));
        Admissions admit;
        engine.advance(admit({ticket(1, {0, 0}, 0)}));
        engine.advance(admit({place({1, 2, 3}, blocks::stone()), place({4, 0, 4}, blocks::chest()),
                              SpawnItem{at_block(8.5, 3, 8.5), {}, {items::stone, 1}}}));
        const auto view = engine.view();
        constexpr auto ow = DimensionId::overworld, nether = DimensionId::the_nether;
        require(view.tick() == 2 && view.block(ow, {1, 2, 3}).value() == blocks::stone(), "view reads committed state");
        require(view.block(ow, {100, 0, 0}).error() == ErrorCode::unloaded_chunk, "unloaded");
        require(view.block(ow, {0, 999, 0}).error() == ErrorCode::out_of_bounds, "height");
        require(view.block(nether, {1, 2, 3}).error() == ErrorCode::unloaded_chunk, "the same position in another level");
        require(view.block_entity(ow, {4, 0, 4}).value().has_value(), "block entity copy");
        require(!view.block_entity(ow, {1, 2, 3}).value().has_value(), "no block entity");
        require(view.entity(ow, EntityId{1}).value().has_value() && !view.entity(nether, EntityId{1}).value().has_value(),
                "entity copy, in its level only");
        engine.advance({});
        require(view.block(ow, {1, 2, 3}).error() == ErrorCode::stale_view, "view expires after a tick");

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
            std::uint64_t first_light = 0;
            for (Tick t = 1; t <= s.ticks; ++t) {
                engine.advance(in.per_tick[t - 1]);
                if (t == 2) first_light = engine.stats().light_nanoseconds;
            }
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (std::getenv("CIRCUIT_LIGHT_SPLIT")) std::cout << std::format("  light of ticks 1-2: {:.1f} ms\n", static_cast<double>(first_light) / 1e6);
            std::cout << std::format("  many_machines {} ticks, {} workers: {:.1f} ms (light {:.1f} ms), {} regions\n", s.ticks, w, ms,
                                     static_cast<double>(engine.stats().light_nanoseconds) / 1e6, engine.region_count());
        }
    });

    suite.run("levels keep identical coordinates apart", [&] {
        const auto all = level_scenarios();
        for (const auto& s : all) {
            const auto in = materialize(s);
            const auto ref = run_oracle(s, in);
            // The runtime matches the oracle, whatever the workers, region
            // layout or schedule; regions never span two levels.
            for (const auto& options : {pool(1), pool(4), pool(4, LayoutPolicy::perturbed, 3), pool(2, LayoutPolicy::single_region),
                                        seeded({.seed = 9, .abort_probability = 0.05}, LayoutPolicy::perturbed, 5)}) {
                const auto diff = compare(s, in, ref, options);
                require(diff.empty(), diff);
            }
            const auto& last = ref.dumps.rbegin()->second;
            require(last.levels.size() == 2, std::format("{}: two levels hold state", s.name));
        }
        // Each level of the two-level run is the one-level run of its
        // scenario: columns, blocks, block entities, scheduled ticks, block
        // events and the per-level counters exactly, and the same records.
        const auto drops = raised(drops_scenario(), 8);
        const auto both = in_levels(drops, DimensionId::overworld, drops, DimensionId::the_nether);
        const auto both_ref = run_oracle(both, materialize(both));
        for (const auto level : {DimensionId::overworld, DimensionId::the_nether}) {
            const auto alone = in_level(drops, level);
            const auto alone_ref = run_oracle(alone, materialize(alone));
            for (const auto& [t, dump] : alone_ref.dumps) {
                const auto a = without_entity_ids(both_ref.dumps.at(t).level(level));
                const auto b = without_entity_ids(dump.level(level));
                require(a == b, std::format("{} at {}: the level alone and beside the other agree", dimension_name(level), t));
            }
            require(level_records(both_ref.traces, level) == level_records(alone_ref.traces, level),
                    std::format("{}: the same records", dimension_name(level)));
        }
        // The same machines at the same coordinates behave the same in both
        // levels: these rules draw no random numbers.
        for (const auto& [t, dump] : both_ref.dumps) {
            auto nether = without_entity_ids(dump.level(DimensionId::the_nether));
            nether.dimension = DimensionId::overworld;
            require(nether == without_entity_ids(dump.level(DimensionId::overworld)), std::format("mirror at {}", t));
        }
        // Entity ids are counted over all levels: in each tick the overworld's
        // spawns come first, then the Nether's.
        const auto& last = both_ref.dumps.rbegin()->second;
        require(!last.overworld().entities.empty() && last.next_entity_id > 2 * last.overworld().entities.size(), "ids shared");
        // Random ticks draw from each level's own stream.
        const auto wheat = raised(wheat_scenario(), 8);
        const auto grown = in_levels(wheat, DimensionId::overworld, wheat, DimensionId::the_nether);
        const auto grown_ref = run_oracle(grown, materialize(grown));
        const auto& end = grown_ref.dumps.rbegin()->second;
        require(end.overworld().blocks != end.level(DimensionId::the_nether).blocks, "wheat grows independently in each level");
        const auto grown_diff = compare(grown, materialize(grown), grown_ref, pool(4));
        require(grown_diff.empty(), grown_diff);
    });

    suite.run("chunk levels decide what runs", [&] {
        const auto index = static_cast<std::size_t>(std::ranges::find(scenarios, std::string("chunk_levels"), &Scenario::name) - scenarios.begin());
        require(index < scenarios.size(), "the chunk level scenario");
        const auto& ref = references[index];
        const auto at = [&](Tick t) -> const WorldDump& { return ref.dumps.at(t); };
        using enum DimensionId;
        const auto stored = [&](Tick t, std::int32_t x, DimensionId level = overworld) {
            const auto* chest = block_entity_at(at(t), {x + 9, 1, 2}, level);
            unsigned n = 0;
            if (chest)
                for (const auto& stack : chest->slots) n += stack.count;
            return n;
        };
        // The items spawned at `tick` above a probe at x, by height.
        const auto heights = [&](Tick t, std::int32_t x, DimensionId level = overworld) {
            std::vector<std::int64_t> out;
            for (const auto& e : at(t).level(level).entities)
                if (e.position.x == (x + 12) * fixed_one + fixed_one / 2) out.push_back(e.position.y);
            return out;
        };
        const auto lit = [&](Tick t, std::int32_t x) { return state_at(at(t), {x + 2, 0, 2}).flag(); };
        const auto head = [&](Tick t, std::int32_t x) { return state_at(at(t), {x + 5, 1, 2}).is(BlockType::piston_head); };
        // Tick 50: entity ticking (chunk 0) runs everything; block ticking
        // (chunk 1) all but entities; border (chunk 2) and loaded without
        // simulation (chunk 6) nothing.
        require(!lit(50, 0) && !lit(50, 16) && lit(50, 32) && lit(50, 96), "scheduled ticks at simulation level 32 and lower");
        require(head(50, 0) && head(50, 16) && !head(50, 32) && !head(50, 96), "block events at simulation level 32 and lower");
        require(stored(50, 0) > 0 && stored(50, 16) == stored(50, 0) && stored(50, 32) == 0 && stored(50, 96) == 0,
                "block entities at simulation level 32 and lower");
        require(heights(50, 0) == std::vector<std::int64_t>{fixed_one} && heights(50, 16) == std::vector<std::int64_t>{4 * fixed_one}
                    && heights(50, 32) == std::vector<std::int64_t>{4 * fixed_one} && heights(50, 96) == std::vector<std::int64_t>{4 * fixed_one},
                "entities at simulation level 31 and lower");
        // Ticks 60-79: the player's simulation ticket simulates chunk 6.
        require(!lit(70, 96) && head(70, 96) && stored(80, 96) > 0 && heights(80, 96) == std::vector<std::int64_t>{fixed_one, fixed_one},
                "a simulation ticket simulates the loaded chunks");
        require(stored(90, 96) == stored(80, 96) && stored(80, 96) < stored(80, 0), "without it, chunk 6 stops");
        // Chunk (-12,0) dropped at tick 4 with its lamp's tick due at 7: back
        // at 30, the tick is due at 32 and the chunk runs ticks and block
        // entities again from 32; its block event ran before it dropped.
        const auto lamp_off = [&](Tick t) {
            for (const auto& r : ref.traces[t - 1])
                if (r.kind == TraceKind::block_changed && r.pos == BlockPos{-190, 0, 2} && r.dimension == overworld) return true;
            return false;
        };
        Tick off = 0;
        for (Tick t = 1; t <= 60; ++t)
            if (lamp_off(t)) off = t;
        require(off == 32, std::format("a dropped chunk's tick waits out its delay, off at {}", off));
        require(state_at(at(10), {-187, 1, 2}).is(BlockType::moving_piston) && !head(30, -192) && head(40, -192),
                "its moving piston waits");
        require(stored(30, -192) == stored(10, -192) && stored(40, -192) > stored(30, -192), "its hopper waits too");
        // The post-teleport ticket loads chunk (-6,0) at tick 90 and expires at 95.
        const auto chunk_record = [&](Tick t, TraceKind kind) {
            return std::ranges::count_if(ref.traces[t - 1], [&](const TraceRecord& r) {
                return r.kind == kind && r.pos == chunk_origin({-6, 0});
            });
        };
        require(chunk_record(90, TraceKind::chunk_loaded) == 1 && chunk_record(95, TraceKind::chunk_unloaded) == 1,
                "a ticket's lifetime");
        // The Nether, without players or forced chunks: idle after tick 300
        // until a player arrives at 400; awake again for 300 ticks after
        // they leave at 420.
        require(stored(300, 0, the_nether) > stored(290, 0, the_nether) && stored(390, 0, the_nether) == stored(300, 0, the_nether),
                "an idle level stops its block entities");
        require(heights(390, 0, the_nether).size() == 8
                    && std::ranges::count(heights(390, 0, the_nether), 4 * fixed_one) == 2,
                "and its entities");
        require(stored(420, 0, the_nether) > stored(390, 0, the_nether) && stored(450, 0, the_nether) > stored(420, 0, the_nether)
                    && std::ranges::count(heights(450, 0, the_nether), fixed_one) == 9,
                "a player wakes it at once");
        require(at(450).level(the_nether).last_active == 419 && at(450).overworld().last_active == 450 && at(450).started == 0,
                "the last active ticks");
    });

    suite.run("random ticks run where each version runs them", [&] {
        // A wheat field in chunk (0,0), with a player in the chunk, 120
        // blocks away or 130 blocks away; and a spectator in the chunk.
        const auto grown = [&](std::string_view version, std::optional<PlayerPresence> player) {
            const auto& b = **BundleRegistry::select_for_simulation(version);
            Engine engine(b, 5, pool(2));
            Oracle oracle(b, 5);
            Admissions admit;
            std::vector<CommandBody> setup;
            for (int x = 0; x < 16; ++x)
                for (int z = 0; z < 16; ++z) setup.push_back(place({x, 0, z}, blocks::wheat()));
            if (player) setup.push_back(*player);
            for (Tick t = 1; t <= 200; ++t) {
                const auto commands = admit(t == 1 ? std::vector<CommandBody>{ticket(1, {0, 0}, 0)}
                                            : t == 2 ? setup
                                                     : std::vector<CommandBody>{});
                engine.advance(commands);
                oracle.advance(commands);
                require(engine.trace() == oracle.trace(), std::format("{} tick {}: both executors agree", version, t));
            }
            require(engine.dump() == oracle.dump() && oracle.dump().overworld().blocks.size() == 256, "both executors end alike");
            unsigned ages = 0;
            for (int x = 0; x < 16; ++x)
                for (int z = 0; z < 16; ++z) ages += state_at(oracle.dump(), {x, 0, z}).level();
            return ages;
        };
        const auto player = [](double x, bool spectator = false) {
            return PlayerPresence{{1, 1}, at_block(x, 0.0, 8.5), spectator};
        };
        require(grown("java-1.21.8", std::nullopt) > 0, "1.21.8: without players");
        require(grown("java-1.21.4", std::nullopt) == 0 && grown("java-1.21.4", player(8.5, true)) == 0,
                "1.21.4: not without a player, nor for a spectator");
        require(grown("java-1.21.4", player(8.5)) > 0 && grown("java-1.21.4", player(127.5)) > 0, "1.21.4: within 128 blocks");
        require(grown("java-1.21.4", player(136.5)) == 0, "1.21.4: not 128 blocks from the chunk's center");
    });

    suite.run("light decides where crops grow", [&] {
        const auto index = static_cast<std::size_t>(std::ranges::find(scenarios, std::string("crop_light"), &Scenario::name) - scenarios.begin());
        require(index < scenarios.size(), "the crop scenario");
        const auto& ref = references[index];
        // Crops grown by tick `t`, by their distance from the glowstone at
        // (4,0,4) and from the lamp at (12,0,12).
        const auto grown = [&](Tick t, auto&& where) {
            unsigned n = 0;
            for (int x = 0; x < 16; ++x)
                for (int z = 0; z < 16; ++z) {
                    const auto w = state_at(ref.dumps.at(t), {x, 0, z});
                    if (w.is(BlockType::wheat) && w.level() > 0 && where(x, z)) ++n;
                }
            return n;
        };
        const auto near_glowstone = [](int x, int z) { return std::abs(x - 4) + std::abs(z - 4) <= 6; };
        const auto near_lamp = [](int x, int z) { return std::abs(x - 12) + std::abs(z - 12) <= 6; };
        const auto anywhere = [](int, int) { return true; };
        require(grown(120, near_glowstone) > 0 && grown(120, anywhere) == grown(120, near_glowstone), "only crops in light 9 or more grew");
        require(grown(240, near_lamp) > 0 && grown(240, [&](int x, int z) { return !near_glowstone(x, z) && !near_lamp(x, z); }) == 0,
                "the lit lamp lets the crops around it grow");
    });

    suite.run("light follows the blocks of the previous tick", [&] {
        // The engine's incremental light against light computed from scratch
        // from the previous tick's blocks, in columns sampled from scenarios
        // with lamps switching, pistons moving and chunks dropping and
        // returning. A column's light depends only on the columns around it.
        for (const auto& s : {chunk_levels_scenario(), many_machines_scenario(), piston_pulse_scenario()}) {
            const auto in = materialize(s);
            Engine engine(bundle(), s.seed, pool(4));
            std::size_t compared = 0;
            // Light is read at these ticks only, so that changes in between
            // are followed together.
            for (Tick t = 1; t <= std::min<Tick>(s.ticks, 66); ++t) {
                const bool check = t == 2 || t == 3 || t == 17 || t == 31 || t == 32 || t == 45 || t == 66;
                std::optional<WorldDump> before;
                if (check) before = engine.dump();
                engine.advance(in.per_tick[t - 1]);
                if (!check) continue;
                const DumpLightWorld world(*before, DimensionId::overworld);
                const auto& shape = bundle().world;
                const auto top = std::min(shape.min_y + shape.height - 1, std::max(world.highest(), shape.min_y) + 16);
                std::size_t k = 0;
                for (const auto c : world.columns()) {
                    if (k++ % 23 != t % 23) continue;
                    std::set<ChunkPos> around;
                    for (int dx = -1; dx <= 1; ++dx)
                        for (int dz = -1; dz <= 1; ++dz)
                            if (world.loaded({c.x + dx, c.z + dz})) around.insert({c.x + dx, c.z + dz});
                    const ReferenceLight reference(world, around, shape.min_y, top);
                    for (auto y = shape.min_y; y <= top; ++y)
                        for (int i = 0; i < 256; ++i) {
                            const BlockPos p{c.x * 16 + (i & 15), y, c.z * 16 + (i >> 4)};
                            const auto [block, sky] = engine.light(DimensionId::overworld, p);
                            if (block != reference.get(LightKind::block, p) || sky != reference.get(LightKind::sky, p))
                                throw std::runtime_error(std::format("{} tick {}: light at ({},{},{}) {}/{} instead of {}/{}", s.name, t, p.x, p.y,
                                                                     p.z, block, sky, reference.get(LightKind::block, p),
                                                                     reference.get(LightKind::sky, p)));
                        }
                    ++compared;
                }
            }
            require(compared > 5, std::format("{}: {} columns compared", s.name, compared));
        }
    });

    suite.run("commands in levels that do not exist are refused", [&] {
        Engine engine(bundle(), 1, pool(1));
        Oracle oracle(bundle(), 1);
        const std::vector<Command> commands{{1, AddTicket{1, {0, 0}, 0}, static_cast<DimensionId>(7)},
                                            {2, AddTicket{2, {0, 0}, 0}, DimensionId::the_end}};
        engine.advance(commands);
        oracle.advance(commands);
        require(engine.trace() == oracle.trace() && engine.dump() == oracle.dump(), "both executors agree");
        require(engine.trace()[0].kind == TraceKind::command_rejected && engine.trace()[1].kind == TraceKind::command_applied,
                "an unknown level is refused");
        require(engine.dump().levels.size() == 1 && engine.dump().levels[0].dimension == DimensionId::the_end, "only the End loaded");
    });

    return suite.finish();
}
