// SPDX-License-Identifier: AGPL-3.0-only
// circuit-trace fixtures: format, content mapping, recording and evaluation
// on both the runtime engine and the independent oracle.
#include "../oracle/oracle.hpp"
#include "../support/check.hpp"
#include "../support/light_reference.hpp"
#include "../support/scenario.hpp"
#include <circuit/evidence/content.hpp>
#include <circuit/evidence/fixture_runner.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/physics.hpp>
#include <circuit/gameplay/light.hpp>
#include <array>
#include <charconv>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

using namespace circuit::evidence;
using circuit::oracle::Oracle;
using circuit::test::require;
using Outcome = FixtureReport::Outcome;
using circuit::evidence::EntityEvent;
using circuit::evidence::EntityOrder;
using circuit::evidence::Vec3d;
namespace fs = std::filesystem;

namespace {
class OracleExecutor final : public FixtureExecutor {
public:
    // The overworld's level random starts from 0, as the reference harness seeds it.
    OracleExecutor(const circuit::contracts::VersionBundle& bundle, std::uint64_t seed)
        : oracle_(bundle, seed, {}, std::int64_t{0}), shape_(bundle.world) {}
    void advance(const std::vector<circuit::contracts::Command>& commands) override {
        before_ = std::move(dump_);
        oracle_.advance(commands);
        dump_ = oracle_.dump();
        light_.clear();
        world_.reset();
    }
    const std::vector<circuit::contracts::TraceRecord>& trace() const override { return oracle_.trace(); }
    circuit::gameplay::BlockState block(BlockPos pos) const override { return circuit::test::state_at(dump_, pos); }
    std::optional<circuit::contracts::ItemStack> slot(BlockPos pos, std::uint8_t slot) const override {
        const auto* be = circuit::test::block_entity_at(dump_, pos);
        if (!be || slot >= be->slots.size()) return std::nullopt;
        return be->slots[slot];
    }
    std::vector<circuit::gameplay::Entity> entities() const override { return dump_.overworld().entities; }
    // Light during a tick follows the blocks at the end of the one before: it
    // is computed from scratch from that dump.
    std::optional<std::pair<std::uint8_t, std::uint8_t>> light(BlockPos pos) const override {
        if (!world_) {
            world_.emplace(before_, circuit::contracts::DimensionId::overworld);
            top_ = std::min(shape_.min_y + shape_.height - 1, std::max(world_->highest(), shape_.min_y) + 16);
        }
        const auto chunk = circuit::contracts::chunk_of(pos);
        if (!world_->loaded(chunk)) return std::pair<std::uint8_t, std::uint8_t>{0, 0};
        if (pos.y > top_) return std::pair<std::uint8_t, std::uint8_t>{0, 15};
        // A column's light depends only on cells within 15 blocks: the
        // loaded columns around it.
        auto it = light_.find(chunk);
        if (it == light_.end()) {
            std::set<circuit::contracts::ChunkPos> around;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dz = -1; dz <= 1; ++dz)
                    if (world_->loaded({chunk.x + dx, chunk.z + dz})) around.insert({chunk.x + dx, chunk.z + dz});
            it = light_.try_emplace(chunk, *world_, around, shape_.min_y, top_).first;
        }
        using circuit::gameplay::LightKind;
        return std::pair{it->second.get(LightKind::block, pos), it->second.get(LightKind::sky, pos)};
    }

private:
    Oracle oracle_;
    circuit::contracts::WorldShape shape_;
    circuit::gameplay::WorldDump dump_, before_;
    mutable std::optional<circuit::test::DumpLightWorld> world_;
    mutable std::int32_t top_{};
    mutable std::map<circuit::contracts::ChunkPos, circuit::test::ReferenceLight> light_;
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

const char* items_text = R"(circuit-trace 2
title Items falling onto a hopper and onto stone
game-version 1.21.8
source circuit_reference
harness -
mechanisms item_entity hopper
area -2 0 -2 2 3 2
ticks 30
changes ordered
entities observed

at 0 set -1 0 0 minecraft:stone
at 0 set 1 0 0 minecraft:hopper[enabled=true,facing=down]
at 1 spawn-item 1.5 2.5 0.5 0 0 0 minecraft:redstone 3
at 1 spawn-item -0.5 2.5 0.5 0.125 0.25 0 minecraft:stone 1
slot 30 1 0 0 0 ?
)";

const char* header2 = "circuit-trace 2\ntitle Test\ngame-version 1.21.8\nsource circuit_reference\nharness -\n"
                      "mechanisms item_entity\narea -2 0 -2 2 1 2\nticks 4\n";
// A target-version trace has harness provenance and was recorded from the
// scenario currently in the repository.
void require_recorded_from_skeleton(const fs::path& fixtures, const fs::path& path, const Fixture& trace, std::string_view version) {
    require(trace.source == circuit::contracts::EvidenceSource::harness_trace && trace.harness.starts_with("circuit-harness:") &&
                trace.game_version == version && trace.harness.ends_with(std::string("+mc") + std::string(version)),
            std::format("{}: harness provenance", path.string()));
    fs::path origin;
    for (const auto* dir : {"skeletons", "documented", "open-skeletons"})
        if (fs::exists(fixtures / dir / path.filename())) {
            origin = fixtures / dir / path.filename();
            break;
        }
    require(!origin.empty(), std::format("{}: no skeleton", path.string()));
    const auto skeleton = parse(read_text(origin));
    auto actions = [](const Fixture& f) {
        std::vector<std::tuple<Tick, int, BlockPos, std::string, int, std::string, int, Vec3d, Vec3d>> out;
        for (const auto& a : f.actions)
            out.emplace_back(a.tick, static_cast<int>(a.kind), a.pos, a.state, a.slot, a.item, a.count, a.position, a.velocity);
        return out;
    };
    require(trace.format == skeleton.format && trace.observe_entities == skeleton.observe_entities
                && trace.observe_chunks == skeleton.observe_chunks && trace.observe_rng == skeleton.observe_rng
                && trace.dimension == skeleton.dimension,
            std::format("{}: recorded with other observations than its skeleton", path.string()));
    require(trace.title == skeleton.title && trace.area_min == skeleton.area_min && trace.area_max == skeleton.area_max
                && trace.ticks == skeleton.ticks && trace.changes == skeleton.changes
                && trace.random_tick_speed == skeleton.random_tick_speed
                && trace.neighbor_update_limit == skeleton.neighbor_update_limit && actions(trace) == actions(skeleton),
            std::format("{}: recorded from a different scenario; re-record it", path.string()));
}
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
        expect("circuit-trace 3\n", 1, "unsupported format");
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
        expect(h + "at 0 insert 0 0 0 0 minecraft:stone 100\n", 9, "invalid count");
        expect(std::string(header).replace(std::string(header).find("area"), 18, "area 0 0 0 200 0 0"), 7, "extent");
    });

    suite.run("version 2 format errors", [] {
        const auto expect = [](std::string text, std::size_t line, std::string_view fragment) {
            const auto f = parse_fixture(text);
            require(!f && f.error().line == line && f.error().message.find(fragment) != std::string::npos,
                    std::format("expected line {} `{}`, got {}", line, fragment,
                                f ? std::string("success") : std::format("{} {}", f.error().line, f.error().message)));
        };
        const std::string h = header2, e = std::string(header2) + "entities observed\n";
        expect(std::string(header) + "entities observed\n", 9, "needs circuit-trace 2");
        expect(std::string(header) + "at 0 spawn-item 0.5 0.5 0.5 0 0 0 minecraft:stone 1\n", 9, "needs circuit-trace 2");
        expect(h + "entity-added 1 commands e1 minecraft:item 0.5 0.5 0.5 0 0 0\n", 9, "require `entities observed`");
        expect(h + "chunk 1 commands 0 0 border\n", 9, "require `chunks observed`");
        expect(h + "draws 1 random_ticks level 2\n", 9, "require `rng observed`");
        expect(h + "tolerance 2\n", 9, "invalid tolerance");
        expect(h + "chunks observed\nchunk 1 commands 0 0 loaded\n", 10, "unknown chunk status");
        expect(e + "entity-added 1 commands e2 minecraft:item 0.5 0.5 0.5 0 0 0\n", 10, "the next entity is e1");
        expect(e + "entity-added 1 brewing e1 minecraft:item 0.5 0.5 0.5 0 0 0\n", 10, "unknown stage");
        expect(e + "entity-added 1 commands e1 minecraft:item 0.5 nan 0.5 0 0 0\n", 10, "invalid decimal");
        expect(e + "entity-removed 1 entities e1 discarded\n", 10, "was not added");
        expect(e + "entity-added 1 commands e1 minecraft:item 0.5 0.5 0.5 0 0 0\nentity-removed 1 entities e1 discarded\n"
                   "entity 1 e1 0.5 0.5 0.5 0 0 0\n", 12, "was removed");
        expect(e + "entity-added 2 commands e1 minecraft:item 0.5 0.5 0.5 0 0 0 count=1 count=2\n", 10, "duplicate property");
        expect(e + "entity-added 2 commands e1 minecraft:item 0.5 0.5 0.5 0 0 0\nentity-changed 1 entities e1 count=2\n", 11, "tick order");
        expect(h + "changes ordered\nchange 1 0 0 0 minecraft:air minecraft:stone\n", 10, "tick, stage");
    });

    suite.run("format limits", [] {
        std::string big = header;
        for (int i = 0; i < 20; ++i) big += "block 1 0 0 0 ?\n";
        require(parse_fixture(big, {.max_lines = 10}).error().message.find("too many lines") != std::string::npos, "lines");
        require(parse_fixture(big, {.max_bytes = 64}).error().line == 0, "bytes");
        require(parse_fixture(std::string(header) + "# " + std::string(5000, 'c') + "\n").error().message == "line too long", "length");
    });

    suite.run("scenarios that load their own chunks", [&] {
        const std::string head = "circuit-trace 2\ntitle Loading\ngame-version 1.21.8\nsource circuit_reference\nharness -\n"
                                 "mechanisms chunk_tickets\narea 0 0 0 15 3 15\nticks 5\nchunks observed\nload none\nobserve -2 -2 2 2\n";
        const auto f = parse(head + "at 1 command forceload add 0 0\nat 3 command forceload remove all\n");
        require(!f.load_area && f.observe_window == std::array<std::int32_t, 4>{-2, -2, 2, 2} && f.actions.size() == 2
                    && f.actions[0].kind == Action::Kind::command && f.actions[0].command == "forceload add 0 0",
                "headers and commands");
        require(write_fixture(parse(write_fixture(f))) == write_fixture(f), "canonical text");
        for (const auto* bad : {"load some\n", "observe 2 0 1 0\n", "observe 0 0 64 0\n"}) {
            const auto r = circuit::evidence::parse_fixture("circuit-trace 2\ntitle Loading\ngame-version 1.21.8\nsource circuit_reference\n"
                                                            "harness -\nmechanisms chunk_tickets\narea 0 0 0 15 3 15\nticks 5\n" + std::string(bad));
            require(!r, std::format("refused: {}", bad));
        }
        require(!circuit::evidence::parse_fixture("circuit-trace 1\ntitle Loading\ngame-version 1.21.8\nsource circuit_reference\n"
                                                  "harness -\nmechanisms chunk_tickets\narea 0 0 0 15 3 15\nticks 5\n"
                                                  "at 1 command forceload add 0 0\n"),
                "commands need version 2");
        // Circuit records the chunks it loads: the forced chunk and its two
        // rings, all within the observed window.
        auto recorded = record_fixture(f, engine_executor(1));
        require(recorded.has_value(), recorded ? std::string() : recorded.error());
        std::map<std::pair<std::int32_t, std::int32_t>, std::string> status;
        for (const auto& c : recorded->chunk_events)
            if (c.tick == 1) status[{c.x, c.z}] = c.status;
        require(status.size() == 25 && status.at({0, 0}) == "entity_ticking" && status.at({1, 1}) == "block_ticking"
                    && status.at({2, -2}) == "border",
                std::format("{} chunks at tick 1", status.size()));
        require(evaluate_fixture(*recorded, engine_executor(2)).outcome == Outcome::passed, "its own recording passes");
        // Other commands are not modeled.
        const auto other = parse(head + "at 1 command gamerule spawnChunkRadius 3\n");
        require(evaluate_fixture(other, engine_executor(1)).outcome == Outcome::unsupported, "other commands");
        const auto big = parse(head + "at 1 command forceload add 0 0 300 300\n");
        require(evaluate_fixture(big, engine_executor(1)).outcome == Outcome::unsupported, "more than 256 chunks");
    });

    suite.run("heightmap properties match both official servers", [&] {
        // tests/fixtures/heightmap: which states count for MOTION_BLOCKING, by
        // block when its states agree, as the official servers report them.
        namespace registry = circuit::gameplay::registry;
        std::size_t states = 0;
        for (const auto& [file, release] : {std::pair{"motion-blocking-1.21.8.tsv", "1.21.8"}, std::pair{"motion-blocking-1.21.4-only.tsv", "1.21.4"}}) {
            const auto* mapping = ContentMapping::for_game_version(release);
            const auto check = [&](std::uint32_t internal, bool expected) {
                const auto notation = registry::notation(internal);
                const auto state = mapping->block(notation);
                require(state.has_value(), notation);
                require(circuit::gameplay::motion_blocking(*state) == expected, std::format("{} {}: {}", release, notation, !expected));
                const bool leaves = notation.find("_leaves") != std::string::npos;
                require(circuit::gameplay::motion_blocking_no_leaves(*state) == (expected && !leaves), std::format("{} {}: leaves", release, notation));
                ++states;
            };
            std::istringstream lines(read_text(fixtures / "heightmap" / file));
            for (std::string line; std::getline(lines, line);) {
                if (line.empty() || line.front() == '#') continue;
                const auto tab = line.find('\t');
                require(tab != std::string::npos, line);
                const auto name = line.substr(0, tab), value = line.substr(tab + 1);
                if (name.find('[') != std::string::npos) {
                    const auto state = mapping->block(name);
                    require(state.has_value(), name);
                    require(circuit::gameplay::motion_blocking(*state) == (value == "1"), std::format("{} {}", release, name));
                    ++states;
                    continue;
                }
                const auto block = registry::find_block(release, name);
                require(block.has_value(), name);
                const auto info = registry::block(*block);
                for (auto s = info.first_state; s < info.first_state + info.state_count; ++s) {
                    bool waterlogged = false;
                    for (const auto& [key, v] : registry::properties(s)) waterlogged |= key == "waterlogged" && v == "true";
                    check(s, value == "waterlogged" ? waterlogged : value == "1");
                }
            }
        }
        require(states == 27946 + 12, std::format("{} states", states));
    });

    suite.run("collision boxes match both official servers", [&] {
        // tests/fixtures/collision: every state's collision boxes as the
        // official servers report them, in 1/32 block; Circuit's through its
        // own state representation (modeled states included).
        namespace registry = circuit::gameplay::registry;
        using Boxes = std::vector<std::array<int, 6>>;
        const auto parse_boxes = [](std::string_view text) {
            Boxes out;
            if (text == "-") return out;
            for (std::size_t i = 0; i < text.size();) {
                const auto end = std::min(text.find(';', i), text.size());
                std::array<int, 6> b{};
                std::size_t at = i;
                for (auto& v : b) {
                    const auto stop = std::min(text.find_first_of(",;", at), end);
                    std::from_chars(text.data() + at, text.data() + stop, v);
                    at = stop + 1;
                }
                out.push_back(b);
                i = end + 1;
            }
            std::ranges::sort(out);
            return out;
        };
        const auto circuit_boxes = [](circuit::gameplay::BlockState s, std::string_view release) {
            Boxes out;
            for (const auto& b : circuit::gameplay::physics::collision_boxes(s, release)) {
                std::array<int, 6> v{};
                const std::array<double, 6> d{b.min_x, b.min_y, b.min_z, b.max_x, b.max_y, b.max_z};
                for (std::size_t i = 0; i < 6; ++i) {
                    v[i] = static_cast<int>(std::lround(d[i] * 32.0));
                    require(static_cast<double>(v[i]) == d[i] * 32.0, "boxes in 1/32 block");
                }
                out.push_back(v);
            }
            std::ranges::sort(out);
            return out;
        };
        // The official boxes of a release's states, by Circuit state.
        const auto official = [&](std::string_view file, std::string_view release, std::map<std::uint32_t, Boxes>& out) {
            const auto* mapping = ContentMapping::for_game_version(release);
            std::istringstream lines(read_text(fixtures / "collision" / std::string(file)));
            for (std::string line; std::getline(lines, line);) {
                if (line.empty() || line.front() == '#') continue;
                const auto tab = line.find('\t');
                require(tab != std::string::npos, line);
                const auto name = line.substr(0, tab);
                const auto boxes = parse_boxes(std::string_view(line).substr(tab + 1));
                if (name.find('[') != std::string::npos) {
                    const auto state = mapping->block(name);
                    require(state.has_value(), name);
                    out[state->raw()] = boxes;
                    continue;
                }
                const auto block = registry::find_block(release, name);
                require(block.has_value(), name);
                const auto info = registry::block(*block);
                for (auto s = info.first_state; s < info.first_state + info.state_count; ++s) {
                    const auto state = mapping->block(registry::notation(s));
                    require(state.has_value(), registry::notation(s));
                    out[state->raw()] = boxes;
                }
            }
        };
        std::map<std::uint32_t, Boxes> latest, older;
        official("collision-1.21.8.tsv", "1.21.8", latest);
        older = latest;
        official("collision-1.21.4-only.tsv", "1.21.4", older);
        std::size_t states = 0;
        for (const auto& [release, expected] : {std::pair{"1.21.8", &latest}, std::pair{"1.21.4", &older}}) {
            const auto* mapping = ContentMapping::for_game_version(release);
            for (std::uint32_t wire = 0; wire < registry::wire_state_count(release); ++wire) {
                const auto internal = registry::from_wire(release, static_cast<std::int32_t>(wire));
                require(internal.has_value(), std::format("{} wire state {}", release, wire));
                const auto notation = registry::notation(*internal);
                const auto state = mapping->block(notation);
                require(state.has_value(), notation);
                const auto it = expected->find(state->raw());
                require(it != expected->end(), std::format("{} {}: no official boxes", release, notation));
                require(circuit_boxes(*state, release) == it->second, std::format("{} {}", release, notation));
                ++states;
            }
        }
        require(states == 27946 + 27866, std::format("{} states", states));
    });

    suite.run("light properties match both official servers", [&] {
        // tests/fixtures/light: every state's emission, dampening and
        // occlusion shape as the official servers report them.
        namespace registry = circuit::gameplay::registry;
        std::size_t states = 0;
        for (const auto& [file, release] : {std::pair{"states-1.21.8.tsv", "1.21.8"}, std::pair{"states-1.21.4-only.tsv", "1.21.4"}}) {
            const auto* mapping = ContentMapping::for_game_version(release);
            std::istringstream lines(read_text(fixtures / "light" / file));
            for (std::string line; std::getline(lines, line);) {
                if (line.empty() || line.front() == '#') continue;
                std::vector<std::string> f;
                for (std::size_t i = 0; i <= line.size();) {
                    const auto tab = std::min(line.find('\t', i), line.size());
                    f.push_back(line.substr(i, tab - i));
                    i = tab + 1;
                }
                require(f.size() == 4, line);
                const auto state = mapping->block(f[0]);
                require(state.has_value(), std::format("{}: {}", f[0], state ? "" : state.error()));
                const auto info = circuit::gameplay::light_info(*state);
                require(info.emission == std::stoi(f[1]) && info.dampening == std::stoi(f[2]),
                        std::format("{} {}: emission {} dampening {}", release, f[0], info.emission, info.dampening));
                if (f[3] == "-") {
                    require(info.shape == 0, std::format("{} {}: occludes by shape", release, f[0]));
                } else {
                    // The faces the reported boxes touch, against the registry's.
                    std::vector<std::array<int, 6>> boxes;
                    for (std::size_t i = 0; i <= f[3].size();) {
                        const auto semi = std::min(f[3].find(';', i), f[3].size());
                        std::array<int, 6> b{};
                        std::istringstream in(f[3].substr(i, semi - i));
                        for (auto& v : b) {
                            in >> v;
                            in.ignore(1);
                        }
                        boxes.push_back(b);
                        i = semi + 1;
                    }
                    constexpr std::array<std::pair<int, int>, 6> faces{{{1, 0}, {1, 1}, {2, 0}, {2, 1}, {0, 0}, {0, 1}}};
                    for (std::size_t d = 0; d < 6; ++d) {
                        const auto [axis, side] = faces[d];
                        const int u = axis == 0 ? 1 : 0, v = axis == 2 ? 1 : 2;
                        std::array<std::uint64_t, 4> want{};
                        for (const auto& b : boxes)
                            if ((side == 0 && b[static_cast<std::size_t>(axis)] == 0) || (side == 1 && b[static_cast<std::size_t>(axis) + 3] == 16))
                                for (int a = b[static_cast<std::size_t>(u)]; a < b[static_cast<std::size_t>(u) + 3]; ++a)
                                    for (int c = b[static_cast<std::size_t>(v)]; c < b[static_cast<std::size_t>(v) + 3]; ++c)
                                        want[static_cast<std::size_t>((a * 16 + c) / 64)] |= std::uint64_t{1} << ((a * 16 + c) % 64);
                        require(info.shape != 0 && registry::light_face(info.shape, static_cast<circuit::contracts::Direction>(d)) == want,
                                std::format("{} {}: face {}", release, f[0], d));
                    }
                }
                ++states;
            }
        }
        require(states == 27958, std::format("{} states", states));
    });

    suite.run("canonical text round trip", [&] {
        const auto f = parse(read_text(fixtures / "documented" / "lamp_timing.ctrace"));
        const auto text = write_fixture(f);
        require(write_fixture(parse(text)) == text, "write(parse(write(f))) is stable");
        require(f.source == circuit::contracts::EvidenceSource::documented_behavior && !f.is_skeleton(), "fields");
    });

    suite.run("content mapping round trips every target-version state", [] {
        namespace registry = circuit::gameplay::registry;
        // Every wire state of each version has exactly one Circuit state, whose
        // notation names every property.
        for (const auto* release : {"1.21.8", "1.21.4"}) {
            const auto* m = ContentMapping::for_game_version(release);
            require(m != nullptr, "mapping availability");
            const auto wires = registry::wire_state_count(release);
            require(wires > 27000, "every wire state");
            std::set<std::uint32_t> raws;
            for (std::uint32_t wire = 0; wire < wires; ++wire) {
                const auto internal = registry::from_wire(release, static_cast<std::int32_t>(wire));
                require(internal && registry::to_wire(release, *internal) == static_cast<std::int32_t>(wire), "wire round trip");
                const auto notation = registry::notation(*internal);
                const auto s = m->block(notation);
                require(s.has_value(), std::format("{} {}: {}", release, notation, s ? "" : s.error()));
                require(m->notation(*s) == notation, std::format("{}: {} -> {}", release, notation, m->notation(*s)));
                require(raws.insert(s->raw()).second, std::format("{}: {} shares its state", release, notation));
            }
        }
        const auto* m = ContentMapping::for_game_version("1.21.8");
        require(!ContentMapping::for_game_version("1.20.4"), "no mapping for other versions");
        // Every valid Circuit state of a 1.21.8 block is one of its wire states.
        unsigned states = 0;
        circuit::gameplay::for_each_block_state([&](circuit::gameplay::BlockState s) {
            if (s.is(circuit::gameplay::BlockType::other) && !registry::to_wire("1.21.8", s.registry_state())) return;
            ++states;
            const auto back = m->block(m->notation(s));
            require(back && *back == s, std::format("round trip {} via {}", s.raw(), m->notation(s)));
        });
        require(states == registry::wire_state_count("1.21.8"), std::format("{} states", states));
        for (std::uint16_t i = 1; i <= circuit::gameplay::items::max_id; ++i)
            require(m->item(m->item_name(i)) == i, "item round trip");
    });

    suite.run("unrepresentable content is reported", [] {
        const auto* m = ContentMapping::for_game_version("1.21.8");
        const auto unsupported = [&](std::string_view s) {
            const auto r = m->block(s);
            require(!r && r.error().find("unsupported") != std::string::npos, std::format("{} -> {}", s, r ? "ok" : r.error()));
        };
        require(m->block("minecraft:repeater[delay=1,facing=west,locked=true,powered=false]").has_value(), "locked repeaters are modeled");
        require(m->block("minecraft:chest[facing=north,type=single,waterlogged=true]")
                    == circuit::gameplay::blocks::chest(circuit::contracts::Direction::north).with_flag(true),
                "waterlogged chests are represented");
        require(m->block("minecraft:trapped_chest[facing=north,type=single,waterlogged=false]")->is(circuit::gameplay::BlockType::other),
                "blocks without rules of their own are registry states");
        require(m->block("minecraft:chest[facing=north,type=left,waterlogged=false]")
                    == circuit::gameplay::blocks::chest(circuit::contracts::Direction::north, circuit::gameplay::blocks::ChestType::left),
                "double chest halves are modeled");
        require(m->block("minecraft:comparator[facing=north,mode=subtract,powered=true]")
                    == circuit::gameplay::blocks::comparator(circuit::contracts::Direction::south, true, true),
                "comparator facing names the input side (unverified convention)");
        unsupported("examplemod:gadget");
        const auto* old = ContentMapping::for_game_version("1.21.4");
        require(!old->block("minecraft:dried_ghast[facing=north,hydration=0,waterlogged=false]").has_value(), "1.21.8 blocks in 1.21.4");
        require(m->block("minecraft:creaking_heart[axis=y,creaking_heart_state=awake,natural=true]").has_value() &&
                    old->block("minecraft:creaking_heart[active=true,axis=y,natural=true]").has_value() &&
                    !m->block("minecraft:creaking_heart[active=true,axis=y,natural=true]").has_value(),
                "each version has its own creaking heart schema");
        require(!m->block("minecraft:stone[snowy=true]").has_value(), "unknown property");
        require(!m->block("minecraft:dirt[snowy=true]").has_value(), "unknown property of a registry block");
        require(!m->block("minecraft:repeater[delay=1,facing=west]").has_value(), "missing modeled property");
        require(!m->block("minecraft:repeater[delay=5,facing=west,powered=false]").has_value(), "range");
        using circuit::gameplay::WireSide;
        require(m->block("minecraft:redstone_wire[east=side,north=none,power=7,south=up,west=none]")
                    == circuit::gameplay::blocks::wire(7, WireSide::none, WireSide::up, WireSide::none, WireSide::side),
                "dust shape is modeled");
        require(m->block("minecraft:repeater[delay=2,facing=west,locked=false,powered=true]")
                    == circuit::gameplay::blocks::repeater(circuit::contracts::Direction::east, 2, true),
                "repeater facing names the input side (unverified convention)");
        require(m->block("minecraft:water[level=12]") == circuit::gameplay::blocks::water_falling().with_level(4), "falling water");
        require(m->block("minecraft:moving_piston[facing=east,type=sticky]")
                    == circuit::gameplay::blocks::moving_piston(circuit::contracts::Direction::east, true), "moving piston");
        require(m->item("minecraft:diamond").has_value(), "every item of the version is in the registry");
        require(!m->item("examplemod:gizmo").has_value(), "unknown item");
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
                    && recorded.harness == "circuit-reference-rules-v10", "provenance");
        require(!recorded.recorded_changes.empty(), "changes recorded");
        for (const auto& exec : {engine_executor(1), engine_executor(8), oracle_executor()}) {
            const auto r = evaluate_fixture(recorded, exec);
            require(r.outcome == Outcome::passed && r.changes_compared > 0 && r.probes_compared == 4, r.detail);
        }
    });

    suite.run("version 2 records entity lifecycles and states", [] {
        const auto skeleton = parse(items_text);
        const auto by_engine = record_fixture(skeleton, engine_executor(4));
        const auto by_oracle = record_fixture(skeleton, oracle_executor());
        require(by_engine && by_oracle, by_engine ? "" : by_engine.error());
        const auto text = write_fixture(*by_engine);
        require(text == write_fixture(*by_oracle), "engine and oracle record identical fixtures");
        const auto recorded = parse(text);
        require(write_fixture(recorded) == text, "the recording round-trips");
        // Both items are added in the command stage of tick 1 at their exact positions.
        require(recorded.entity_events.size() >= 3, "events recorded");
        const auto& first = recorded.entity_events[0];
        require(first.kind == EntityEvent::Kind::added && first.handle == "e1" && first.stage == "commands" && first.tick == 1
                    && first.position == Vec3d{1.5, 2.5, 0.5} && first.type == "minecraft:item",
                "e1 added at its spawn position");
        require(recorded.entity_events[1].velocity == Vec3d{0.125, 0.25, 0.0}, "e2 keeps its velocity");
        // The hopper picks up the redstone in the block-entity stage and holds it.
        const auto removal = std::ranges::find_if(recorded.entity_events, [](const EntityEvent& e) { return e.kind == EntityEvent::Kind::removed; });
        require(removal != recorded.entity_events.end() && removal->handle == "e1" && removal->stage == "block_entities"
                    && removal->reason == "discarded", "the hopper takes e1");
        require(recorded.slot_probes[0].item.value == "minecraft:redstone" && recorded.slot_probes[0].count == 3, "into its slot");
        require(!recorded.entity_probes.empty() && !recorded.entity_orders.empty(), "probes and orders recorded");
        for (const auto& exec : {engine_executor(1), engine_executor(8), oracle_executor()}) {
            const auto r = evaluate_fixture(recorded, exec);
            require(r.outcome == Outcome::passed && r.events_compared > 0 && r.probes_compared > 0, r.detail);
        }
        // A position 1/8192 block off fails exactly and passes within a declared tolerance.
        auto moved = recorded;
        moved.entity_probes[0].position[1] += 1.0 / 8192.0;
        auto r = evaluate_fixture(moved, engine_executor());
        require(r.outcome == Outcome::failed && r.detail.find("entity probe") != std::string::npos, r.detail);
        moved.tolerance = 0.001;
        require(evaluate_fixture(moved, engine_executor()).outcome == Outcome::passed, "within the tolerance");
        // Removal reasons, stages and tick order are compared.
        auto reason = recorded;
        std::ranges::find_if(reason.entity_events, [](const EntityEvent& e) { return e.kind == EntityEvent::Kind::removed; })->reason = "killed";
        r = evaluate_fixture(reason, engine_executor());
        require(r.outcome == Outcome::failed && r.detail.find("removed (killed)") != std::string::npos, r.detail);
        auto stage = recorded;
        stage.entity_events[0].stage = "entities";
        require(evaluate_fixture(stage, engine_executor()).outcome == Outcome::failed, "stages are compared");
        auto order = recorded;
        const auto two = std::ranges::find_if(order.entity_orders, [](const EntityOrder& o) { return o.handles.size() == 2; });
        require(two != order.entity_orders.end(), "both items tick");
        std::ranges::reverse(two->handles);
        r = evaluate_fixture(order, engine_executor());
        require(r.outcome == Outcome::failed && r.detail.find("entity order") != std::string::npos, r.detail);
        // Draws from the level random are counted per stage: these items draw nothing.
        auto rng = recorded;
        rng.observe_rng = true;
        rng.rng.push_back({1, "random_ticks", "level", 3, 0});
        r = evaluate_fixture(rng, engine_executor());
        require(r.outcome == Outcome::failed && r.detail.find("expected 3 draws from `level` in random_ticks, observed 0") != std::string::npos,
                r.detail);
        // Any position is exact: doubles as the target versions keep them.
        auto odd = skeleton;
        odd.actions[2].position[0] = 1.1;
        const auto odd_recorded = record_fixture(odd, engine_executor());
        require(odd_recorded && evaluate_fixture(*odd_recorded, engine_executor()).outcome == Outcome::passed
                    && odd_recorded->entity_events[0].position[0] == 1.1,
                "positions are exact");
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
        auto r = evaluate_fixture(parse(h + "changes ordered\nchange 1 0 0 0 minecraft:air examplemod:gadget\n"),
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
      // Traces of each supported game version, recorded by the external harness.
      for (const auto& [directory, version] : {std::pair{"harness", "1.21.8"}, std::pair{"harness-1.21.4", "1.21.4"}}) {
        std::size_t traces = 0;
        for (const auto& entry : fs::directory_iterator(fixtures / directory)) {
            const auto trace = parse(read_text(entry.path()));
            require_recorded_from_skeleton(fixtures, entry.path(), trace, version);
            for (const auto& exec : {engine_executor(4), oracle_executor()}) {
                const auto r = evaluate_fixture(trace, exec);
                require(r.outcome == Outcome::passed, std::format("{}: {}", entry.path().string(), r.detail));
            }
            ++traces;
        }
        require(traces >= 20, std::format("{} target-version traces for {}", traces, version));
      }
    });

    suite.run("open target-version traces keep their first mismatch", [&] {
        // Traces Circuit does not match yet stay visible, apart from the passing
        // suite, with the outcome and first mismatch recorded in EXPECTED.txt.
        for (const auto& [directory, version] : {std::pair{"open", "1.21.8"}, std::pair{"open-1.21.4", "1.21.4"}}) {
            std::map<std::string, std::string> expected;
            std::istringstream lines(read_text(fixtures / directory / "EXPECTED.txt"));
            for (std::string line; std::getline(lines, line);) {
                if (line.empty() || line.front() == '#') continue;
                const auto space = line.find(' ');
                expected[line.substr(0, space)] = line.substr(space + 1);
            }
            std::size_t traces = 0;
            for (const auto& entry : fs::directory_iterator(fixtures / directory)) {
                if (entry.path().extension() != ".ctrace") continue;
                const auto trace = parse(read_text(entry.path()));
                require_recorded_from_skeleton(fixtures, entry.path(), trace, version);
                const auto name = entry.path().filename().string();
                require(expected.contains(name), std::format("{}: not listed in EXPECTED.txt", entry.path().string()));
                for (const auto& exec : {engine_executor(4), oracle_executor()}) {
                    const auto r = evaluate_fixture(trace, exec);
                    require(r.outcome != Outcome::passed,
                            std::format("{} passes now: move it to the harness directory and update the ledger", entry.path().string()));
                    require(r.outcome == Outcome::failed || r.outcome == Outcome::unsupported,
                            std::format("{}: {}", entry.path().string(), r.detail));
                    const auto observed = std::format("{} {}", outcome_name(r.outcome), r.detail.substr(0, r.detail.find(':')));
                    require(observed == expected[name], std::format("{}: the first mismatch moved from `{}` to `{}`: {}", entry.path().string(),
                                                                    expected[name], observed, r.detail));
                }
                expected.erase(name);
                ++traces;
            }
            require(expected.empty(), std::format("{}: EXPECTED.txt lists missing traces", directory));
            (void)traces; // traces leave as they pass; EXPECTED.txt lists each one left
            // Every open skeleton has a trace on each version, or the harness refused it there.
            std::set<std::string> refused;
            if (fs::exists(fixtures / directory / "NOT_APPLICABLE.txt")) {
                std::istringstream na(read_text(fixtures / directory / "NOT_APPLICABLE.txt"));
                for (std::string line; std::getline(na, line);)
                    if (!line.empty() && line.front() != '#') refused.insert(line.substr(0, line.find(' ')));
            }
            for (const auto& entry : fs::directory_iterator(fixtures / "open-skeletons")) {
                const auto name = entry.path().filename().string();
                const bool traced = fs::exists(fixtures / directory / name);
                require(traced != refused.contains(name),
                        std::format("{}: {} needs a trace or a NOT_APPLICABLE entry, not both", directory, name));
            }
        }
    });

    suite.run("recordings of the same scenario are compared observation by observation", [&] {
        const auto official = parse(read_text(fixtures / "open" / "item_drop_piston.ctrace"));
        const auto circuit = parse(read_text(fixtures / "reference" / "item_drop_piston.ctrace"));
        require(!first_difference(official, official).has_value(), "a recording equals itself");
        require(!first_difference(official, parse(read_text(fixtures / "open-1.21.4" / "item_drop_piston.ctrace"))).has_value(),
                "provenance is not compared: both versions recorded the same");
        const auto d = first_difference(official, circuit);
        // The drop matches; the moving block pushes it a tick early (C4.1).
        require(d && d->starts_with("tick 1 observation 2 (line 28 / line 28)") && d->find("entity e1") != std::string::npos, *d);
        // Property order in block states does not matter; values do.
        auto dust = parse(read_text(fixtures / "reference" / "item_drop_piston.ctrace"));
        for (auto& c : dust.recorded_changes)
            if (c.before.starts_with("minecraft:redstone_wire")) c.before = "minecraft:redstone_wire[west=none,south=none,power=0,north=none,east=none]";
        require(!first_difference(circuit, dust).has_value(), "properties in another order");
        // Doubles within a tolerance, and different scenarios.
        auto moved = circuit;
        moved.entity_probes.front().position[0] += 1e-9;
        require(first_difference(circuit, moved).has_value() && !first_difference(circuit, moved, 1e-6).has_value(), "tolerance");
        auto other = circuit;
        other.actions.back().tick += 1;
        require(first_difference(circuit, other) == std::optional<std::string>("different scenarios: actions"), "scenario");
        // Game rules are part of the scenario; Circuit models their defaults only.
        const auto with_rule = parse(read_text(fixtures / "open" / "tnt_explodes_disabled.ctrace"));
        require(with_rule.gamerules == std::vector<std::pair<std::string, std::string>>{{"tntExplodes", "false"}}, "game rule read");
        require(write_fixture(parse(write_fixture(with_rule))) == write_fixture(with_rule), "game rules round-trip");
        require(first_difference(with_rule, parse(read_text(fixtures / "open" / "tnt_redstone_ignition.ctrace")))
                    == std::optional<std::string>("different scenarios: observation headers"), "a game rule changes the scenario");
        require(evaluate_fixture(with_rule, engine_executor()).outcome == Outcome::unsupported, "unsupported");
        require(!parse_fixture(std::string(header) + "gamerule tntExplodes false\n"), "game rules need version 2");
        require(!parse_fixture(std::string(header2) + "gamerule a true\ngamerule a false\n"), "duplicate game rule");
        // A rule at its default changes nothing; randomTickSpeed is world
        // state at any value; another rule away from its default is not
        // modeled; a rule the version lacks makes the scenario invalid there.
        auto text = read_text(fixtures / "reference" / "item_drop_piston.ctrace");
        text.insert(text.find("entities observed"), "gamerule tntExplodes true\ngamerule randomTickSpeed 3\n");
        require(evaluate_fixture(parse(text), engine_executor()).outcome == Outcome::passed, "rules at their defaults");
        auto faster = text;
        faster.replace(faster.find("randomTickSpeed 3"), 17, "randomTickSpeed 5");
        require(evaluate_fixture(parse(faster), engine_executor()).outcome == Outcome::passed, "randomTickSpeed at another value");
        auto changed = text;
        changed.replace(changed.find("tntExplodes true"), 16, "tntExplodes false");
        const auto r = evaluate_fixture(parse(changed), engine_executor());
        require(r.outcome == Outcome::unsupported && r.detail.find("tntExplodes") != std::string::npos, r.detail);
        auto old = text;
        old.replace(old.find("game-version 1.21.8"), 19, "game-version 1.21.4");
        const auto o = evaluate_fixture(parse(old), engine_executor());
        require(o.outcome == Outcome::invalid && o.detail.find("tntExplodes does not exist in 1.21.4") != std::string::npos, o.detail);
    });

    suite.run("the 1.21.4 rule set rests on identical recordings", [&] {
        // Both bundles run the same rules because every scenario recorded on
        // 1.21.4 is identical to its 1.21.8 recording.
        const auto& b4 = **circuit::contracts::BundleRegistry::select_for_simulation("java-1.21.4");
        const auto& b8 = **circuit::contracts::BundleRegistry::select_for_simulation("java-1.21.8");
        require(b4.rules->id == b8.rules->id && b4.rules->basis.find("identical") != std::string_view::npos, "the basis is stated");
        std::size_t pairs = 0;
        for (const auto& entry : fs::directory_iterator(fixtures / "harness-1.21.4")) {
            const auto counterpart = fixtures / "harness" / entry.path().filename();
            require(fs::exists(counterpart), std::format("{} has a 1.21.8 recording", entry.path().filename().string()));
            const auto diff = first_difference(parse(read_text(entry.path())), parse(read_text(counterpart)));
            require(!diff, std::format("{}: {}", entry.path().filename().string(), diff.value_or("")));
            ++pairs;
        }
        require(pairs >= 30, std::format("{} pairs", pairs));
    });

    return suite.finish();
}
