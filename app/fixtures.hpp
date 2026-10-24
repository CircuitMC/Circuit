// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/data/packs.hpp>
#include <string>
#include <vector>

namespace circuit::app {

// Evaluates circuit-trace fixtures against the reference rules and prints one
// line per fixture. Returns 1 if any fixture failed or could not be read.
// With data packs (the official jar), the rules run with their data, as
// scenarios of loot tables need.
int check_fixtures(const std::vector<std::string>& paths, unsigned workers, const data::PackOptions& packs = {});

// Runs a fixture's actions, lets it settle for its tick count, then reports the
// engine's per-phase time over `ticks` further ticks.
int profile_fixture(const std::string& path, unsigned ticks, unsigned workers);

// Records a skeleton fixture from the reference rules into `output`.
int record_fixture_file(const std::string& skeleton, const std::string& output, unsigned workers, const data::PackOptions& packs = {});
// Compares two recordings of the same scenario; prints the first differing
// observation. Returns 0 when they agree, 1 when they differ, 2 on errors.
int diff_fixtures(const std::string& first, const std::string& second, double tolerance);

// Prints the compatibility table of an executable bundle: target and claimed
// tier, evidence source and limitations of every mechanism.
int print_compatibility(const std::string& bundle_id);

} // namespace circuit::app
