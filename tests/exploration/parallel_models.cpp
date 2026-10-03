// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/exploration/models.hpp>
#include <circuit/runtime/executor.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace circuit::exploration;
namespace {
void require(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}
template<class F> void rejects(F action, const char* why) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, why);
}
Proposal change(const State& state, Resource id, std::int64_t value) {
    return {{{id, state.observe(id)}}, {{id, value}}, {}};
}

void dependency_scheduling() {
    const std::array<Access, 6> actions{{
        {{}, {0}}, {{}, {1}}, {{0}, {2}}, {{1}, {3}}, {{2, 3}, {4}}, {{0}, {0}}
    }};
    const auto waves = dependency_waves(actions);
    require(waves == std::vector<std::vector<std::size_t>>{{0, 1}, {2, 3}, {4, 5}}, "dependency edges lost");
    require(!conflicts({{0}, {}}, {{0}, {}}), "read/read should be independent");
    require(conflicts({{0}, {}}, {{}, {0}}), "read/write anti-dependency lost");
    require(dependency_waves({}).empty(), "empty workload should be empty");
    for (unsigned workers : {1u, 2u, 4u, 8u, 16u}) {
        circuit::runtime::StageExecutor pool(workers);
        for (unsigned variation = 0; variation < 3; ++variation) {
            std::array<int, 5> cells{};
            std::array<int, 6> outputs{};
            for (auto wave : waves) {
                if (variation == 1) std::reverse(wave.begin(), wave.end());
                if (variation == 2) std::rotate(wave.begin(), wave.begin() + 1, wave.end());
                std::vector<std::function<void()>> jobs;
                for (auto i : wave) jobs.emplace_back([&, i] {
                    if (i % 2 == 0) std::this_thread::yield();
                    switch (i) {
                    case 0: outputs[i] = cells[0] = 1; break;
                    case 1: outputs[i] = cells[1] = 2; break;
                    case 2: outputs[i] = cells[2] = cells[0] + 10; break;
                    case 3: outputs[i] = cells[3] = cells[1] + 20; break;
                    case 4: outputs[i] = cells[4] = cells[2] + cells[3]; break;
                    case 5: outputs[i] = cells[0] += 4; break;
                    }
                });
                pool.submit(std::move(jobs)).get(); // Coordinator only.
            }
            // Independent arithmetic expectations, not the scheduler at one worker.
            require(cells == std::array<int, 5>{5, 2, 11, 22, 33}, "parallel values differ from reference");
            require(outputs == std::array<int, 6>{1, 2, 11, 22, 33, 5}, "canonical observations differ");
        }
    }
}

void boundary_chain_and_delays() {
    Agenda queue;
    queue.schedule({{1, 0, 0}, 0, 9});
    queue.schedule({{3, 0, 0}, 0, 100});
    std::array<unsigned, 4> visits{};
    unsigned count = 0;
    while (auto event = queue.next(1)) {
        require(++count <= 10, "unbounded chain");
        ++visits[event->target];
        if (event->value > 0)
            queue.schedule({{1, 0, event->key.order + 1}, (event->target + 1) % 4, event->value - 1});
    }
    require(visits == std::array<unsigned, 4>{3, 3, 2, 2}, "fixed-round approximation lost a return");
    require(queue.size() == 1 && !queue.next(2), "delayed action ran too early");
    const auto delayed = queue.next(5); // Overdue delivery may still spawn same-tick work.
    require(delayed && delayed->key.tick == 3, "scheduled time was rewritten");
    queue.schedule({{3, 0, 1}, 1, 101});
    require(queue.next(5)->value == 101, "overdue causal child rejected");
    require(!queue.next(5), "unexpected work remains");
    rejects([&] { queue.schedule({{2, 0, 8}, 0, 0}); }, "late event accepted");
}

void transient_pulse() {
    Agenda queue;
    queue.schedule({{1, 0, 1}, 0, 0}); // Arrival order is deliberately reversed.
    queue.schedule({{1, 0, 0}, 0, 1});
    bool powered = false, latched = false;
    std::vector<std::int64_t> trace;
    while (auto e = queue.next(1)) {
        const bool next = e->value != 0;
        if (!powered && next) latched = !latched;
        powered = next;
        trace.push_back(e->value);
    }
    require(!powered && latched && trace == std::vector<std::int64_t>{1, 0}, "pulse was coalesced away");
    // A final-value-only consumer would see zero and never latch.
    require(latched != (trace.back() != 0), "counterexample does not distinguish final-state coalescing");
}

void semantic_phase_order() {
    Agenda queue;
    queue.schedule({{4, 2, 0}, 0, 30});
    queue.schedule({{4, 1, 9}, 0, 20});
    queue.schedule({{4, 1, 1}, 0, 10});
    rejects([&] { queue.schedule({{4, 1, 1}, 0, 99}); }, "duplicate ordering key accepted");
    require(queue.next(4)->value == 10 && queue.next(4)->value == 20, "phase/sequence order lost");
    require(queue.next(4)->value == 30, "later phase executed early");
    rejects([&] { queue.schedule({{4, 1, 10}, 0, 40}); }, "completed earlier phase reopened");
}

void competing_hoppers() {
    State world;
    world.define(0, 1); world.define(1, 0); world.define(2, 0);
    const auto transfer = [&](Resource destination) {
        return Proposal{{{0, world.observe(0)}, {destination, world.observe(destination)}},
                        {{0, 0}, {destination, 1}}, {"remove source", "insert destination"}};
    };
    const auto left = transfer(1), right = transfer(2);
    require(world.commit(left) == Commit::applied, "first transfer failed");
    require(world.commit(right) == Commit::stale, "second hopper duplicated the item");
    require(world.observe(0).value == 0 && world.observe(1).value == 1 && world.observe(2).value == 0,
            "failed transfer changed an inventory");
    require(world.events().size() == 2, "rejected operation leaked events");
}

void stale_furnace_read() {
    State world; world.define(0, 0); world.define(1, 0); // Fuel and modeled progress.
    const Proposal premature{{{0, world.observe(0)}, {1, world.observe(1)}}, {{1, 0}}, {"idle"}};
    require(world.commit(change(world, 0, 1)) == Commit::applied, "fuel insertion failed");
    require(world.commit(premature) == Commit::stale, "old read silently won over earlier input");
    const Proposal recomputed{{{0, world.observe(0)}, {1, world.observe(1)}}, {{0, 0}, {1, 1}}, {"consume", "advance"}};
    require(world.commit(recomputed) == Commit::applied, "valid recomputation failed");
    require(world.observe(1).value == 1 && world.events().size() == 2, "time/effects applied twice");
}

void piston_footprint_and_order() {
    State world; world.define(0, 1); world.define(1, 0); world.define(2, 0);
    Proposal plan{{{0, world.observe(0)}, {1, world.observe(1)}}, {{0, 0}, {1, 1}, {2, 2}}, {"remove", "place", "notify"}};
    require(world.commit(plan) == Commit::undeclared_write, "expanded footprint was not detected");
    require(world.observe(0).value == 1 && world.observe(1).value == 0 && world.events().empty(),
            "out-of-footprint operation partially published");
    plan.expected.emplace(2, world.observe(2)); // Re-resolve the complete footprint before retry.
    require(world.commit(plan) == Commit::applied, "complete footprint rejected");
    require(world.events() == std::vector<std::string>{"remove", "place", "notify"}, "internal event order lost");
    // This labels internal steps; it does not prove actual piston callback semantics.
}

void migrating_entity_clock() {
    EntityClock entity(0);
    auto old = entity.ticket();
    require(entity.advance(old, 7) == Advance::advanced, "source did not advance");
    entity.migrate(1);
    require(entity.advance(old, 7) == Advance::stale_owner, "old owner remained writable");
    require(entity.advance(entity.ticket(), 7) == Advance::already_advanced, "destination ticked entity twice");
    require(entity.advance(entity.ticket(), 8) == Advance::advanced && entity.age() == 2, "next tick lost");
    require(entity.advance(entity.ticket(), 6) == Advance::past_tick, "time reversal accepted");
    entity.migrate(0);
    require(entity.advance(old, 9) == Advance::stale_owner, "return to same chunk revived old ticket");
}

void unload_and_reload_generation() {
    State world; world.define(0, 12);
    auto task = change(world, 0, 99);
    world.replace(0, 12); // Same value and revision; different lifetime.
    require(world.commit(task) == Commit::stale, "same-value reload revived stale work");
    require(world.observe(0).value == 12, "old task mutated replacement object");
}

void global_spawn_reservation() {
    State world; world.define(0, 1); world.define(10, 0); world.define(20, 0);
    const Access a{{0, 10}, {0, 10}}, b{{0, 20}, {0, 20}};
    require(conflicts(a, b), "distant regions ignored global quota dependency");
    const Proposal left{{{0, world.observe(0)}, {10, world.observe(10)}}, {{0, 0}, {10, 1}}, {"spawn left"}};
    const Proposal right{{{0, world.observe(0)}, {20, world.observe(20)}}, {{0, 0}, {20, 1}}, {"spawn right"}};
    require(world.commit(left) == Commit::applied && world.commit(right) == Commit::stale, "global budget overallocated");
    require(world.observe(10).value + world.observe(20).value == 1, "too many entities spawned");
}

void async_query_revalidation() {
    State world; world.define(0, 0); world.define(1, 0); // Geometry/light input and action target.
    const Proposal path{{{0, world.observe(0)}, {1, world.observe(1)}}, {{1, 1}}, {"use query result"}};
    require(world.commit(change(world, 0, 1)) == Commit::applied, "obstruction change failed");
    require(world.commit(path) == Commit::stale && world.observe(1).value == 0, "stale path/light result was consumed");
}

void rng_retry_uses_captured_opportunity() {
    State world; world.define(0, 0);
    unsigned draws = 0;
    const auto next_sample = [&] { ++draws; return 42; }; // Recorded opportunity, not a vanilla RNG algorithm.
    const int opportunity = next_sample();
    auto proposal = change(world, 0, opportunity);
    require(world.commit(change(world, 0, 7)) == Commit::applied, "conflicting action failed");
    require(world.commit(proposal) == Commit::stale, "stale growth plan accepted");
    proposal = change(world, 0, opportunity);
    require(world.commit(proposal) == Commit::applied && draws == 1, "retry consumed an extra opportunity");
    require(world.observe(0).value == 42, "retry changed the recorded random decision");
}

void in_flight_stage_closure() {
    StageLedger stage(1); // Root producer ticket 0 is registered before admission.
    const auto child = stage.fork(0);
    stage.finish(0);
    require(!stage.closed(), "empty root queue was mistaken for stage completion");
    const auto grandchild = stage.fork(child); // Register before finishing parent.
    stage.finish(child);
    require(!stage.closed(), "in-flight child was lost");
    stage.finish(grandchild);
    require(stage.closed(), "completed stage did not close");
    rejects([&] { stage.fork(child); }, "retired producer introduced late work");
    rejects([&] { stage.finish(child); }, "ticket completed twice");
}
}
int main() {
    const std::array scenarios{
        std::pair{"S01 dependency scheduling", &dependency_scheduling},
        std::pair{"S02 returning chains and delays", &boundary_chain_and_delays},
        std::pair{"S03 transient pulse", &transient_pulse},
        std::pair{"S04 phase and event order", &semantic_phase_order},
        std::pair{"S05 competing hoppers", &competing_hoppers},
        std::pair{"S06 stale furnace read", &stale_furnace_read},
        std::pair{"S07 piston footprint and trace", &piston_footprint_and_order},
        std::pair{"S08 migrating entity clock", &migrating_entity_clock},
        std::pair{"S09 unload generation", &unload_and_reload_generation},
        std::pair{"S10 global spawn budget", &global_spawn_reservation},
        std::pair{"S11 asynchronous query validation", &async_query_revalidation},
        std::pair{"S12 random opportunity retry", &rng_retry_uses_captured_opportunity},
        std::pair{"S13 in-flight stage closure", &in_flight_stage_closure}
    };
    try {
        for (auto [name, run] : scenarios) { run(); std::cout << "PASS " << name << '\n'; }
        std::cout << "13 abstract models passed; this is not vanilla compatibility evidence\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
