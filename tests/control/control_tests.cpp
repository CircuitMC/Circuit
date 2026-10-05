// SPDX-License-Identifier: AGPL-3.0-only
#include "../support/check.hpp"
#include <circuit/gameplay/world_types.hpp>
#include <circuit/runtime/job_runner.hpp>
#include <circuit/runtime/tick_clock.hpp>
#include <vector>

using circuit::test::require;
using namespace std::chrono_literals;
using circuit::runtime::TickClock;

namespace {
// Records its own steps into a shared log so interleavings can be compared.
class Recorder final : public circuit::runtime::Task {
public:
    Recorder(int id, int steps, std::vector<int>& log) : id_(id), left_(steps), log_(log) {}
    bool step() override {
        log_.push_back(id_);
        return --left_ > 0;
    }
    void inject_abort() override { log_.push_back(-id_); }

private:
    int id_, left_;
    std::vector<int>& log_;
};

std::vector<int> interleaving(circuit::runtime::ExplorationOptions options) {
    std::vector<int> log;
    circuit::runtime::SeededRunner runner(options);
    std::vector<Recorder> tasks;
    for (int i = 1; i <= 4; ++i) tasks.emplace_back(i, 5, log);
    std::vector<circuit::runtime::Task*> pointers;
    for (auto& t : tasks) pointers.push_back(&t);
    runner.run(pointers, 0);
    return log;
}
} // namespace

int main() {
    circuit::test::Suite suite("control_tests");

    suite.run("seeded interleavings are reproducible", [] {
        using S = circuit::runtime::ExplorationOptions::Strategy;
        for (auto strategy : {S::random, S::priority}) {
            const auto a = interleaving({.seed = 9, .strategy = strategy});
            require(a == interleaving({.seed = 9, .strategy = strategy}), "same seed, same schedule");
            require(a.size() == 20, "every step runs");
            bool differs = false;
            for (std::uint64_t seed = 10; seed < 20 && !differs; ++seed)
                differs = interleaving({.seed = seed, .strategy = strategy}) != a;
            require(differs, "seeds explore different schedules");
        }
        const auto aborted = interleaving({.seed = 3, .abort_probability = 1.0});
        require(aborted.size() == 4 && aborted[0] < 0, "injected aborts end tasks without running them");
    });

    suite.run("tick clock catch-up and backlog limit", [] {
        const TickClock::clock::time_point t0{};
        TickClock clock(t0, 50ms, 40);
        require(clock.due(t0) && !clock.due(t0 - 1ms), "first tick is due at start");
        clock.ticked();
        require(!clock.due(t0 + 49ms) && clock.due(t0 + 50ms), "next tick after one period");
        clock.ticked();
        // 1 second late: 20 ticks behind, caught up back to back.
        int caught_up = 0;
        while (clock.due(t0 + 1100ms)) {
            clock.ticked();
            ++caught_up;
        }
        require(caught_up == 21 && clock.dropped_backlog() == 0, "backlog within the limit is run");
        // 5 seconds late: the backlog is dropped, not skipped in logical time.
        require(clock.due(t0 + 6150ms) && clock.overloads() == 1 && clock.dropped_backlog() == 100, "re-anchor");
        clock.ticked();
        require(!clock.due(t0 + 6150ms) && clock.due(t0 + 6200ms), "resumes from the new anchor");
    });

    suite.run("dump digest", [] {
        circuit::gameplay::WorldDump a, b;
        a.blocks.push_back({{1, 2, 3}, 4});
        b = a;
        require(digest(a) == digest(b), "equal dumps, equal digests");
        b.blocks[0].second = 5;
        require(digest(a) != digest(b), "state change changes the digest");
        b = a;
        b.time = 1;
        require(digest(a) != digest(b), "time is included");
    });

    return suite.finish();
}
