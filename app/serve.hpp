// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/server/server.hpp>

namespace circuit::app {

// Runs the network server until SIGINT or SIGTERM, then stops it gracefully.
// Returns a process exit code: non-zero when the world cannot be opened or
// the server stopped after a failure.
int serve(server::ServerConfig config);

// Converts between Anvil worlds and the configured world directory, printing
// what was converted and what was not. Return process exit codes.
int import_anvil(const std::string& source, const server::ServerConfig& config, bool substitute);
int export_anvil(const server::ServerConfig& config, const std::string& target);

} // namespace circuit::app
