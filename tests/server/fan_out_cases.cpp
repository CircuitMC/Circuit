// SPDX-License-Identifier: AGPL-3.0-only
// The fork and join of the chunk packet encoders: nothing leaves fan_out()
// while a helper still runs, whichever thread throws.
#include "../support/check.hpp"
#include "../../src/server/fan_out.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using circuit::server::fan_out;
using circuit::test::require;

namespace {

// Helper jobs on threads of their own, joined when the object goes: declare
// it after the work the jobs refer to.
struct Threads {
    std::vector<std::jthread> threads;
    void post(std::function<void()> job) { threads.emplace_back(std::move(job)); }
};

// Waits until `done()` holds, for at most ten seconds; returns whether it did.
template <class Done>
bool eventually(Done&& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

// The message of the exception fan_out() threw, or an empty string.
template <class Post, class Take>
std::string thrown_by(std::size_t helpers, Post&& post, const Take& take) {
    try {
        fan_out(helpers, post, take);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

} // namespace

CIRCUIT_TEST("encoder fan-out runs every item once on the caller and the helpers") {
    for (std::size_t helpers : {0u, 1u, 3u, 7u}) {
        std::vector<int> items(200, 0);
        std::atomic<std::size_t> index{0};
        const auto take = [&] {
            for (std::size_t i; (i = index.fetch_add(1)) < items.size();) ++items[i];
        };
        Threads threads;
        require(thrown_by(helpers, [&](auto job) { threads.post(std::move(job)); }, take).empty(), "fan_out threw");
        for (const int item : items) require(item == 1, "an item was not handled exactly once");
        require(threads.threads.size() == helpers, "fan_out did not post one job per helper");
    }
}

CIRCUIT_TEST("encoder fan-out waits for the helpers when the caller's work throws") {
    for (int round = 0; round < 20; ++round) {
        const auto caller = std::this_thread::get_id();
        std::atomic<int> started{0}, finished{0};
        std::atomic<bool> caller_threw{false};
        const auto take = [&] {
            if (std::this_thread::get_id() == caller) {
                // Throw only while every helper is inside its work.
                (void)eventually([&] { return started.load() == 3; });
                caller_threw.store(true);
                throw std::runtime_error("caller");
            }
            ++started;
            (void)eventually([&] { return caller_threw.load(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ++finished;
        };
        Threads threads;
        const auto message = thrown_by(3, [&](auto job) { threads.post(std::move(job)); }, take);
        const int finished_on_return = finished.load();
        // A fan-out that returned early still has helpers that use `take`;
        // let them finish before it is destroyed, so that the failure is a report.
        (void)eventually([&] { return finished.load() == 3; });
        require(message == "caller", "fan_out did not rethrow the caller's exception");
        require(finished_on_return == 3, "fan_out returned while helpers were working");
    }
}

CIRCUIT_TEST("encoder fan-out rethrows a helper's exception after the caller's work") {
    const auto caller = std::this_thread::get_id();
    std::atomic<bool> helper_threw{false}, caller_done{false};
    const auto take = [&] {
        if (std::this_thread::get_id() != caller) {
            helper_threw.store(true);
            throw std::runtime_error("helper");
        }
        (void)eventually([&] { return helper_threw.load(); });
        caller_done.store(true);
    };
    Threads threads;
    require(thrown_by(1, [&](auto job) { threads.post(std::move(job)); }, take) == "helper", "fan_out did not rethrow the helper's exception");
    require(caller_done.load(), "the caller's work did not finish");
}

CIRCUIT_TEST("encoder fan-out waits for the posted helpers when a later post fails") {
    for (int round = 0; round < 20; ++round) {
        const auto caller = std::this_thread::get_id();
        std::atomic<int> started{0}, finished{0};
        std::atomic<bool> caller_ran{false};
        const auto take = [&] {
            if (std::this_thread::get_id() == caller) {
                caller_ran.store(true);
                return;
            }
            ++started;
            (void)eventually([&] { return caller_ran.load(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ++finished;
        };
        // Of five helpers the third cannot be posted: two run, three never do.
        Threads threads;
        const auto post = [&](auto job) {
            if (threads.threads.size() == 2) throw std::runtime_error("post");
            threads.post(std::move(job));
        };
        const auto message = thrown_by(5, post, take);
        const int finished_on_return = finished.load();
        (void)eventually([&] { return finished.load() == 2; });
        require(message == "post", "fan_out did not rethrow the exception of post");
        require(finished_on_return == 2, "fan_out returned while the posted helpers were working");
        require(threads.threads.size() == 2 && started.load() == 2, "a job ran that post had not accepted");
    }
}
