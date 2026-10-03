// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/plugin/loader.hpp>
#include <circuit/protocol.hpp>
#include <circuit/runtime/demo_kernel.hpp>
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace std::chrono;
struct Host final : circuit::plugin::IHost {
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
    std::cout << "Circuit 0.0.1 — local framework prototype (no Minecraft listener)\n"
        "  --demo [--game-version java-1.21.8|java-1.20.4]\n"
        "         [--workers 1..64] [--ticks 1..1000000] [--plugin <shared-library>]\n"
        "  --list-versions | --version | --help\n";
}
}
int main(int argc, char** argv) {
    try {
        unsigned workers = 4, ticks = 20;
        bool demo = false;
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
                std::cout << "Circuit/0.0.1 framework; AGPL-3.0-only; public build identifier\n";
                return 0;
            }
            if (arg == "--list-versions") {
                for (const auto& v : circuit::protocol::builtin_registry::candidates)
                    std::cout << v.id << " planned; play=false\n";
                return 0;
            }
            if (arg == "--demo") demo = true;
            else if (arg == "--workers") workers = number(value(), 1, 64);
            else if (arg == "--ticks") ticks = number(value(), 1, 1000000);
            else if (arg == "--game-version") selected = value();
            else if (arg == "--plugin") libraries.emplace_back(value());
            else throw std::invalid_argument("unknown option: " + std::string(arg));
        }
        const auto version = circuit::protocol::builtin_registry::select(selected);
        if (!version) throw std::invalid_argument("unknown game version: " + selected);
        if (!demo) {
            usage();
            std::cerr << "Play protocol is not implemented. Use --demo for the synthetic scheduler.\n";
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
