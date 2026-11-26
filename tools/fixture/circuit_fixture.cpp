// SPDX-License-Identifier: AGPL-3.0-only
// circuit-fixture: records, checks and compares circuit-trace fixtures with the
// reference rules on the oracle (the sequential executor in vanilla order).
// It stands in for the fixture commands of the first generation's server
// binary until the new runtime has its own command line.
#include "../../tests/oracle/fixture_executor.hpp"
#include <circuit/contracts/version.hpp>
#include <circuit/evidence/fixture_runner.hpp>
#include <circuit/evidence/trace_format.hpp>
#include <circuit/server/data_report.hpp>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace circuit;

constexpr const char* usage = "usage: circuit-fixture [--vanilla-jar <jar>] [--data-pack <dir>]... <command>\n"
                              "  --check-fixture <file> [--check-fixture <file> ...]\n"
                              "  --record-fixture <skeleton> --output <file>\n"
                              "  --diff-fixtures <first> <second> [--tolerance <blocks>]\n"
                              "  --compatibility [--game-version java-1.21.8]\n"
                              "--workers is accepted and ignored: the oracle runs on one thread.\n";

std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open fixture: " + path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The data packs' data for each bundle, loaded once; none without packs.
evidence::DataSource data_source(const data::PackOptions& packs) {
    if (packs.vanilla_jar.empty() && packs.packs.empty()) return {};
    auto cache = std::make_shared<std::map<std::string, std::shared_ptr<const gameplay::GameData>>>();
    return [packs, cache](const contracts::VersionBundle& bundle) -> std::shared_ptr<const gameplay::GameData> {
        const std::string id(bundle.id);
        if (const auto it = cache->find(id); it != cache->end()) return it->second;
        auto loaded = server::load_game_data(bundle, packs);
        if (!loaded) throw std::runtime_error("data packs: " + loaded.error());
        return cache->emplace(id, *loaded).first->second;
    };
}

int check_fixtures(const std::vector<std::string>& paths, const data::PackOptions& packs) {
    int status = 0;
    const auto executor = oracle::oracle_executor(data_source(packs));
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
        if (report.outcome == evidence::FixtureReport::Outcome::failed || report.outcome == evidence::FixtureReport::Outcome::invalid)
            status = 1;
    }
    std::cout << "Fixtures check the reference rules; only harness_trace fixtures can support compatibility claims.\n";
    return status;
}

int record_fixture_file(const std::string& skeleton, const std::string& output, const data::PackOptions& packs) {
    const auto parsed = evidence::parse_fixture(read_text(skeleton));
    if (!parsed) throw std::runtime_error(std::format("{}:{}: {}", skeleton, parsed.error().line, parsed.error().message));
    const auto recorded = evidence::record_fixture(*parsed, oracle::oracle_executor(data_source(packs)));
    if (!recorded) throw std::runtime_error(std::format("cannot record {}: {}", skeleton, recorded.error()));
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    out << evidence::write_fixture(*recorded);
    if (!out) throw std::runtime_error("cannot write fixture: " + output);
    std::cout << std::format("recorded {} -> {} (source circuit_reference)\n", skeleton, output);
    return 0;
}

int diff_fixtures(const std::string& first, const std::string& second, double tolerance) {
    const auto a = evidence::parse_fixture(read_text(first));
    const auto b = evidence::parse_fixture(read_text(second));
    if (!a || !b) {
        const auto& [path, error] = !a ? std::pair{first, a.error()} : std::pair{second, b.error()};
        std::cout << std::format("ERROR {}:{}: {}\n", path, error.line, error.message);
        return 2;
    }
    if (const auto difference = evidence::first_difference(*a, *b, tolerance)) {
        std::cout << std::format("DIFFER {} / {}: {}\n", first, second, *difference);
        return 1;
    }
    std::cout << std::format("SAME {} / {}\n", first, second);
    return 0;
}

int print_compatibility(const std::string& bundle_id) {
    const auto bundle = contracts::BundleRegistry::select_for_simulation(bundle_id);
    if (!bundle) {
        std::cerr << std::format("circuit-fixture: {} has no executable rules\n", bundle_id);
        return 2;
    }
    const auto& rules = *(*bundle)->rules;
    const auto& schema = (*bundle)->schema;
    std::size_t experimental = 0;
    for (const auto& r : schema.game_rules) experimental += r.experimental ? 1 : 0;
    std::cout << std::format("{}: content {}, component schema of protocol {}, data packs {}, resource packs {}, "
                             "registries {}, {} game rules ({} experimental)\n",
                             bundle_id, schema.content_release, schema.component_schema, schema.data_pack_format, schema.resource_pack_format,
                             schema.registries, schema.game_rules.size(), experimental);
    std::cout << std::format("rules {}, block registry {}; basis: {}\n", rules.id, rules.block_registry, rules.basis);
    std::cout << std::format("{}: {} (mechanism, target tier, claimed tier, evidence)\n", bundle_id, rules.id);
    for (const auto& m : rules.mechanisms)
        std::cout << std::format("{:<21} {}  {}  {}\n    {}\n", m.id, contracts::tier_name(m.target),
                                 m.claimed ? contracts::tier_name(*m.claimed) : "--", evidence::source_name(m.evidence), m.limitation);
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        data::PackOptions packs;
        std::vector<std::string> checks;
        std::string record, output, diff_first, diff_second, game_version = "java-1.21.8";
        double tolerance = 0.0;
        bool compatibility = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::invalid_argument(arg + " needs a value");
                return argv[++i];
            };
            if (arg == "--check-fixture") checks.push_back(value());
            else if (arg == "--record-fixture") record = value();
            else if (arg == "--output") output = value();
            else if (arg == "--diff-fixtures") {
                diff_first = value();
                diff_second = value();
            } else if (arg == "--tolerance") tolerance = std::stod(value());
            else if (arg == "--compatibility") compatibility = true;
            else if (arg == "--game-version") game_version = value();
            else if (arg == "--vanilla-jar") packs.vanilla_jar = value();
            else if (arg == "--data-pack") packs.packs.emplace_back(value());
            else if (arg == "--workers") value();
            else if (arg == "--help" || arg == "-h") {
                std::cout << usage;
                return 0;
            } else throw std::invalid_argument("unknown argument " + arg);
        }
        if (!record.empty()) {
            if (output.empty()) throw std::invalid_argument("--record-fixture requires --output");
            return record_fixture_file(record, output, packs);
        }
        if (!diff_first.empty()) return diff_fixtures(diff_first, diff_second, tolerance);
        if (!checks.empty()) return check_fixtures(checks, packs);
        if (compatibility) return print_compatibility(game_version);
        std::cerr << usage;
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "circuit-fixture: " << e.what() << '\n';
        return 2;
    }
}
