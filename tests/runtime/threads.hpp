// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Helpers of the worker-thread cases of runtime_tests.
#include <chrono>
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

} // namespace circuit::test
