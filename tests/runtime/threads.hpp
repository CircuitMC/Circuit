// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Helpers of the worker-thread cases of runtime_tests.
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <optional>
#include <thread>

namespace circuit::test {

// Waits until `done()` holds, for at most ten seconds; returns whether it did.
// The cases use it where a missing wake-up should fail the case, not hang it.
template <class Done>
bool eventually(Done&& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

// The number of threads of this process, or nullopt where the system does not
// list them. A thread that has just been joined can still be listed for a
// moment, so the count is read until two readings 10 ms apart agree.
inline std::optional<std::size_t> settled_thread_count() {
    std::error_code error;
    const auto count = [&]() -> std::optional<std::size_t> {
        std::filesystem::directory_iterator tasks("/proc/self/task", error);
        if (error) return std::nullopt;
        return static_cast<std::size_t>(std::distance(tasks, std::filesystem::directory_iterator{}));
    };
    auto last = count();
    for (int i = 0; last && i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto now = count();
        if (now == last) return now;
        last = now;
    }
    return last;
}

} // namespace circuit::test
