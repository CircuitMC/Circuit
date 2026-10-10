// SPDX-License-Identifier: AGPL-3.0-only
#include "simulate.hpp"
#include <circuit/contracts/replay.hpp>
#include <circuit/protocol/components.hpp>
#include <circuit/server/components.hpp>
#include <circuit/runtime/engine.hpp>
#include <circuit/runtime/tick_clock.hpp>
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <csignal>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>

namespace circuit::app {
namespace {
using namespace circuit::contracts;
using namespace circuit::gameplay;
using D = Direction;

CommandBody place(BlockPos p, BlockState s) { return SetBlock{p, s.raw(), std::nullopt}; }

// Built-in demonstration: machines that straddle chunk edges.
std::vector<CommandBody> demo_inputs(Tick t) {
    if (t == 1) return {AddTicket{1, {0, 0}, 2}};
    if (t == 2)
        return {place({13, 0, 0}, blocks::lever()), place({14, 0, 0}, blocks::wire()), place({15, 0, 0}, blocks::wire()),
                place({16, 0, 0}, blocks::wire()), place({17, 0, 0}, blocks::wire()),
                place({18, 0, 0}, blocks::repeater(D::east, 2)), place({19, 0, 0}, blocks::wire()),
                place({20, 0, 0}, blocks::lamp()), place({14, 0, 8}, blocks::lamp()), place({17, 0, 8}, blocks::lamp()),
                place({15, 0, 8}, blocks::observer(D::east)), place({16, 0, 8}, blocks::observer(D::west)),
                place({15, 1, 20}, blocks::chest()), InsertItem{{15, 1, 20}, 0, {items::iron_ingot, 64}},
                place({15, 0, 20}, blocks::hopper(D::east)), place({16, 0, 20}, blocks::hopper(D::east)),
                place({17, 0, 20}, blocks::chest())};
    if (t % 10 == 3) return {UseBlock{{13, 0, 0}}};
    return {};
}

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open replay: " + path);
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    for (std::size_t i = 0; i < chars.size(); ++i) bytes[i] = static_cast<std::byte>(chars[i]);
    return bytes;
}

void write_file(const std::string& path, const std::vector<std::byte>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write replay: " + path);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("failed writing replay: " + path);
}
} // namespace

int run_simulation(const SimulateOptions& options) {
    ReplayLog log;
    Tick last = 0;
    if (!options.replay_path.empty()) {
        const auto decoded = decode_replay(read_file(options.replay_path));
        if (!decoded) throw std::runtime_error(std::format("invalid replay: {}", error_name(decoded.error())));
        log = *decoded;
        last = log.ticks.empty() ? 0 : log.ticks.back().tick;
    } else {
        const auto selected = BundleRegistry::select_for_simulation(options.game_version);
        if (!selected)
            throw std::runtime_error(std::format("cannot simulate {}: {}", options.game_version, error_name(selected.error())));
        log.bundle_id = options.game_version;
        log.rules_id = std::string((*selected)->rules->id);
        log.block_registry_id = std::string((*selected)->rules->block_registry);
        log.seed = options.seed;
        std::uint64_t admission = 1;
        for (Tick t = 1; t <= options.ticks; ++t) {
            auto bodies = demo_inputs(t);
            if (bodies.empty() && t != options.ticks) continue; // The last tick is kept to preserve the run length.
            TickInput input{t, {}};
            for (auto& b : bodies) input.commands.push_back({admission++, std::move(b)});
            log.ticks.push_back(std::move(input));
        }
        last = options.ticks;
    }
    const auto bundle = BundleRegistry::select_for_simulation(log.bundle_id);
    if (!bundle) throw std::runtime_error(std::format("cannot simulate {}: {}", log.bundle_id, error_name(bundle.error())));
    if (const auto ok = check_replay_rules(log, **bundle); !ok)
        throw std::runtime_error(std::format("replay recorded under {} on {}; this build runs {}: {}", log.rules_id, log.bundle_id,
                                             (*bundle)->rules->id, error_name(ok.error())));
    // Item components in the record must be the bundle protocol's and valid.
    if (const auto* codec = circuit::protocol::ComponentCodec::for_protocol((*bundle)->protocol)) {
        const auto ctx = circuit::server::component_context((*bundle)->protocol);
        for (const auto& t : log.ticks)
            for (const auto& c : t.commands)
                std::visit([&](const auto& body) {
                    if constexpr (requires { body.stack; })
                        if (!body.stack.components.empty())
                            if (const auto checked = codec->check(body.stack.components, ctx); !checked)
                                throw std::runtime_error(std::format("invalid replay: tick {}: {}", t.tick, checked.error()));
                }, c.body);
    }
    if (!options.record_path.empty()) {
        const auto bytes = encode_replay(log);
        if (!bytes) throw std::runtime_error(std::format("cannot record replay: {}", error_name(bytes.error())));
        write_file(options.record_path, *bytes);
    }
    std::map<Tick, const std::vector<Command>*> inputs;
    for (const auto& t : log.ticks) inputs[t.tick] = &t.commands;

    runtime::EngineOptions engine_options;
    engine_options.workers = options.workers;
    engine_options.record_trace = false;
    runtime::Engine engine(**bundle, log.seed, engine_options);
    std::cout << std::format("Circuit headless simulation: {} with {} (not verified against the target version); "
                             "workers={} seed={} ticks={}; no network port opened\n",
                             (*bundle)->id, (*bundle)->rules->id, options.workers, log.seed, last);

    const std::vector<Command> none;
    const auto step = [&] {
        const auto it = inputs.find(engine.time() + 1);
        engine.advance(it == inputs.end() ? none : *it->second);
    };
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t overloads = 0, dropped = 0;
    if (!options.realtime) {
        while (engine.time() < last) step();
    } else {
        asio::io_context control;
        asio::steady_timer timer(control);
        asio::signal_set signals(control, SIGINT, SIGTERM);
        runtime::TickClock clock(std::chrono::steady_clock::now());
        signals.async_wait([&](const asio::error_code& error, int) {
            if (!error) {
                timer.cancel();
                control.stop();
            }
        });
        std::function<void(const asio::error_code&)> loop = [&](const asio::error_code& error) {
            if (error == asio::error::operation_aborted) return;
            if (error) throw std::runtime_error(error.message());
            while (engine.time() < last && clock.due(std::chrono::steady_clock::now())) {
                step();
                clock.ticked();
            }
            if (engine.time() >= last) {
                signals.cancel();
                return;
            }
            timer.expires_at(clock.deadline());
            timer.async_wait(loop);
        };
        timer.expires_at(clock.deadline());
        timer.async_wait(loop);
        control.run();
        overloads = clock.overloads();
        dropped = clock.dropped_backlog();
    }
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const auto dump = engine.dump();
    const auto& s = engine.stats();
    std::cout << std::format("committed_ticks={} elapsed_ms={:.1f} mean_tick_ms={:.3f}\n", engine.time(), ms,
                             engine.time() ? ms / static_cast<double>(engine.time()) : 0.0);
    std::size_t blocks = 0, block_entities = 0, entities = 0, ticks = 0, events = 0;
    for (const auto& l : dump.levels) {
        blocks += l.blocks.size();
        block_entities += l.block_entities.size();
        entities += l.entities.size();
        ticks += l.ticks.size();
        events += l.block_events.size();
    }
    std::cout << std::format("digest={:#018x} blocks={} block_entities={} entities={} pending_ticks={} pending_events={}\n",
                             digest(dump), blocks, block_entities, entities, ticks, events);
    std::cout << std::format("loaded_chunks={} regions={} phases={} attempts={} conflict_aborts={} merges={} splits={} "
                             "max_parallel_tasks={}\n",
                             engine.loaded_chunks(), engine.region_count(), s.phases, s.attempts, s.conflict_aborts,
                             s.merges, s.splits, s.max_tasks_in_phase);
    if (options.realtime) std::cout << std::format("overloads={} dropped_backlog_ticks={}\n", overloads, dropped);
    return 0;
}

} // namespace circuit::app
