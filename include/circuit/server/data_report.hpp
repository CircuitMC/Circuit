// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/version.hpp>
#include <circuit/data/packs.hpp>
#include <expected>
#include <string>

namespace circuit::server {

// Loads data packs as a server would and describes them: the packs and their
// digests, the fingerprint, tag and resource counts, what the server sends,
// and where block tags disagree with the public registry data.
std::expected<std::string, std::string> data_report(const contracts::VersionBundle& bundle, const data::PackOptions& options);

} // namespace circuit::server
