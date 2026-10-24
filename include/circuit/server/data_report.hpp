// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/version.hpp>
#include <circuit/data/packs.hpp>
#include <circuit/gameplay/game_data.hpp>
#include <expected>
#include <memory>
#include <string>

namespace circuit::server {

// Loads data packs as a server would and describes them: the packs and their
// digests, the fingerprint, tag and resource counts, what the server sends,
// and where block tags disagree with the public registry data.
std::expected<std::string, std::string> data_report(const contracts::VersionBundle& bundle, const data::PackOptions& options);

// The data the rules use from data packs loaded as a server would (null
// without packs).
std::expected<std::shared_ptr<const gameplay::GameData>, std::string> load_game_data(const contracts::VersionBundle& bundle,
                                                                                      const data::PackOptions& options);

} // namespace circuit::server
