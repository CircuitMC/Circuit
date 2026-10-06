// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
#include "fixtures.hpp"
#include <circuit/evidence/fixture_runner.hpp>
#include <circuit/evidence/trace_format.hpp>
#include <circuit/runtime/engine.hpp>
#include <chrono>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace circuit::app {
namespace {
std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open fixture: " + path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

int check_fixtures(const std::vector<std::string>& paths, unsigned workers) {
    int status = 0;
    const auto executor = evidence::engine_executor(workers);
    for (const auto& path : paths) {
        const auto parsed = evidence::parse_fixture(read_text(path));
        if (!parsed) {
            std::cout << std::format("ERROR {}:{}: {}\n", path, parsed.error().line, parsed.error().message);
            status = 1;
            continue;
        }
        const auto report = evidence::evaluate_fixture(*parsed, executor);
        std::cout << std::format("{} {} [{}; {}] {}\n", evidence::outcome_name(report.outcome), path,
                                 evidence::source_name(parsed->source), parsed->game_version, report.detail);
        if (report.outcome == evidence::FixtureReport::Outcome::failed
            || report.outcome == evidence::FixtureReport::Outcome::invalid)
            status = 1;
    }
    std::cout << "Fixtures check the reference rules; only harness_trace fixtures can support compatibility claims.\n";
    return status;
}

int profile_fixture(const std::string& path, unsigned ticks, unsigned workers) {
    const auto parsed = evidence::parse_fixture(read_text(path));
    if (!parsed) throw std::runtime_error(std::format("{}:{}: {}", path, parsed.error().line, parsed.error().message));
    const auto inputs = evidence::fixture_inputs(*parsed);
    if (!inputs) throw std::runtime_error(std::format("cannot run {}: {}", path, inputs.error()));
    runtime::EngineOptions options;
    options.workers = workers;
    options.record_trace = false;
    options.measure_tasks = true;
    runtime::Engine engine(inputs->bundle, 0, options);
    const std::vector<contracts::Command> none;
    const auto step = [&] {
        const auto it = inputs->by_tick.find(engine.time() + 1);
        engine.advance(it == inputs->by_tick.end() ? none : it->second);
    };
    while (engine.time() < parsed->ticks + 2) step(); // Load, setup and settle.
    const auto before = engine.stats();
    std::vector<double> durations;
    durations.reserve(ticks);
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < ticks; ++i) {
        const auto t = std::chrono::steady_clock::now();
        step();
        durations.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
    }
    const auto total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const auto& after = engine.stats();
    std::ranges::sort(durations);
    const auto at = [&](double q) { return durations[std::min(durations.size() - 1, static_cast<std::size_t>(q * static_cast<double>(durations.size())))]; };
    std::cout << std::format("profile {} ticks={} workers={} total_ms={:.1f} mean_tick_ms={:.3f} p50={:.3f} p95={:.3f} p99={:.3f} max={:.3f} "
                             "regions={} max_parallel_tasks={}\n",
                             path, ticks, workers, total, total / ticks, at(0.50), at(0.95), at(0.99), durations.back(), engine.region_count(),
                             after.max_tasks_in_phase);
    double phases = 0;
    for (std::size_t p = 1; p < contracts::phase_order.size(); ++p) {
        const auto ms = static_cast<double>(after.phase_nanoseconds[p] - before.phase_nanoseconds[p]) / 1e6;
        phases += ms;
        std::cout << std::format("  phase {:<15} {:9.1f} ms {:5.1f}%\n", contracts::phase_name(contracts::phase_order[p]), ms, 100.0 * ms / total);
    }
    // Where the time of the parallel phases goes: rule work, the critical path
    // (the longest task of each runner call) and coordination around it.
    const auto ms = [](std::uint64_t ns) { return static_cast<double>(ns) / 1e6; };
    const auto work = ms(after.work_nanoseconds - before.work_nanoseconds);
    const auto critical = ms(after.critical_nanoseconds - before.critical_nanoseconds);
    const auto runner = ms(after.runner_nanoseconds - before.runner_nanoseconds);
    const auto serial = ms(after.phase_nanoseconds[0] - before.phase_nanoseconds[0]);
    const auto commit = ms(after.commit_nanoseconds - before.commit_nanoseconds);
    std::cout << std::format("  rule work {:.1f} ms; critical path {:.1f} ms; in the job runner {:.1f} ms; commands phase {:.1f} ms; "
                             "coordination outside the runner {:.1f} ms, of which commit {:.1f} ms; work per worker-ms in the runner {:.2f}; "
                             "layout {:.1f} ms\n",
                             work, critical, runner, serial, phases - runner, commit, runner > 0 ? work / (runner * workers) : 0.0,
                             total - phases - serial);
    return 0;
}

int record_fixture_file(const std::string& skeleton, const std::string& output, unsigned workers) {
    const auto parsed = evidence::parse_fixture(read_text(skeleton));
    if (!parsed) throw std::runtime_error(std::format("{}:{}: {}", skeleton, parsed.error().line, parsed.error().message));
    const auto recorded = evidence::record_fixture(*parsed, evidence::engine_executor(workers));
    if (!recorded) throw std::runtime_error(std::format("cannot record {}: {}", skeleton, recorded.error()));
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    out << evidence::write_fixture(*recorded);
    if (!out) throw std::runtime_error("cannot write fixture: " + output);
    std::cout << std::format("recorded {} -> {} (source circuit_reference)\n", skeleton, output);
    return 0;
}

int print_compatibility(const std::string& bundle_id) {
    const auto bundle = contracts::BundleRegistry::select_for_simulation(bundle_id);
    if (!bundle) {
        std::cerr << std::format("Circuit: {} has no executable rules\n", bundle_id);
        return 2;
    }
    const auto& rules = *(*bundle)->rules;
    std::cout << std::format("{}: {} (mechanism, target tier, claimed tier, evidence)\n", bundle_id, rules.id);
    for (const auto& m : rules.mechanisms)
        std::cout << std::format("{:<21} {}  {}  {}\n    {}\n", m.id, contracts::tier_name(m.target),
                                 m.claimed ? contracts::tier_name(*m.claimed) : "--", evidence::source_name(m.evidence),
                                 m.limitation);
    return 0;
}

} // namespace circuit::app
