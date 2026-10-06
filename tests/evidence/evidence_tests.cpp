// SPDX-License-Identifier: AGPL-3.0-only
// circuit-trace fixtures: format, content mapping, recording and evaluation
// on both the runtime engine and the independent oracle.
#include "../oracle/oracle.hpp"
#include "../support/check.hpp"
#include "../support/scenario.hpp"
#include <circuit/evidence/content.hpp>
#include <circuit/evidence/fixture_runner.hpp>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>

using namespace circuit::evidence;
using circuit::oracle::Oracle;
using circuit::test::require;
using Outcome = FixtureReport::Outcome;
namespace fs = std::filesystem;

namespace {
class OracleExecutor final : public FixtureExecutor {
public:
    OracleExecutor(const circuit::contracts::VersionBundle& bundle, std::uint64_t seed) : oracle_(bundle, seed) {}
    void advance(const std::vector<circuit::contracts::Command>& commands) override {
        oracle_.advance(commands);
        dump_ = oracle_.dump();
    }
    const std::vector<circuit::contracts::TraceRecord>& trace() const override { return oracle_.trace(); }
    circuit::gameplay::BlockState block(BlockPos pos) const override { return circuit::test::state_at(dump_, pos); }
    std::optional<circuit::contracts::ItemStack> slot(BlockPos pos, std::uint8_t slot) const override {
        const auto* be = circuit::test::block_entity_at(dump_, pos);
        if (!be || slot >= be->slots.size()) return std::nullopt;
        return be->slots[slot];
    }

private:
    Oracle oracle_;
    circuit::gameplay::WorldDump dump_;
};

ExecutorFactory oracle_executor() {
    return [](const circuit::contracts::VersionBundle& b, std::uint64_t seed) -> std::unique_ptr<FixtureExecutor> {
        return std::make_unique<OracleExecutor>(b, seed);
    };
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Fixture parse(std::string_view text) {
    const auto f = parse_fixture(text);
    if (!f) throw std::runtime_error(std::format("parse error at line {}: {}", f.error().line, f.error().message));
    return *f;
}

const char* header = "circuit-trace 1\ntitle Test\ngame-version 1.21.8\nsource circuit_reference\nharness -\n"
                     "mechanisms piston\narea -2 0 -2 2 1 2\nticks 4\n";

const char* skeleton_text = R"(circuit-trace 1
title Two pistons and a lamp
game-version 1.21.8
source circuit_reference
harness -
mechanisms piston redstone_lamp
area -3 0 -3 3 1 3
ticks 6
changes ordered

at 0 set 1 0 0 minecraft:stone
at 0 set 0 0 0 minecraft:piston[extended=false,facing=east]
at 0 set -1 0 1 minecraft:redstone_lamp[lit=false]
at 0 set 0 0 2 minecraft:piston[extended=false,facing=south]
at 1 set -1 0 0 minecraft:lever[face=floor,facing=east,powered=true]
at 3 use -1 0 0
block 1 2 0 0 ?
block 1 -1 0 1 ?
block 4 1 0 0 ?
block 5 -1 0 1 ?
)";
} // namespace

int main(int argc, char** argv) {
    circuit::test::Suite suite("evidence_tests");
    const fs::path fixtures = argc > 1 ? fs::path(argv[1]) : fs::path("tests/fixtures");

    suite.run("state notation", [] {
        const auto s = parse_state("minecraft:repeater[delay=1,facing=west,powered=false]");
        require(s && s->id == "minecraft:repeater" && s->properties.size() == 3, "valid state");
        require(parse_state("minecraft:stone").has_value(), "no properties");
        for (auto bad : {"stone", ":stone", "minecraft:", "minecraft:Stone", "minecraft:a[", "minecraft:a[]", "minecraft:a[x]",
                         "minecraft:a[x=1,x=2]", "minecraft:a[x=1,]", "minecraft:a:b", "minecraft:a[x=A]"})
            require(!parse_state(bad).has_value(), std::format("rejects {}", bad));
    });

    suite.run("format errors carry line numbers", [] {
        const auto expect = [](std::string text, std::size_t line, std::string_view fragment) {
            const auto f = parse_fixture(text);
            require(!f && f.error().line == line && f.error().message.find(fragment) != std::string::npos,
                    std::format("expected line {} `{}`, got {}", line, fragment,
                                f ? std::string("success") : std::format("{} {}", f.error().line, f.error().message)));
        };
        const std::string h = header;
        expect("circuit-trace 2\n", 1, "unsupported format");
        expect("title x\n", 1, "first line");
        expect("circuit-trace 1\ntitle a\ntitle b\n", 3, "duplicate");
        expect("circuit-trace 1\ntitle a\nat 0 use 0 0 0\n", 3, "missing required key `game-version`");
        expect(h + "bogus 1\n", 9, "unknown key");
        expect(h + "neighbor-update-limit 0\n", 9, "invalid neighbor update limit");
        expect(h + "at 0 use 0 0 0\nticks 5\n", 10, "after the body");
        expect(h + "at 5 use 0 0 0\n", 9, "tick outside");
        expect(h + "at 0 use 9 0 0\n", 9, "outside the area");
        expect(h + "change 1 0 0 0 minecraft:air minecraft:stone\n", 9, "require `changes");
        expect(h + "at 0 set 0 0 0 minecraft:stone[\n", 9, "unterminated");
        expect(h + "at 2 use 0 0 0\nat 1 use 0 0 0\n", 10, "tick order");
        expect(h + "slot 1 0 0 0 0 empty 3\n", 9, "count 0");
        expect(h + "at 0 insert 0 0 0 0 minecraft:stone 65\n", 9, "invalid count");
        expect(std::string(header).replace(std::string(header).find("area"), 18, "area 0 0 0 200 0 0"), 7, "extent");
    });

    suite.run("format limits", [] {
        std::string big = header;
        for (int i = 0; i < 20; ++i) big += "block 1 0 0 0 ?\n";
        require(parse_fixture(big, {.max_lines = 10}).error().message.find("too many lines") != std::string::npos, "lines");
        require(parse_fixture(big, {.max_bytes = 64}).error().line == 0, "bytes");
        require(parse_fixture(std::string(header) + "# " + std::string(5000, 'c') + "\n").error().message == "line too long", "length");
    });

    suite.run("canonical text round trip", [&] {
        const auto f = parse(read_text(fixtures / "documented" / "lamp_timing.ctrace"));
        const auto text = write_fixture(f);
        require(write_fixture(parse(text)) == text, "write(parse(write(f))) is stable");
        require(f.source == circuit::contracts::EvidenceSource::documented_behavior && !f.is_skeleton(), "fields");
    });

    suite.run("content mapping round trips every reference state", [] {
        const auto* m = ContentMapping::for_game_version("1.21.8");
        require(m && !ContentMapping::for_game_version("1.20.4"), "mapping availability");
        unsigned states = 0;
        for (std::uint32_t raw = 0; raw < (1u << 18); ++raw) {
            const auto s = circuit::gameplay::BlockState::decode(raw);
            if (!s) continue;
            ++states;
            const auto back = m->block(m->notation(*s));
            require(back && *back == *s, std::format("round trip {} via {}", raw, m->notation(*s)));
        }
        require(states > 100, "enumerated registry");
        for (std::uint16_t i = 1; i <= circuit::gameplay::items::max_id; ++i)
            require(m->item(m->item_name(i)) == i, "item round trip");
    });

    suite.run("unrepresentable content is reported", [] {
        const auto* m = ContentMapping::for_game_version("1.21.8");
        const auto unsupported = [&](std::string_view s) {
            const auto r = m->block(s);
            require(!r && r.error().find("unsupported") != std::string::npos, std::format("{} -> {}", s, r ? "ok" : r.error()));
        };
        unsupported("minecraft:repeater[delay=1,facing=west,locked=true,powered=false]");
        unsupported("minecraft:chest[facing=north,type=single,waterlogged=true]");
        unsupported("minecraft:trapped_chest[facing=north,type=single,waterlogged=false]");
        require(m->block("minecraft:chest[facing=north,type=left,waterlogged=false]")
                    == circuit::gameplay::blocks::chest(circuit::contracts::Direction::north, circuit::gameplay::blocks::ChestType::left),
                "double chest halves are modeled");
        require(m->block("minecraft:comparator[facing=north,mode=subtract,powered=true]")
                    == circuit::gameplay::blocks::comparator(circuit::contracts::Direction::south, true, true),
                "comparator facing names the input side (unverified convention)");
        unsupported("minecraft:dirt");
        require(!m->block("minecraft:stone[snowy=true]").has_value(), "unknown property");
        require(!m->block("minecraft:repeater[delay=1,facing=west]").has_value(), "missing modeled property");
        require(!m->block("minecraft:repeater[delay=5,facing=west,powered=false]").has_value(), "range");
        require(m->block("minecraft:redstone_wire[east=side,north=none,power=7,south=up,west=none]")
                    == circuit::gameplay::blocks::wire(7), "dust shape is ignored");
        require(m->block("minecraft:repeater[delay=2,facing=west,locked=false,powered=true]")
                    == circuit::gameplay::blocks::repeater(circuit::contracts::Direction::east, 2, true),
                "repeater facing names the input side (unverified convention)");
        require(m->block("minecraft:water[level=12]") == circuit::gameplay::blocks::water_falling(), "falling water");
        require(m->block("minecraft:moving_piston[facing=east,type=sticky]")
                    == circuit::gameplay::blocks::moving_piston(circuit::contracts::Direction::east, true), "moving piston");
        require(!m->item("minecraft:diamond").has_value(), "unknown item");
    });

    suite.run("record and evaluate on engine and oracle", [] {
        const auto skeleton = parse(skeleton_text);
        require(skeleton.is_skeleton(), "placeholders");
        require(evaluate_fixture(skeleton, engine_executor()).outcome == Outcome::invalid, "skeletons are not evaluated");
        const auto by_engine = record_fixture(skeleton, engine_executor(4));
        const auto by_oracle = record_fixture(skeleton, oracle_executor());
        require(by_engine && by_oracle, "recorded");
        const auto text = write_fixture(*by_engine);
        require(text == write_fixture(*by_oracle), "engine and oracle record identical fixtures");
        const auto recorded = parse(text);
        require(recorded.source == circuit::contracts::EvidenceSource::circuit_reference
                    && recorded.harness == "circuit-reference-rules-v4", "provenance");
        require(!recorded.recorded_changes.empty(), "changes recorded");
        for (const auto& exec : {engine_executor(1), engine_executor(8), oracle_executor()}) {
            const auto r = evaluate_fixture(recorded, exec);
            require(r.outcome == Outcome::passed && r.changes_compared > 0 && r.probes_compared == 4, r.detail);
        }
    });

    suite.run("mismatches name the fixture line", [] {
        const auto text = write_fixture(*record_fixture(parse(skeleton_text), engine_executor()));
        // A wrong probe value.
        auto wrong = text;
        const auto probe = wrong.find("block 5 -1 0 1 minecraft:redstone_lamp[lit=");
        require(probe != std::string::npos, "probe present");
        const auto value = probe + std::string_view("block 5 -1 0 1 minecraft:redstone_lamp[lit=").size();
        if (wrong.compare(value, 4, "true") == 0) wrong.replace(value, 4, "false");
        else wrong.replace(value, 5, "true");
        auto r = evaluate_fixture(parse(wrong), engine_executor());
        require(r.outcome == Outcome::failed && r.detail.find("block probe (line") != std::string::npos, r.detail);
        // Two adjacent changes swapped: ordered fails, unordered passes.
        auto lines = std::vector<std::string>{};
        for (std::size_t start = 0; start < text.size();) {
            const auto end = text.find('\n', start);
            lines.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        std::size_t first = 0;
        while (!lines[first].starts_with("change 1")) ++first;
        require(lines[first + 1].starts_with("change 1"), "two changes in tick 1");
        std::swap(lines[first], lines[first + 1]);
        std::string swapped;
        for (const auto& l : lines) swapped += l + "\n";
        r = evaluate_fixture(parse(swapped), engine_executor());
        require(r.outcome == Outcome::failed && r.detail.find("change 0") != std::string::npos, r.detail);
        auto unordered = swapped;
        unordered.replace(unordered.find("changes ordered"), 15, "changes unordered");
        require(evaluate_fixture(parse(unordered), engine_executor()).outcome == Outcome::passed, "unordered");
        // A missing change.
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(first));
        std::string missing;
        for (const auto& l : lines) missing += l + "\n";
        r = evaluate_fixture(parse(missing), engine_executor());
        require(r.outcome == Outcome::failed && r.detail.find("expected") != std::string::npos, r.detail);
    });

    suite.run("unsupported and invalid fixtures", [] {
        const std::string h = header;
        auto r = evaluate_fixture(parse(h + "changes ordered\nchange 1 0 0 0 minecraft:air minecraft:dirt\n"),
                                  engine_executor());
        require(r.outcome == Outcome::unsupported && r.detail.find("line 10") != std::string::npos, r.detail);
        r = evaluate_fixture(parse(h + "at 0 set 0 0 0 minecraft:piston[extended=true,facing=up]\n"), engine_executor());
        require(r.outcome == Outcome::unsupported && r.detail.find("rejected") != std::string::npos, r.detail);
        auto other = h;
        other.replace(other.find("mechanisms piston"), 17, "mechanisms teleport");
        require(evaluate_fixture(parse(other), engine_executor()).outcome == Outcome::invalid, "unknown mechanism");
        other = h;
        other.replace(other.find("1.21.8"), 6, "1.20.4");
        require(evaluate_fixture(parse(other), engine_executor()).outcome == Outcome::unsupported, "no mapping");
    });

    suite.run("repository fixtures", [&] {
        std::size_t evaluated = 0, rerecorded = 0;
        for (const auto* dir : {"documented", "reference"})
            for (const auto& entry : fs::directory_iterator(fixtures / dir)) {
                const auto f = parse(read_text(entry.path()));
                for (const auto& exec : {engine_executor(4), oracle_executor()}) {
                    const auto r = evaluate_fixture(f, exec);
                    require(r.outcome == Outcome::passed, std::format("{}: {}", entry.path().string(), r.detail));
                }
                ++evaluated;
            }
        for (const auto& entry : fs::directory_iterator(fixtures / "skeletons")) {
            const auto recorded = record_fixture(parse(read_text(entry.path())), engine_executor());
            require(recorded.has_value(), entry.path().string());
            const auto committed = fixtures / "reference" / entry.path().filename();
            require(write_fixture(*recorded) == read_text(committed),
                    std::format("{} no longer matches the reference rules; re-record and review it", committed.string()));
            ++rerecorded;
        }
        require(evaluated >= 9 && rerecorded >= 8, std::format("{} evaluated, {} re-recorded", evaluated, rerecorded));
    });

    suite.run("profiling workloads are valid scenarios", [&] {
        std::size_t workloads = 0;
        for (const auto& entry : fs::directory_iterator(fixtures / "workloads")) {
            const auto f = parse(read_text(entry.path()));
            require(f.block_probes.empty() && f.slot_probes.empty() && !f.actions.empty(), entry.path().string());
            require(fixture_inputs(f).has_value(), std::format("{}: cannot run", entry.path().string()));
            ++workloads;
        }
        require(workloads >= 5, "workloads present");
    });

    suite.run("target-version traces pass and match their skeletons", [&] {
        std::size_t traces = 0;
        for (const auto& entry : fs::directory_iterator(fixtures / "harness")) {
            const auto trace = parse(read_text(entry.path()));
            require(trace.source == circuit::contracts::EvidenceSource::harness_trace && trace.harness.starts_with("circuit-harness:"),
                    std::format("{}: harness provenance", entry.path().string()));
            // The trace must come from the scenario currently in the repository.
            auto origin = fixtures / "skeletons" / entry.path().filename();
            if (!fs::exists(origin)) origin = fixtures / "documented" / entry.path().filename();
            require(fs::exists(origin), std::format("{}: no skeleton", entry.path().string()));
            const auto skeleton = parse(read_text(origin));
            auto actions = [](const Fixture& f) {
                std::vector<std::tuple<Tick, int, BlockPos, std::string, int, std::string, int>> out;
                for (const auto& a : f.actions)
                    out.emplace_back(a.tick, static_cast<int>(a.kind), a.pos, a.state, a.slot, a.item, a.count);
                return out;
            };
            require(trace.title == skeleton.title && trace.area_min == skeleton.area_min && trace.area_max == skeleton.area_max
                        && trace.ticks == skeleton.ticks && trace.changes == skeleton.changes
                        && trace.random_tick_speed == skeleton.random_tick_speed
                        && trace.neighbor_update_limit == skeleton.neighbor_update_limit && actions(trace) == actions(skeleton),
                    std::format("{}: recorded from a different scenario; re-record it", entry.path().string()));
            for (const auto& exec : {engine_executor(4), oracle_executor()}) {
                const auto r = evaluate_fixture(trace, exec);
                require(r.outcome == Outcome::passed, std::format("{}: {}", entry.path().string(), r.detail));
            }
            ++traces;
        }
        require(traces >= 20, std::format("{} target-version traces", traces));
    });

    return suite.finish();
}
