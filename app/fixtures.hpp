// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <string>
#include <vector>

namespace circuit::app {

// Evaluates circuit-trace fixtures against the reference rules and prints one
// line per fixture. Returns 1 if any fixture failed or could not be read.
int check_fixtures(const std::vector<std::string>& paths, unsigned workers);

// Records a skeleton fixture from the reference rules into `output`.
int record_fixture_file(const std::string& skeleton, const std::string& output, unsigned workers);

} // namespace circuit::app
