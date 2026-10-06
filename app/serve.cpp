// SPDX-License-Identifier: AGPL-3.0-only
#include "serve.hpp"
#include <circuit/server/anvil.hpp>
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <csignal>
#include <iostream>
#include <optional>

namespace circuit::app {

int serve(server::ServerConfig config) {
    std::optional<server::Server> server;
    try {
        server.emplace(std::move(config));
    } catch (const std::exception& e) {
        std::cerr << "circuit-server: " << e.what() << '\n';
        return 1;
    }
    server->start();
    asio::io_context signals_io;
    asio::signal_set signals(signals_io, SIGINT, SIGTERM);
    bool signalled = false;
    signals.async_wait([&](const asio::error_code& ec, int) { signalled = !ec; });
    // Until the first signal, or until the server stops by itself.
    while (!signalled && server->running()) {
        signals_io.restart();
        signals_io.run_for(std::chrono::milliseconds(200));
    }
    const bool failed = !server->running();
    server->stop();
    return failed ? 1 : 0;
}

int import_anvil(const std::string& source, const server::ServerConfig& config, bool substitute) {
    server::anvil::ImportOptions options;
    options.unsupported = substitute ? server::anvil::Unsupported::substitute : server::anvil::Unsupported::reject;
    const auto report = server::import_anvil_world(source, config, options);
    if (!report) {
        std::cerr << "circuit-server: import failed: " << report.error() << '\n';
        return 1;
    }
    std::cout << "imported \"" << report->level_name << "\" into " << config.world_directory.string() << ": " << report->chunks
              << " chunks, " << report->blocks << " blocks, " << report->block_entities << " block entities, " << report->entities
              << " entities, " << report->scheduled_ticks << " scheduled ticks at tick " << report->time << '\n';
    for (const auto& [kind, count] : report->unsupported) std::cout << "  replaced or dropped: " << kind << " (" << count << ")\n";
    for (const auto& note : report->notes) std::cout << "  note: " << note << '\n';
    return 0;
}

int export_anvil(const server::ServerConfig& config, const std::string& target) {
    const auto report = server::export_anvil_world(config, target);
    if (!report) {
        std::cerr << "circuit-server: export failed: " << report.error() << '\n';
        return 1;
    }
    std::cout << "exported " << config.world_directory.string() << " to " << target << ": " << report->chunks << " chunks in "
              << report->regions << " region files, " << report->block_entities << " block entities, " << report->entities
              << " entities, " << report->scheduled_ticks << " scheduled ticks\n";
    for (const auto& note : report->notes) std::cout << "  note: " << note << '\n';
    return 0;
}

} // namespace circuit::app
