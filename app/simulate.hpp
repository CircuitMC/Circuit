// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>
#include <string>

namespace circuit::app {

struct SimulateOptions {
    std::string game_version{"java-1.21.8"};
    unsigned workers{4};
    unsigned ticks{200};
    std::uint64_t seed{1};
    std::string replay_path; // Run a recorded input log instead of the built-in demonstration.
    std::string record_path; // Write the inputs that were run as a replay log.
    bool realtime{false};    // Pace ticks at 20 Hz instead of running as fast as possible.
};

// Headless run of the reference rules on the region-parallel engine. No
// network listener is opened. Returns a process exit code.
int run_simulation(const SimulateOptions& options);

} // namespace circuit::app
