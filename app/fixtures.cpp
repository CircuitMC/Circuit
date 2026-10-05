// SPDX-License-Identifier: AGPL-3.0-only
#include "fixtures.hpp"
#include <circuit/evidence/fixture_runner.hpp>
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

} // namespace circuit::app
