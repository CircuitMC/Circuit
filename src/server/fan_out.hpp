// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The fork and join of the chunk packet encoders (game.cpp).
#include <cstddef>
#include <exception>
#include <latch>
#include <mutex>

namespace circuit::server {

// Runs `take()` on the calling thread and in `helpers` jobs handed to
// `post(job)`, and returns when all of them have finished. `take` is the same
// loop for everyone: it takes work from a shared index until none is left.
//
// The jobs refer to this frame and to `take`, so nothing leaves the function
// before every posted job has finished: an exception from `take`, on any
// thread, or from `post` is kept, the first one wins, and it is rethrown after
// the wait. A job that `post` did not accept never runs.
template <class Post, class Take>
void fan_out(std::size_t helpers, Post&& post, const Take& take) {
    std::latch done(static_cast<std::ptrdiff_t>(helpers));
    std::exception_ptr failure;
    std::mutex failure_mutex;
    const auto guarded = [&](const auto& work) {
        try {
            work();
        } catch (...) {
            std::lock_guard lock(failure_mutex);
            if (!failure) failure = std::current_exception();
        }
    };
    std::size_t posted = 0;
    guarded([&] {
        for (; posted < helpers; ++posted)
            post([&] {
                guarded(take);
                done.count_down();
            });
    });
    for (auto missing = posted; missing < helpers; ++missing) done.count_down();
    guarded(take);
    done.wait();
    if (failure) std::rethrow_exception(failure);
}

} // namespace circuit::server
