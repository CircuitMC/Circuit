// SPDX-License-Identifier: AGPL-3.0-only
#include "../support/check.hpp"
#include <circuit/runtime/demo_kernel.hpp>
#include <atomic>
#include <stdexcept>

using circuit::test::require;

int main() {
    using namespace circuit::runtime;
    circuit::test::Suite suite("runtime_tests");

    suite.run("demo kernel matches the arithmetic reference at 1/2/4/8/16 workers", [] {
        // Independent arithmetic oracle for the synthetic chain (no scheduler reuse).
        for (unsigned workers : {1u, 2u, 4u, 8u, 16u}) {
            for (bool reversed : {false, true}) {
                DemoKernel kernel(workers);
                kernel.schedule(1, 0, 12); // Thirteen visits, including multiple returns.
                kernel.schedule(3, 2, 5); // Must not run early.
                for (unsigned tick = 1; tick <= 5; ++tick) {
                    kernel.advance(reversed);
                    for (std::size_t chunk = 0; chunk < 4; ++chunk) {
                        std::uint64_t expected = 0;
                        for (unsigned step = 0; step <= 12; ++step) expected += step % 4 == chunk;
                        if (tick >= 3)
                            for (unsigned step = 0; step <= 5; ++step) expected += (2 + step) % 4 == chunk;
                        require(kernel.snapshot().chunks[chunk] == ChunkCounters{tick, expected}, "reference mismatch");
                    }
                }
            }
        }
    });

    suite.run("demo kernel publishes nothing from a failed tick and retries it", [] {
        DemoKernel kernel(2);
        kernel.schedule(1, 0, 12);
        const auto before = kernel.snapshot();
        bool aborted = false;
        try { kernel.advance(false, 2); } catch (const std::runtime_error&) { aborted = true; }
        require(aborted && kernel.snapshot() == before, "partial tick published");
        kernel.advance(); // Failed attempt must retain the original queued event.
        require(kernel.snapshot().chunks[0] == ChunkCounters{1, 4}, "retry lost or duplicated event");
    });

    suite.run("stage executor drains a batch with a failing job and recovers", [] {
        StageExecutor pool(2);
        pool.submit({}).get();
        std::atomic_uint count{0};
        bool failure_seen = false;
        try { pool.submit({[] { throw std::runtime_error("worker failure"); }, [&] { ++count; }}).get(); }
        catch (const std::runtime_error&) { failure_seen = true; }
        require(failure_seen && count == 1, "exception did not drain all jobs");
        pool.submit({[&] { ++count; }}).get();
        require(count == 2, "executor cannot recover");
    });

    return suite.finish();
}
