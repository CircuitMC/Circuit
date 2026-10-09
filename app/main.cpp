// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/plugin/basic_host.hpp>
#include <circuit/plugin/loader.hpp>
#include <circuit/protocol.hpp>
#include <circuit/runtime/demo_kernel.hpp>
#include "fixtures.hpp"
#include "serve.hpp"
#include <circuit/server/anvil.hpp>
#include "simulate.hpp"
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <charconv>
#include <cstdint>
#include <chrono>
#include <csignal>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
using namespace std::chrono;
struct Host final : circuit::plugin::HostWithoutWorld {
    circuit::plugin::TickMetrics metrics;
    void log(circuit::plugin::LogLevel, std::string_view message) noexcept override {
        std::cout << "[plugin] " << message << '\n';
    }
    circuit::plugin::TickMetrics tick_metrics() const noexcept override { return metrics; }
};
unsigned number(std::string_view input, unsigned low, unsigned high) {
    unsigned value{};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
    if (error != std::errc{} || end != input.data() + input.size() || value < low || value > high)
        throw std::invalid_argument("numeric argument outside supported range");
    return value;
}
void usage() {
    std::cout << "Circuit 0.1.0 — region-parallel simulation and Java Edition 1.21.8 / 1.21.4 server\n"
        "  --simulate [--game-version java-1.21.8] [--workers 1..64] [--ticks 1..1000000]\n"
        "             [--seed N] [--replay <file>] [--record <file>] [--realtime]\n"
        "  --check-fixture <file> [--check-fixture <file> ...] [--workers 1..64]\n"
        "  --record-fixture <skeleton> --output <file> [--workers 1..64]\n"
        "  --diff-fixtures <first> <second> [--tolerance <blocks>]\n"
        "  --profile-fixture <file> [--ticks N] [--workers 1..64]\n"
        "  --compatibility [--game-version java-1.21.8]\n"
        "  --serve [--config <file>] [--world <dir>] [--port N] [--offline] [--workers 1..64]\n"
        "  --import-anvil <java-world> --world <dir> [--config <file>] [--substitute-unsupported]\n"
        "  --export-anvil <new-java-world> --world <dir> [--config <file>]\n"
        "  --migrate-world <new-dir> --world <dir> --game-version java-1.21.8|java-1.21.4\n"
        "  --demo [--game-version java-1.21.8|java-1.20.4]\n"
        "         [--workers 1..64] [--ticks 1..1000000] [--plugin <shared-library>]\n"
        "  --list-versions | --version | --help\n";
}
}
int main(int argc, char** argv) {
    try {
        unsigned workers = 4, ticks = 20;
        bool demo = false, simulate = false, ticks_given = false, workers_given = false, compatibility = false, serve = false,
             offline = false;
        std::string config_path, world_directory, import_source, export_target, migrate_target;
        bool version_given = false;
        bool substitute = false;
        std::optional<std::uint16_t> port;
        circuit::app::SimulateOptions simulation;
        std::vector<std::string> fixtures;
        std::string record_fixture, fixture_output, profile, diff_first, diff_second;
        double tolerance = 0.0;
        std::string selected = "java-1.21.8";
        std::vector<std::string> libraries;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            const auto value = [&]() -> std::string_view {
                if (i + 1 >= argc) throw std::invalid_argument("missing argument value");
                return argv[++i];
            };
            if (arg == "--help") { usage(); return 0; }
            if (arg == "--version") {
                std::cout << "Circuit/0.1.0; AGPL-3.0-only; bundles java-1.21.8, java-1.21.4; public build identifier\n";
                return 0;
            }
            if (arg == "--list-versions") {
                for (const auto& v : circuit::protocol::builtin_registry::candidates)
                    std::cout << v.id << (v.status == circuit::protocol::implementation_status::supported ? " supported" : " planned")
                              << "; protocol=" << (v.protocol_number ? std::to_string(*v.protocol_number) : std::string("-"))
                              << "; play=" << (v.supports_play ? "true" : "false") << '\n';
                return 0;
            }
            if (arg == "--demo") demo = true;
            else if (arg == "--simulate") simulate = true;
            else if (arg == "--workers") { workers = number(value(), 1, 64); workers_given = true; }
            else if (arg == "--ticks") { ticks = number(value(), 1, 1000000); ticks_given = true; }
            else if (arg == "--seed") simulation.seed = number(value(), 0, UINT32_MAX);
            else if (arg == "--replay") simulation.replay_path = value();
            else if (arg == "--record") simulation.record_path = value();
            else if (arg == "--realtime") simulation.realtime = true;
            else if (arg == "--check-fixture") fixtures.emplace_back(value());
            else if (arg == "--record-fixture") record_fixture = value();
            else if (arg == "--diff-fixtures") {
                diff_first = value();
                diff_second = value();
            } else if (arg == "--tolerance") {
                tolerance = std::stod(std::string(value()));
                if (!(tolerance >= 0.0 && tolerance <= 1.0)) throw std::invalid_argument("--tolerance must be within 0-1");
            }
            else if (arg == "--output") fixture_output = value();
            else if (arg == "--profile-fixture") profile = value();
            else if (arg == "--compatibility") compatibility = true;
            else if (arg == "--serve") serve = true;
            else if (arg == "--config") config_path = value();
            else if (arg == "--port") port = static_cast<std::uint16_t>(number(value(), 0, 65535));
            else if (arg == "--offline") offline = true;
            else if (arg == "--world") world_directory = value();
            else if (arg == "--import-anvil") import_source = value();
            else if (arg == "--export-anvil") export_target = value();
            else if (arg == "--substitute-unsupported") substitute = true;
            else if (arg == "--game-version") { selected = value(); version_given = true; }
            else if (arg == "--migrate-world") migrate_target = value();
            else if (arg == "--plugin") libraries.emplace_back(value());
            else throw std::invalid_argument("unknown option: " + std::string(arg));
        }
        const auto version = circuit::protocol::builtin_registry::select(selected);
        if (!version) throw std::invalid_argument("unknown game version: " + selected);
        if (simulate && demo) throw std::invalid_argument("--simulate and --demo are exclusive");
        if (!record_fixture.empty()) {
            if (fixture_output.empty()) throw std::invalid_argument("--record-fixture requires --output");
            return circuit::app::record_fixture_file(record_fixture, fixture_output, workers);
        }
        if (!diff_first.empty()) return circuit::app::diff_fixtures(diff_first, diff_second, tolerance);
        if (!fixtures.empty()) return circuit::app::check_fixtures(fixtures, workers);
        if (!profile.empty()) return circuit::app::profile_fixture(profile, ticks_given ? ticks : 600, workers);
        if (compatibility) return circuit::app::print_compatibility(selected);
        if (!migrate_target.empty()) {
            if (world_directory.empty() || !version_given) throw std::invalid_argument("--migrate-world needs --world and --game-version");
            const auto tick = circuit::server::migrate_world(world_directory, migrate_target, selected);
            if (!tick) {
                std::cerr << "circuit-server: migration failed: " << tick.error() << '\n';
                return 1;
            }
            std::cout << "migrated " << world_directory << " to " << migrate_target << " for " << selected << " at tick " << *tick << '\n';
            return 0;
        }
        if (serve || !import_source.empty() || !export_target.empty()) {
            if (static_cast<int>(serve) + !import_source.empty() + !export_target.empty() > 1)
                throw std::invalid_argument("--serve, --import-anvil and --export-anvil are exclusive");
            circuit::server::ServerConfig config;
            if (!config_path.empty()) {
                auto loaded = circuit::server::load_config(config_path);
                if (!loaded) throw std::invalid_argument(loaded.error());
                config = std::move(*loaded);
            }
            if (port) config.port = *port;
            if (offline) config.online_mode = false;
            if (!world_directory.empty()) config.world_directory = world_directory;
            if (version_given) config.game_version = selected;
            if (workers_given) config.workers = workers;
            if (!import_source.empty()) return circuit::app::import_anvil(import_source, config, substitute);
            if (!export_target.empty()) return circuit::app::export_anvil(config, export_target);
            return circuit::app::serve(std::move(config));
        }
        if (simulate) {
            simulation.game_version = selected;
            simulation.workers = workers;
            simulation.ticks = ticks_given ? ticks : 200;
            return circuit::app::run_simulation(simulation);
        }
        if (!demo) {
            usage();
            std::cerr << "Play protocol is not implemented. Use --simulate for the headless core or --demo for the "
                         "synthetic scheduler.\n";
            return 2;
        }
        Host host; // Must outlive every loaded plugin.
        std::vector<circuit::plugin::LoadedPlugin> plugins;
        for (const auto& library : libraries) {
            auto loaded = circuit::plugin::PluginLoader::load(
                library, host, circuit::plugin::LoadPhase::Registration);
            if (!loaded) throw std::runtime_error(loaded.error().message);
            plugins.push_back(std::move(*loaded));
        }
        circuit::runtime::DemoKernel kernel(workers);
        kernel.schedule(1, 0, 12);
        kernel.schedule(3, 2, 5);
        std::cout << "Circuit demo: " << version->descriptor().id << ", workers=" << workers
                  << "; synthetic rules only; no network port opened\n";
        asio::io_context control;
        asio::steady_timer timer(control);
        asio::signal_set signals(control, SIGINT, SIGTERM);
        signals.async_wait([&](const asio::error_code& error, int) {
            if (!error) { timer.cancel(); control.stop(); }
        });
        auto deadline = steady_clock::now();
        std::uint64_t elapsed_ns = 0;
        std::function<void(const asio::error_code&)> tick;
        tick = [&](const asio::error_code& error) {
            if (error == asio::error::operation_aborted) return;
            if (error) throw std::runtime_error(error.message());
            const auto start = steady_clock::now();
            kernel.advance();
            const auto cost = static_cast<std::uint64_t>(duration_cast<nanoseconds>(steady_clock::now() - start).count());
            elapsed_ns += cost;
            host.metrics = {kernel.snapshot().tick, cost, elapsed_ns / kernel.snapshot().tick};
            for (auto& plugin : plugins) plugin.tick();
            if (kernel.snapshot().tick >= ticks) { signals.cancel(); return; }
            // Keep a monotonic 20 Hz target; overload does not skip logical ticks.
            deadline += milliseconds(50);
            timer.expires_at(deadline);
            timer.async_wait(tick);
        };
        timer.expires_at(deadline);
        timer.async_wait(tick);
        control.run();
        std::cout << "committed_ticks=" << kernel.snapshot().tick
                  << " mean_simulation_ms=" << host.metrics.mean_tick_ns / 1000000.0 << '\n';
        for (std::size_t i = 0; i < kernel.snapshot().chunks.size(); ++i) {
            const auto& chunk = kernel.snapshot().chunks[i];
            std::cout << "chunk=" << i << " time_advances=" << chunk.time_advances << " signals=" << chunk.signals << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Circuit: " << error.what() << '\n';
        return 1;
    }
}
