// SPDX-License-Identifier: AGPL-3.0-only
// Anvil import and export: round trips of every scenario's state, the
// unsupported-content policies, malformed input, and the world tools.
//
// Set CIRCUIT_ANVIL_EXPORT_DIR to keep the exported scenario and random
// sequence worlds, for loading them in the game.
#include "../sim/scenarios.hpp"
#include "../support/check.hpp"
#include <circuit/gameplay/rng.hpp>
#include <circuit/protocol/framing.hpp>
#include <circuit/protocol/nbt.hpp>
#include <circuit/runtime/engine.hpp>
#include <circuit/server/anvil.hpp>
#include <circuit/server/server.hpp>
#include <circuit/storage/world_directory.hpp>
#include <cstdlib>
#include <cstring>
#include <set>
#include <filesystem>
#include <optional>
#include <numeric>
#include <functional>
#include <format>
#include <circuit/gameplay/block_registry.hpp>
#include <fstream>
#include <unistd.h>
#include <zlib.h>

using namespace circuit::test;
namespace anvil = circuit::server::anvil;
namespace nbt = circuit::protocol::nbt;
namespace fs = std::filesystem;
using circuit::runtime::Engine;
using nbt::Tag;
using nbt::TagType;

namespace {
const VersionBundle& bundle() { return **BundleRegistry::select_for_simulation("java-1.21.8"); }

fs::path temporary_directory() {
    std::string templ = (fs::temp_directory_path() / "circuit-anvil-XXXXXX").string();
    require(::mkdtemp(templ.data()) != nullptr, "temporary directory");
    return templ;
}

// ---- an independent minimal Anvil writer for hand-made worlds ---------------

std::vector<std::byte> named(const Tag& root) {
    circuit::protocol::Writer w;
    nbt::write_named(w, "", root);
    return w.take();
}
std::vector<std::byte> gzip_bytes(std::span<const std::byte> in) {
    z_stream z{};
    deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
    std::vector<std::byte> out(deflateBound(&z, static_cast<uLong>(in.size())) + 32);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    deflate(&z, Z_FINISH);
    out.resize(out.size() - z.avail_out);
    deflateEnd(&z);
    return out;
}
void write(const fs::path& p, std::span<const std::byte> data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}
std::vector<std::byte> gunzip_bytes(std::span<const std::byte> in) {
    z_stream z{};
    inflateInit2(&z, 31);
    std::vector<std::byte> out(1 << 20);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    require(inflate(&z, Z_FINISH) == Z_STREAM_END, "gzip stream");
    out.resize(out.size() - z.avail_out);
    inflateEnd(&z);
    return out;
}
std::vector<std::byte> read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<char> c((std::istreambuf_iterator<char>(in)), {});
    std::vector<std::byte> out(c.size());
    std::memcpy(out.data(), c.data(), c.size());
    return out;
}
// One region file holding `chunks`, each with compression type `type`.
std::vector<std::byte> region(const std::vector<std::pair<ChunkPos, Tag>>& chunks, std::uint8_t type = 2) {
    std::vector<std::byte> out(8192);
    for (const auto& [pos, root] : chunks) {
        const auto raw = named(root);
        const auto body = type == 2 ? circuit::protocol::deflate_bytes(raw) : type == 1 ? gzip_bytes(raw) : raw;
        const auto start = out.size();
        const auto length = static_cast<std::uint32_t>(body.size() + 1);
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::byte>((length >> (8 * i)) & 0xFF));
        out.push_back(static_cast<std::byte>(type));
        out.insert(out.end(), body.begin(), body.end());
        out.resize((out.size() + 4095) / 4096 * 4096);
        const auto k = static_cast<std::size_t>((pos.x & 31) + (pos.z & 31) * 32);
        const auto loc = static_cast<std::uint32_t>(((start / 4096) << 8) | ((out.size() - start) / 4096));
        for (int i = 0; i < 4; ++i) out[4 * k + static_cast<std::size_t>(i)] = static_cast<std::byte>((loc >> (24 - 8 * i)) & 0xFF);
    }
    return out;
}
Tag level_dat(std::int32_t data_version = anvil::supported_data_version, std::int64_t time = 100) {
    auto data = Tag::compound();
    data.set("DataVersion", Tag::int_(data_version));
    data.set("LevelName", Tag::string("hand made"));
    data.set("Time", Tag::long_(time));
    data.set("SpawnX", Tag::int_(1));
    data.set("SpawnY", Tag::int_(-60));
    data.set("SpawnZ", Tag::int_(2));
    auto gen = Tag::compound();
    gen.set("seed", Tag::long_(42));
    data.set("WorldGenSettings", std::move(gen));
    auto root = Tag::compound();
    root.set("Data", std::move(data));
    return root;
}
Tag block(std::string name, std::vector<std::pair<std::string, std::string>> props = {}) {
    auto t = Tag::compound();
    t.set("Name", Tag::string(std::move(name)));
    if (!props.empty()) {
        auto p = Tag::compound();
        for (auto& [k, v] : props) p.set(k, Tag::string(v));
        t.set("Properties", std::move(p));
    }
    return t;
}
// A full chunk whose section 0 (y 0-15) holds `palette`, cell i taking
// palette[i % palette.size()] in the first `cells` cells and palette[0] after.
Tag chunk(ChunkPos pos, std::vector<Tag> palette, std::size_t cells = 4) {
    auto root = Tag::compound();
    root.set("DataVersion", Tag::int_(anvil::supported_data_version));
    root.set("xPos", Tag::int_(pos.x));
    root.set("zPos", Tag::int_(pos.z));
    root.set("yPos", Tag::int_(-4));
    root.set("Status", Tag::string("minecraft:full"));
    auto sections = Tag::list(TagType::compound);
    auto s = Tag::compound();
    s.set("Y", Tag::byte(0));
    auto states = Tag::compound();
    const auto n = palette.size();
    auto list = Tag::list(TagType::compound);
    for (auto& p : palette) list.push(std::move(p));
    states.set("palette", std::move(list));
    if (n > 1) {
        const unsigned bits = std::max(4u, static_cast<unsigned>(std::bit_width(n - 1)));
        const unsigned per = 64 / bits;
        std::vector<std::int64_t> data((4096 + per - 1) / per);
        for (std::size_t i = 0; i < cells; ++i)
            data[i / per] |= static_cast<std::int64_t>(static_cast<std::uint64_t>(i % n) << ((i % per) * bits));
        states.set("data", Tag::long_array(std::move(data)));
    }
    s.set("block_states", std::move(states));
    sections.push(std::move(s));
    root.set("sections", std::move(sections));
    return root;
}
fs::path hand_made_world(const fs::path& dir, std::vector<std::pair<ChunkPos, Tag>> chunks, Tag level = level_dat()) {
    write(dir / "level.dat", gzip_bytes(named(level)));
    write(dir / "region" / "r.0.0.mca", region(chunks));
    return dir;
}

// ---- comparisons ---------------------------------------------------------------

// What an export followed by an import keeps of a state: the import makes every
// column unloaded and renumbers creation orders, entity ids and tick suborders.
// A hopper's last tick only matters within a tick and is not saved.
struct Kept {
    Tick time{};
    std::int64_t day_time{};
    std::uint8_t difficulty{};
    std::map<std::string, std::int32_t> game_rules;
    std::tuple<std::int32_t, std::int32_t, std::int32_t, bool, bool> weather; // timers and flags; levels are not saved
    std::vector<ChunkPos> chunks;
    std::vector<std::pair<BlockPos, std::uint32_t>> blocks;
    std::vector<std::pair<BlockPos, BlockEntity>> block_entities;
    std::multiset<std::tuple<int, BlockPos, int, Tick, int>> ticks;
    std::multiset<std::tuple<Vec3, Vec3, std::uint16_t, int, std::uint32_t, bool, int, std::uint32_t, std::uint32_t, std::int32_t, std::int32_t>> entities;
    std::vector<std::pair<std::string, circuit::gameplay::RandomSequence>> sequences;
    circuit::gameplay::SequenceSettings sequence_settings;
    bool operator==(const Kept&) const = default;
};
Kept kept(const WorldDump& world) {
    const auto& d = world.overworld();
    const auto& w = world.weather;
    Kept k{world.time, world.day_time, world.difficulty, world.game_rules, {w.clear_time, w.rain_time, w.thunder_time, w.raining, w.thundering},
           {}, d.blocks, d.block_entities, {}, {}, d.sequences, world.sequence_settings};
    for (const auto& c : d.chunks) k.chunks.push_back(c.pos);
    for (auto& [pos, be] : k.block_entities) {
        be.order = 0;
        be.ticked = 0;
    }
    for (const auto& t : d.ticks) {
        // A block tick of a block without rules of its own whose block is
        // gone (fire that went out) is not saved: it names no block.
        if (t.list == circuit::contracts::TickList::block && t.tick.block == BlockType::other
            && !state_at(world, t.tick.pos).is(BlockType::other))
            continue;
        k.ticks.insert({static_cast<int>(t.list), t.tick.pos, static_cast<int>(t.tick.block), t.tick.due, t.tick.priority});
    }
    for (const auto& e : d.entities)
        k.entities.insert({e.position, e.velocity, e.stack.item, e.stack.count, static_cast<std::uint32_t>(std::min(e.age, 6000)), e.on_ground,
                           static_cast<int>(e.kind), e.carried.raw(), e.fall_time, e.fuse, e.health});
    return k;
}
std::string describe(const Kept& a, const Kept& b) {
    if (a.time != b.time) return "time";
    if (a.day_time != b.day_time) return std::format("day time {} vs {}", a.day_time, b.day_time);
    if (a.difficulty != b.difficulty) return std::format("difficulty {} vs {}", a.difficulty, b.difficulty);
    if (a.game_rules != b.game_rules) return "game rules";
    if (a.weather != b.weather) return "weather";
    if (a.chunks != b.chunks) return std::format("chunks {} vs {}", a.chunks.size(), b.chunks.size());
    if (a.blocks != b.blocks) {
        for (std::size_t i = 0; i < std::min(a.blocks.size(), b.blocks.size()); ++i)
            if (a.blocks[i] != b.blocks[i])
                return std::format("block {} {} {}: {} vs {}", a.blocks[i].first.x, a.blocks[i].first.y, a.blocks[i].first.z,
                                   a.blocks[i].second, b.blocks[i].second);
        return std::format("block count {} vs {}", a.blocks.size(), b.blocks.size());
    }
    if (a.block_entities != b.block_entities) return "block entities";
    if (a.ticks != b.ticks) return std::format("ticks {} vs {}", a.ticks.size(), b.ticks.size());
    if (a.entities != b.entities) return std::format("entities {} vs {}", a.entities.size(), b.entities.size());
    if (a.sequences != b.sequences) return std::format("random sequences {} vs {}", a.sequences.size(), b.sequences.size());
    if (a.sequence_settings != b.sequence_settings) return "random sequence settings";
    return {};
}
} // namespace

int main() {
    Suite suite("anvil_tests");
    const auto root = temporary_directory();
    const char* keep = std::getenv("CIRCUIT_ANVIL_EXPORT_DIR");

    suite.run("placed block entities save what both official servers save", [&] {
        // tests/fixtures/collision/block-entities-*.txt: every block with a
        // block entity and the data it saves when just placed, as SNBT. The
        // registry gives each its type; default_block_entity its data.
        namespace registry = circuit::gameplay::registry;
        // SNBT as the target versions print it: keys in order, numbers with
        // their suffixes, strings in double quotes unless they hold one.
        std::function<std::string(const Tag&)> snbt = [&](const Tag& t) -> std::string {
            switch (t.type()) {
            case TagType::byte_: return std::format("{}b", t.integer());
            case TagType::short_: return std::format("{}s", t.integer());
            case TagType::int_: return std::format("{}", t.integer());
            case TagType::long_: return std::format("{}L", t.integer());
            case TagType::float_: return std::format("{:.1f}f", t.real());
            case TagType::double_: return std::format("{:.1f}d", t.real());
            case TagType::string: {
                const char q = t.text().find('"') != std::string::npos ? '\'' : '"';
                return q + t.text() + q;
            }
            case TagType::int_array: {
                std::string out = "[I;";
                for (std::size_t i = 0; i < t.array().size(); ++i) out += (i ? "," : "") + std::to_string(t.array()[i]);
                return out + "]";
            }
            case TagType::list: {
                std::string out = "[";
                for (std::size_t i = 0; i < t.items().size(); ++i) out += (i ? "," : "") + snbt(t.items()[i]);
                return out + "]";
            }
            case TagType::compound: {
                std::vector<std::size_t> order(t.keys().size());
                std::iota(order.begin(), order.end(), std::size_t{0});
                std::ranges::sort(order, [&](std::size_t a, std::size_t b) { return t.keys()[a] < t.keys()[b]; });
                std::string out = "{";
                for (std::size_t i = 0; i < order.size(); ++i) out += (i ? "," : "") + t.keys()[order[i]] + ":" + snbt(t.items()[order[i]]);
                return out + "}";
            }
            default: return "?";
            }
        };
        for (const auto release : {"1.21.8", "1.21.4"}) {
            std::ifstream in(fs::path(CIRCUIT_SOURCE_DIR) / "tests/fixtures/collision" / std::format("block-entities-{}.txt", release));
            std::set<std::uint32_t> with_entities;
            std::size_t blocks = 0;
            for (std::string line; std::getline(in, line);) {
                if (line.empty() || line.front() == '#') continue;
                const auto tab = line.find('\t');
                const auto name = line.substr(0, tab), saved = line.substr(tab + 1);
                const auto block = registry::find_block(release, name);
                require(block.has_value(), std::format("{} {}", release, name));
                with_entities.insert(*block);
                const auto type = registry::block_entity_type(registry::block(*block).default_state);
                if (saved == "none") {
                    // A moving piston's block entity exists only while it moves.
                    require(name == "minecraft:moving_piston" && type.empty(), name);
                    continue;
                }
                require(!type.empty(), std::format("{} {}: a type", release, name));
                const auto got = snbt(anvil::default_block_entity(registry::block(*block).default_state, release));
                require(got == saved, std::format("{} {}: official {}, Circuit {}", release, name, saved, got));
                ++blocks;
            }
            require(blocks > 160, std::format("{}: {} blocks", release, blocks));
            // Blocks the report does not list have none.
            for (std::uint32_t b = 0; b < registry::block_count(); ++b)
                if (!with_entities.contains(b) && registry::find_block(release, registry::game_name(b)) == b)
                    require(registry::block_entity_type(registry::block(b).default_state).empty(),
                            std::format("{} {}: no block entity", release, registry::game_name(b)));
        }
    });

    suite.run("scenario states round-trip through Anvil", [&] {
        std::size_t exported = 0, with_ticks = 0, with_entities = 0, with_moving = 0;
        for (const auto& s : all_scenarios()) {
            Engine engine(bundle(), s.seed);
            Admissions admit;
            for (Tick t = 1; t <= s.ticks; ++t) {
                engine.advance(admit(s.inputs(t)));
                if (t % std::max<Tick>(1, s.ticks / 4) != 0 && t != s.ticks) continue;
                const auto state = engine.dump();
                if (!state.overworld().block_events.empty()) continue;
                const auto target = root / std::format("{}-{}", s.name, t);
                const auto report = anvil::export_world(state, bundle(), target, {.level_name = s.name, .seed = s.seed});
                require(report.has_value(), std::format("{} at {}: export: {}", s.name, t, report ? "" : report.error()));
                const auto back = anvil::import_world(target, bundle());
                require(back.has_value(), std::format("{} at {}: import: {}", s.name, t, back ? "" : back.error()));
                const auto diff = describe(kept(state), kept(back->state));
                require(diff.empty(), std::format("{} at {}: {}", s.name, t, diff));
                require(back->state.weather.rain_level == (state.weather.raining ? 1.0f : 0.0f)
                            && back->state.weather.thunder_level == (state.weather.thundering ? 1.0f : 0.0f),
                        "imported weather levels follow their flags");
                // The imported state is one the engine accepts and continues.
                Engine resumed(bundle(), s.seed, back->state);
                resumed.advance({});
                ++exported;
                with_ticks += state.overworld().ticks.empty() ? 0 : 1;
                with_entities += state.overworld().entities.empty() ? 0 : 1;
                with_moving += std::ranges::any_of(state.overworld().block_entities, [](const auto& b) { return b.second.kind == BlockType::moving_piston; });
                if (keep) fs::copy(target, fs::path(keep) / target.filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            }
        }
        std::cout << std::format("  {} states exported and imported ({} with scheduled ticks, {} with item entities, {} with moving pistons)\n",
                                 exported, with_ticks, with_entities, with_moving);
        require(with_ticks > 0 && with_entities > 0 && with_moving > 0, "the states cover ticks, entities and moving pistons");
    });

    suite.run("falling blocks round-trip through Anvil", [&] {
        Engine engine(bundle(), 5);
        Admissions admit;
        engine.advance(admit({ticket(1, {0, 0}, 1)}));
        engine.advance(admit({place({3, -1, 3}, blocks::stone()), place({3, 6, 3}, registry_block("gravel"))}));
        for (int i = 0; i < 4; ++i) engine.advance({});
        const auto state = engine.dump();
        require(std::ranges::count_if(state.overworld().entities, [](const auto& e) { return e.kind == circuit::gameplay::EntityKind::falling_block; }) == 1,
                "gravel in the air");
        const auto target = root / "falling";
        require(anvil::export_world(state, bundle(), target, {.level_name = "falling", .seed = 5}).has_value(), "export");
        const auto back = anvil::import_world(target, bundle());
        require(back.has_value(), back ? "" : back.error());
        const auto diff = describe(kept(state), kept(back->state));
        require(diff.empty(), diff);
        // It falls on and lands after the import, once its chunk is loaded.
        Engine resumed(bundle(), 5, back->state);
        Admissions again;
        resumed.advance(again({ticket(1, {0, 0}, 1)}));
        for (int i = 0; i < 20; ++i) resumed.advance({});
        require(state_at(resumed.dump(), {3, 0, 3}) == registry_block("gravel"), "landed");
        // How far it had fallen is saved (fall_distance) and read back.
        const auto fall = [](const WorldDump& w) {
            for (const auto& e : w.overworld().entities)
                if (e.kind == circuit::gameplay::EntityKind::falling_block) return e.fall_distance;
            return -1.0;
        };
        require(fall(state) > 0.0 && fall(back->state) == fall(state), "the fall distance");
    });

    suite.run("cacti waiting to break round-trip through Anvil with their ticks", [&] {
        // Two cacti, stone set beside the lower: it breaks in its block tick
        // (plant_supports), and the upper one waits for its own, a tick
        // later; that tick is saved and read back.
        Engine engine(bundle(), 5);
        Admissions admit;
        engine.advance(admit({ticket(1, {0, 0}, 1)}));
        const auto cactus = registry_block("cactus", {{"age", "0"}});
        engine.advance(admit({place({3, -1, 3}, registry_block("sand")), place({3, 0, 3}, cactus), place({3, 1, 3}, cactus)}));
        engine.advance(admit({place({4, 0, 3}, blocks::stone())}));
        const auto state = engine.dump();
        require(state_at(state, {3, 0, 3}).is_air() && state_at(state, {3, 1, 3}) == cactus
                    && std::ranges::any_of(state.overworld().ticks, [](const auto& t) { return t.tick.pos == BlockPos{3, 1, 3}; }),
                "the upper cactus's tick");
        const auto target = root / "cactus";
        require(anvil::export_world(state, bundle(), target, {.level_name = "cactus", .seed = 5}).has_value(), "export");
        const auto back = anvil::import_world(target, bundle());
        require(back.has_value() && back->report.unsupported.empty(), back ? "unsupported content" : back.error());
        const auto diff = describe(kept(state), kept(back->state));
        require(diff.empty(), diff);
        Engine resumed(bundle(), 5, back->state);
        Admissions again;
        // Its tick runs once its chunk is loaded again.
        resumed.advance(again({ticket(1, {0, 0}, 1)}));
        for (int i = 0; i < 10; ++i) resumed.advance({});
        const auto after = resumed.dump();
        require(state_at(after, {3, 1, 3}).is_air() && after.overworld().ticks.empty(), "broken in its tick");
    });

    suite.run("buttons and torches round-trip through Anvil with their ticks", [&] {
        using circuit::gameplay::blocks::ButtonKind;
        Engine engine(bundle(), 5);
        Admissions admit;
        engine.advance(admit({ticket(1, {0, 0}, 1)}));
        engine.advance(admit({place({3, 0, 3}, blocks::stone()), place({4, 0, 3}, blocks::button(ButtonKind::stone, false, Direction::east)),
                              place({3, 0, 5}, blocks::stone()), place({4, 0, 5}, blocks::wall_torch(Direction::east)),
                              place({6, 0, 6}, blocks::torch()), place({2, 0, 5}, blocks::button(ButtonKind::warped, false, Direction::west))}));
        engine.advance(admit({circuit::contracts::UseBlock{{4, 0, 3}}, circuit::contracts::UseBlock{{2, 0, 5}}}));
        engine.advance({});
        const auto state = engine.dump();
        require(state.overworld().ticks.size() >= 2, "the pressed buttons wait for their release");
        const auto target = root / "buttons";
        require(anvil::export_world(state, bundle(), target, {.level_name = "buttons", .seed = 5}).has_value(), "export");
        const auto back = anvil::import_world(target, bundle());
        require(back.has_value(), back ? "" : back.error());
        const auto diff = describe(kept(state), kept(back->state));
        require(diff.empty(), diff);
        // Released on time after the import, once the chunk is loaded.
        Engine resumed(bundle(), 5, back->state);
        Admissions again;
        resumed.advance(again({ticket(1, {0, 0}, 1)}));
        for (int i = 0; i < 40; ++i) resumed.advance({});
        const auto end = resumed.dump();
        require(!state_at(end, {4, 0, 3}).flag() && !state_at(end, {2, 0, 5}).flag(), "both released");
        require(state_at(end, {2, 0, 5}).variant() == static_cast<std::uint8_t>(ButtonKind::warped), "the warped button kept its kind");
        require(state_at(end, {4, 0, 5}) == blocks::wall_torch(Direction::east) && state_at(end, {6, 0, 6}) == blocks::torch(), "the torches");
    });

    suite.run("blocks with block entities that players place export their official defaults", [&] {
        // A furnace, a shulker box and a barrel placed on stone: each keeps an
        // empty block entity, written out as its type's defaults and read back
        // as saved data.
        namespace registry = circuit::gameplay::registry;
        Engine engine(bundle(), 5);
        Admissions admit;
        const circuit::contracts::PlayerKey alex{1, 2};
        engine.advance(admit({ticket(1, {0, 0}, 1)}));
        std::vector<CommandBody> commands;
        for (const auto& [x, item] : {std::pair{1, "minecraft:furnace"}, std::pair{3, "minecraft:red_shulker_box"}, std::pair{5, "minecraft:barrel"}}) {
            commands.push_back(place({x, 0, 3}, blocks::stone()));
            commands.push_back(circuit::contracts::SetPlayerSlot{alex, 36, {circuit::gameplay::items::by_name(item).value(), 1, 0}});
            commands.push_back(circuit::contracts::PlayerUse{alex, 36, {x, 0, 3}, Direction::up, Direction::north, Direction::north, false, false, 3, 3});
        }
        engine.advance(admit(std::move(commands)));
        const auto state = engine.dump();
        for (const int x : {1, 3, 5}) {
            const auto* be = block_entity_at(state, {x, 1, 3});
            require(be && be->kind == BlockType::other && be->saved.bytes().empty(), std::format("an empty block entity at {}", x));
        }
        const auto target = root / "placed-entities";
        require(anvil::export_world(state, bundle(), target, {.level_name = "placed", .seed = 5}).has_value(), "export");
        const auto back = anvil::import_world(target, bundle());
        require(back.has_value(), back ? "" : back.error());
        for (const auto& [x, saved] : {std::pair{1, "furnace"}, std::pair{3, "shulker_box"}, std::pair{5, "barrel"}}) {
            const auto* be = block_entity_at(back->state, {x, 1, 3});
            require(be && be->kind == BlockType::other, std::format("{}: imported", saved));
            circuit::protocol::Reader r(be->saved.bytes());
            const auto tag = nbt::read_network(r);
            const auto block = state_at(state, {x, 1, 3});
            require(tag && *tag == nbt::canonical(anvil::default_block_entity(block.registry_state(), "1.21.8")),
                    std::format("{}: the official defaults", saved));
        }
    });

    suite.run("daylight detectors round-trip through Anvil with their block entities", [&] {
        Engine engine(bundle(), 5);
        Admissions admit;
        engine.advance(admit({ticket(1, {0, 0}, 1)}));
        engine.advance(admit({circuit::contracts::SetDayTime{6000}, circuit::contracts::SetDifficulty{3}, place({3, 0, 3}, blocks::daylight_detector()),
                              place({5, 0, 3}, blocks::daylight_detector(true))}));
        while (engine.dump().time % 20 != 0) engine.advance({});
        const auto state = engine.dump();
        require(state_at(state, {3, 0, 3}) == blocks::daylight_detector(false, 15) && state.difficulty == 3,
                "the detector saw the noon sky; the world is hard");
        const auto target = root / "daylight";
        require(anvil::export_world(state, bundle(), target, {.level_name = "daylight", .seed = 5}).has_value(), "export");
        const auto back = anvil::import_world(target, bundle());
        require(back.has_value(), back ? "" : back.error());
        const auto diff = describe(kept(state), kept(back->state));
        require(diff.empty(), diff);
        const auto& entities = back->state.overworld().block_entities;
        require(std::ranges::count_if(entities, [](const auto& e) { return e.second.kind == BlockType::daylight_detector; }) == 2,
                "both block entities");
        // The imported detectors keep ticking: at midnight the inverted one gives 11.
        Engine resumed(bundle(), 5, back->state);
        Admissions again;
        resumed.advance(again({ticket(1, {0, 0}, 1), circuit::contracts::SetDayTime{18000}}));
        for (int i = 0; i < 20; ++i) resumed.advance({});
        const auto end = resumed.dump();
        require(state_at(end, {3, 0, 3}) == blocks::daylight_detector() && state_at(end, {5, 0, 3}) == blocks::daylight_detector(true, 11),
                "midnight after the import");
    });

    suite.run("dispensers and droppers round-trip through Anvil with their contents and pending ticks", [&] {
        Engine engine(bundle(), 5);
        Admissions admit;
        engine.advance(admit({ticket(1, {0, 0}, 1)}));
        engine.advance(admit({place({3, 0, 3}, blocks::dispenser(Direction::up)), place({5, 0, 3}, blocks::dispenser(Direction::north, true))}));
        engine.advance(admit({insert({3, 0, 3}, 4, items::stone, 7), insert({5, 0, 3}, 8, items::glass, 2)}));
        // Triggered, with the dispense tick pending at the export.
        engine.advance(admit({place({3, 0, 2}, blocks::redstone_block())}));
        const auto state = engine.dump();
        require(state_at(state, {3, 0, 3}) == blocks::dispenser(Direction::up, false, true), "triggered");
        const auto target = root / "dispensers";
        require(anvil::export_world(state, bundle(), target, {.level_name = "dispensers", .seed = 5}).has_value(), "export");
        const auto back = anvil::import_world(target, bundle());
        require(back.has_value(), back ? "" : back.error());
        const auto diff = describe(kept(state), kept(back->state));
        require(diff.empty(), diff);
        const auto* dropper = block_entity_at(back->state, {5, 0, 3});
        require(dropper && dropper->kind == BlockType::dispenser && dropper->slots.size() == 9 && dropper->slots[8] == ItemStack{items::glass, 2},
                "the dropper's slots");
        // The imported dispenser fires its pending tick.
        Engine resumed(bundle(), 5, back->state);
        Admissions again;
        resumed.advance(again({ticket(1, {0, 0}, 1)}));
        // Its chunk ticks again once loaded; then the pending tick runs.
        for (int i = 0; i < 40; ++i) resumed.advance({});
        const auto end = resumed.dump();
        const auto* dispenser = block_entity_at(end, {3, 0, 3});
        require(dispenser && dispenser->slots[4] == ItemStack{items::stone, 6}, "one stone dispensed after the import");
    });

    suite.run("primed TNT round-trips through Anvil with its fuse", [&] {
        Engine engine(bundle(), 5);
        Admissions admit;
        engine.advance(admit({ticket(1, {0, 0}, 1)}));
        engine.advance(admit({place({3, 0, 3}, registry_block("tnt")), place({4, 0, 3}, blocks::redstone_block())}));
        for (int i = 0; i < 10; ++i) engine.advance({});
        const auto state = engine.dump();
        const auto primed = std::ranges::find_if(state.overworld().entities, [](const auto& e) { return e.kind == circuit::gameplay::EntityKind::primed_tnt; });
        // Primed in the tick of the placement, which counts too.
        require(primed != state.overworld().entities.end() && primed->fuse == 69,
                std::format("primed, eleven ticks into its fuse: {}", primed == state.overworld().entities.end() ? -1 : primed->fuse));
        const auto target = root / "tnt";
        require(anvil::export_world(state, bundle(), target, {.level_name = "tnt", .seed = 5}).has_value(), "export");
        const auto back = anvil::import_world(target, bundle());
        require(back.has_value(), back ? "" : back.error());
        const auto diff = describe(kept(state), kept(back->state));
        require(diff.empty(), diff);
        // Its fuse runs on once its chunk is loaded again: it ticks from two
        // ticks after the load and explodes 69 ticks later.
        Engine resumed(bundle(), 5, back->state);
        Admissions again;
        resumed.advance(again({ticket(1, {0, 0}, 1)}));
        const auto count = [&] {
            return std::ranges::count_if(resumed.dump().overworld().entities, [](const auto& e) { return e.kind == circuit::gameplay::EntityKind::primed_tnt; });
        };
        int ticks = 1;
        while (count() == 1 && ticks < 100) {
            resumed.advance({});
            ++ticks;
        }
        require(ticks == 71, std::format("exploded after {} ticks", ticks));
    });

    suite.run("arrows round-trip through Anvil", [&] {
        Engine engine(bundle(), 5);
        Admissions admit;
        engine.advance(admit({ticket(1, {0, 0}, 1)}));
        engine.advance(admit({place({8, 0, 3}, blocks::obsidian()), SummonEntity{"minecraft:arrow", {2.5, 0.5, 3.5}, {1.0, 0.05, 0.0}, 80, 100},
                              SummonEntity{"minecraft:arrow", {2.5, 5.5, 3.5}, {0.2, 0.3, 0.0}, 80, 0}}));
        for (int i = 0; i < 8; ++i) engine.advance({});
        const auto state = engine.dump();
        const auto stuck = std::ranges::count_if(state.overworld().entities, [](const auto& e) { return e.kind == circuit::gameplay::EntityKind::arrow && e.in_ground; });
        require(stuck == 1, "one arrow stuck, one in flight");
        const auto target = root / "arrows";
        require(anvil::export_world(state, bundle(), target, {.level_name = "arrows", .seed = 5}).has_value(), "export");
        const auto back = anvil::import_world(target, bundle());
        require(back.has_value(), back ? "" : back.error());
        const auto lost = [](const WorldDump& w) {
            std::vector<std::tuple<Vec3, Vec3, bool, std::int32_t, std::int32_t>> out;
            for (const auto& e : w.overworld().entities) out.emplace_back(e.position, e.velocity, e.in_ground, e.life, e.fire);
            std::ranges::sort(out);
            return out;
        };
        require(lost(state) == lost(back->state), "arrows kept with their state");
    });

    suite.run("minecarts and boats round-trip through Anvil", [&] {
        Engine engine(bundle(), 5);
        Admissions admit;
        engine.advance(admit({ticket(1, {0, 0}, 1)}));
        // Moving south the minecart turns its yaw to -90, which its Rotation keeps.
        engine.advance(admit({place({3, -1, 3}, blocks::stone()), SummonEntity{"minecraft:minecart", {3.5, 0.0, 3.5}, {0.0, 0.0, 0.1}, 80, 0},
                              SummonEntity{"minecraft:cherry_boat", {6.5, 4.0, 6.5}, {0.0, 0.0, 0.0}, 80, 0}}));
        for (int i = 0; i < 3; ++i) engine.advance({});
        const auto state = engine.dump();
        const auto target = root / "vehicles";
        require(anvil::export_world(state, bundle(), target, {.level_name = "vehicles", .seed = 5}).has_value(), "export");
        const auto back = anvil::import_world(target, bundle());
        require(back.has_value(), back ? "" : back.error());
        const auto kept = [](const WorldDump& w) {
            std::vector<std::tuple<int, Vec3, Vec3, int, float>> out;
            for (const auto& e : w.overworld().entities) out.emplace_back(static_cast<int>(e.kind), e.position, e.velocity, e.variant, e.yaw);
            std::ranges::sort(out);
            return out;
        };
        require(kept(state) == kept(back->state) && kept(state).size() == 2, "kinds, positions, velocities, the boat's wood and the yaw");
        require(std::ranges::any_of(state.overworld().entities, [](const auto& e) { return e.kind == circuit::gameplay::EntityKind::minecart && e.yaw == -90.0f; }),
                "the minecart's yaw");
    });

    suite.run("random sequences round-trip through Anvil as the official server saves them", [&] {
        // The loot_many skeleton's sequences as the official 1.21.8 server
        // saved them, with the settings' flags absent (true).
        using circuit::gameplay::RandomSequence;
        const std::vector<std::tuple<std::string, std::int64_t, std::int64_t>> saved{
            {"minecraft:blocks/dead_bush", 980453112882939729, 5313872344700270703},
            {"minecraft:blocks/sweet_berry_bush", 4585398311947135046, -4978303496063927502},
            {"minecraft:blocks/short_grass", 2022789406501655404, -468754072450410102}};
        const auto sequences_file = [](const auto& entries, std::optional<std::pair<bool, bool>> flags, std::int32_t salt) {
            auto list = Tag::compound();
            for (const auto& [id, low, high] : entries) {
                auto entry = Tag::compound();
                entry.set("source", Tag::long_array({low, high}));
                list.set(id, std::move(entry));
            }
            auto data = Tag::compound();
            data.set("salt", Tag::int_(salt));
            if (flags) {
                data.set("include_world_seed", Tag::boolean(flags->first));
                data.set("include_sequence_id", Tag::boolean(flags->second));
            }
            data.set("sequences", std::move(list));
            auto root = Tag::compound();
            root.set("data", std::move(data));
            root.set("DataVersion", Tag::int_(anvil::supported_data_version));
            return gzip_bytes(named(root));
        };
        const auto good = chunk({0, 0}, {block("minecraft:air"), block("minecraft:stone")});
        const auto dir = hand_made_world(root / "sequences", {{{0, 0}, good}});
        write(dir / "data" / "random_sequences.dat", sequences_file(saved, std::nullopt, 0));
        const auto in = anvil::import_world(dir, bundle());
        require(in.has_value(), in ? "" : in.error());
        const auto& sequences = in->state.overworld().sequences;
        require(sequences.size() == 3 && sequences[0].first == "minecraft:blocks/dead_bush" && sequences[2].first == "minecraft:blocks/sweet_berry_bush",
                "every sequence, by id");
        require(sequences[1].second == RandomSequence{2022789406501655404ULL, static_cast<std::uint64_t>(-468754072450410102LL)},
                "the grass sequence's state");
        require(in->state.sequence_settings == circuit::gameplay::SequenceSettings{}, "absent flags are true");
        // The engine continues the sequences, and an export writes them as the
        // official server does.
        Engine resumed(bundle(), 42, in->state);
        resumed.advance({});
        const auto state = resumed.dump();
        require(state.overworld().sequences == sequences, "the engine keeps the sequences");
        const auto target = root / "sequences-out";
        require(anvil::export_world(state, bundle(), target, {.level_name = "sequences", .seed = 42}).has_value(), "export");
        const auto raw = gunzip_bytes(read(target / "data" / "random_sequences.dat"));
        circuit::protocol::Reader r(raw);
        std::string name;
        const auto file = nbt::read_named(r, name);
        require(file.has_value(), "the exported file is NBT");
        const auto* data = file->find("data");
        require(file->find("DataVersion") && file->find("DataVersion")->integer() == anvil::supported_data_version && data, "DataVersion and data");
        require(data->find("salt") && data->find("salt")->integer() == 0 && !data->find("include_world_seed") && !data->find("include_sequence_id"),
                "the salt, and no flags while they are true");
        const auto* list = data->find("sequences");
        require(list && list->keys().size() == 3, "three sequences");
        for (const auto& [id, low, high] : saved) {
            const auto* entry = list->find(id);
            require(entry && entry->find("source") && entry->find("source")->array() == std::vector<std::int64_t>{low, high},
                    std::format("{}: its saved state", id));
        }
        if (keep) fs::copy(target, fs::path(keep) / target.filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        const auto back = anvil::import_world(target, bundle());
        require(back.has_value(), back ? "" : back.error());
        const auto diff = describe(kept(state), kept(back->state));
        require(diff.empty(), diff);
        // Settings that are not the defaults survive both ways; ids without a
        // namespace are the game's.
        const auto other = hand_made_world(root / "sequences-salted", {{{0, 0}, good}});
        write(other / "data" / "random_sequences.dat",
              sequences_file(std::vector<std::tuple<std::string, std::int64_t, std::int64_t>>{{"blocks/stone", 1, 2}}, std::pair{false, true}, -7));
        const auto salted = anvil::import_world(other, bundle());
        require(salted.has_value(), salted ? "" : salted.error());
        require(salted->state.sequence_settings == circuit::gameplay::SequenceSettings{-7, false, true}, "the salt and the flags");
        require(salted->state.overworld().sequences.size() == 1 && salted->state.overworld().sequences[0].first == "minecraft:blocks/stone",
                "the namespace");
        const auto salted_out = root / "sequences-salted-out";
        require(anvil::export_world(salted->state, bundle(), salted_out, {.level_name = "salted", .seed = 42}).has_value(), "export");
        if (keep) fs::copy(salted_out, fs::path(keep) / salted_out.filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        const auto salted_back = anvil::import_world(salted_out, bundle());
        require(salted_back.has_value() && salted_back->state.sequence_settings == salted->state.sequence_settings
                    && salted_back->state.overworld().sequences == salted->state.overworld().sequences,
                "the settings round-trip");
        // Malformed sequences fail the import.
        const auto broken = [&](std::string label, std::vector<std::tuple<std::string, std::int64_t, std::int64_t>> entries, bool short_source) {
            const auto d = hand_made_world(root / ("sequences-" + label), {{{0, 0}, good}});
            auto list = Tag::compound();
            for (const auto& [id, low, high] : entries) {
                auto entry = Tag::compound();
                entry.set("source", short_source ? Tag::long_array({low}) : Tag::long_array({low, high}));
                list.set(id, std::move(entry));
            }
            auto data = Tag::compound();
            data.set("sequences", std::move(list));
            auto root_tag = Tag::compound();
            root_tag.set("data", std::move(data));
            root_tag.set("DataVersion", Tag::int_(anvil::supported_data_version));
            write(d / "data" / "random_sequences.dat", gzip_bytes(named(root_tag)));
            const auto r = anvil::import_world(d, bundle());
            require(!r && r.error().find("random_sequences.dat") != std::string::npos, std::format("{}: {}", label, r ? "imported" : r.error()));
        };
        broken("short", {{"minecraft:blocks/stone", 1, 2}}, true);
        broken("id", {{"Minecraft:Stone", 1, 2}}, false);
    });

    suite.run("unsupported content is rejected or substituted by policy", [&] {
        // Every block state of the target version imports; unknown blocks and
        // invalid properties are unsupported.
        const auto dir = hand_made_world(root / "unsupported",
                                         {{{0, 0}, chunk({0, 0}, {block("minecraft:air"), block("minecraft:dirt", {{"snowy", "true"}}),
                                                                  block("minecraft:bedrock", {{"bogus", "1"}}),
                                                                  block("minecraft:glass_pane", {{"east", "maybe"}}),
                                                                  block("examplemod:gadget"),
                                                                  block("minecraft:lever", {{"face", "floor"}, {"facing", "east"}, {"powered", "true"}})},
                                                          12)}});
        const auto rejected = anvil::import_world(dir, bundle());
        require(!rejected, "rejected by default");
        for (const auto* kind : {"block minecraft:dirt", "block minecraft:bedrock", "block minecraft:glass_pane", "block examplemod:gadget"})
            require(rejected.error().find(kind) != std::string::npos, std::format("names {}: {}", kind, rejected.error()));
        require(rejected.error().find("lever") == std::string::npos, "supported blocks are not listed");
        const auto substituted = anvil::import_world(dir, bundle(), {.unsupported = anvil::Unsupported::substitute});
        require(substituted.has_value(), "substitution succeeds");
        const auto at = [&](std::int32_t i) { return state_at(substituted->state, {i & 15, i >> 8, (i >> 4) & 15}); };
        require(at(1) == blocks::stone() && at(2) == blocks::obsidian() && at(3) == blocks::glass() && at(4) == blocks::air() &&
                    at(5) == blocks::lever(true, Direction::east, blocks::LeverFace::floor) && at(7) == blocks::stone(),
                "stand-ins by shape; supported blocks kept");
        require(substituted->report.unsupported.at("block minecraft:dirt") == 2, "counts per kind");
        require(substituted->report.spawn == BlockPos{1, -61, 2} && substituted->report.seed == 42 && substituted->state.time == 100,
                "level data");
    });

    suite.run("item components, block entities and unsupported content", [&] {
        auto c = chunk({0, 0}, {block("minecraft:air"), block("minecraft:chest", {{"facing", "north"}, {"type", "single"}, {"waterlogged", "false"}}),
                                block("minecraft:hopper", {{"enabled", "true"}, {"facing", "down"}}),
                                block("minecraft:oak_sign", {{"rotation", "0"}, {"waterlogged", "false"}})});
        // `damage` -1: a custom name and lore instead of a damage component;
        // -2: enchantments, which have no save mapping yet.
        const auto item = [](std::int8_t slot, std::string id, std::int32_t count, std::optional<std::int32_t> damage = std::nullopt) {
            auto t = Tag::compound();
            t.set("Slot", Tag::byte(slot));
            t.set("id", Tag::string(std::move(id)));
            t.set("count", Tag::int_(count));
            if (damage) {
                auto c = Tag::compound();
                if (*damage == -1) {
                    c.set("minecraft:custom_name", Tag::string("named"));
                    auto lore = Tag::list(TagType::string);
                    lore.push(Tag::string("line"));
                    c.set("minecraft:lore", std::move(lore));
                } else if (*damage == -2) {
                    auto levels = Tag::compound();
                    levels.set("minecraft:sharpness", Tag::int_(3));
                    c.set("minecraft:enchantments", std::move(levels));
                } else {
                    c.set("minecraft:damage", Tag::int_(*damage));
                }
                t.set("components", std::move(c));
            }
            return t;
        };
        auto bes = Tag::list(TagType::compound);
        auto chest = Tag::compound();
        chest.set("id", Tag::string("minecraft:chest"));
        chest.set("x", Tag::int_(1));
        chest.set("y", Tag::int_(0));
        chest.set("z", Tag::int_(0));
        auto items = Tag::list(TagType::compound);
        items.push(item(0, "minecraft:redstone", 64));
        items.push(item(5, "examplemod:gizmo", 3));
        items.push(item(6, "minecraft:stone", 2, -1));
        items.push(item(7, "minecraft:iron_pickaxe", 1, 12)); // a damaged tool is supported
        items.push(item(8, "minecraft:iron_pickaxe", 1, 251)); // beyond its durability (250) it is not
        items.push(item(9, "minecraft:stone", 1, 1));          // nor is damage on an item without durability
        items.push(item(10, "minecraft:diamond_sword", 1, -2)); // enchantments have no save mapping yet
        {
            // Components that name registry entries: a trim, banner patterns, a base color.
            auto armor = Tag::compound();
            armor.set("Slot", Tag::byte(11));
            armor.set("id", Tag::string("minecraft:iron_chestplate"));
            armor.set("count", Tag::int_(1));
            auto trim = Tag::compound();
            trim.set("material", Tag::string("minecraft:gold"));
            trim.set("pattern", Tag::string("minecraft:coast"));
            auto c = Tag::compound();
            c.set("minecraft:trim", std::move(trim));
            armor.set("components", std::move(c));
            items.push(std::move(armor));
            auto banner = Tag::compound();
            banner.set("Slot", Tag::byte(12));
            banner.set("id", Tag::string("minecraft:white_banner"));
            banner.set("count", Tag::int_(1));
            auto layers = Tag::list(TagType::compound);
            auto layer = Tag::compound();
            layer.set("pattern", Tag::string("minecraft:stripe_top"));
            layer.set("color", Tag::string("red"));
            layers.push(std::move(layer));
            auto bc = Tag::compound();
            bc.set("minecraft:banner_patterns", std::move(layers));
            bc.set("minecraft:base_color", Tag::string("lime"));
            banner.set("components", std::move(bc));
            items.push(std::move(banner));
        }
        chest.set("Items", std::move(items));
        bes.push(std::move(chest));
        auto hopper = Tag::compound();
        hopper.set("id", Tag::string("minecraft:hopper"));
        hopper.set("x", Tag::int_(2));
        hopper.set("y", Tag::int_(0));
        hopper.set("z", Tag::int_(0));
        hopper.set("TransferCooldown", Tag::int_(3));
        hopper.set("CustomName", Tag::string("named"));
        hopper.set("components", Tag::compound()); // the game writes an empty map; it is not unsupported
        bes.push(std::move(hopper));
        auto sign = Tag::compound();
        sign.set("id", Tag::string("minecraft:sign"));
        sign.set("x", Tag::int_(3));
        sign.set("y", Tag::int_(0));
        sign.set("z", Tag::int_(0));
        auto front = Tag::compound();
        front.set("has_glowing_text", Tag::byte(0));
        sign.set("front_text", std::move(front));
        sign.set("is_waxed", Tag::byte(1));
        bes.push(std::move(sign));
        c.set("block_entities", std::move(bes));
        const auto dir = root / "entities-world";
        hand_made_world(dir, {{{0, 0}, std::move(c)}});
        // Item entities and a mob in the entity region.
        auto ents = Tag::compound();
        ents.set("DataVersion", Tag::int_(anvil::supported_data_version));
        ents.set("Position", Tag::int_array({0, 0}));
        auto list = Tag::list(TagType::compound);
        const auto doubles = [](double a, double b, double c) {
            auto l = Tag::list(TagType::double_);
            l.push(Tag::double_(a));
            l.push(Tag::double_(b));
            l.push(Tag::double_(c));
            return l;
        };
        auto drop = Tag::compound();
        drop.set("id", Tag::string("minecraft:item"));
        drop.set("Pos", doubles(4.5, 1.25, 4.5));
        drop.set("Motion", doubles(0.0, -0.04, 0.0));
        drop.set("Age", Tag::short_(20));
        auto stack = Tag::compound();
        stack.set("id", Tag::string("minecraft:wheat"));
        stack.set("count", Tag::int_(3));
        drop.set("Item", std::move(stack));
        list.push(std::move(drop));
        auto pig = Tag::compound();
        pig.set("id", Tag::string("minecraft:pig"));
        pig.set("Pos", doubles(6.5, 1.0, 6.5));
        list.push(std::move(pig));
        ents.set("Entities", std::move(list));
        write(dir / "entities" / "r.0.0.mca", region({{{0, 0}, std::move(ents)}}, 1));
        const auto rejected = anvil::import_world(dir, bundle());
        require(!rejected, "rejected");
        for (const auto* kind : {"item examplemod:gizmo", "item component minecraft:damage with an invalid value (2)",
                                 "item component minecraft:enchantments", "entity minecraft:pig"})
            require(rejected.error().find(kind) != std::string::npos, std::format("names {}: {}", kind, rejected.error()));
        require(rejected.error().find("hopper") == std::string::npos && rejected.error().find("sign") == std::string::npos,
                "names and block entities of other blocks are kept: " + rejected.error());
        const auto r = anvil::import_world(dir, bundle(), {.unsupported = anvil::Unsupported::substitute});
        require(r.has_value(), "substituted");
        const auto* c0 = block_entity_at(r->state, {1, 0, 0});
        require(c0 && c0->slots[0] == ItemStack{items::redstone, 64} && c0->slots[5].empty(), "supported items kept");
        require(c0->slots[6].item == items::stone && c0->slots[6].count == 2 && c0->slots[6].components.added().size() == 2,
                "a custom name and lore are kept");
        require(c0->slots[7] == ItemStack{items::iron_pickaxe, 1, 12} && c0->slots[8].empty() && c0->slots[9].empty() && c0->slots[10].empty(),
                "damage kept when valid");
        require(c0->slots[11].components.added().size() == 1 && c0->slots[12].components.added().size() == 2,
                "trims, banner patterns and base colors are kept");
        // Damage, components, names and the sign's data export and come back.
        {
            auto state = r->state;
            const auto out = root / "damaged-export";
            require(anvil::export_world(state, bundle(), out, {.level_name = "damaged", .seed = 1}).has_value(), "exported");
            const auto again = anvil::import_world(out, bundle());
            require(again.has_value(), again ? "" : again.error());
            const auto* chest_again = block_entity_at(again->state, {1, 0, 0});
            require(chest_again->slots[7] == ItemStack{items::iron_pickaxe, 1, 12}, "damage round-trips");
            require(chest_again->slots[6] == c0->slots[6], "components round-trip");
            require(chest_again->slots[11] == c0->slots[11] && chest_again->slots[12] == c0->slots[12], "named entries round-trip");
            require(block_entity_at(again->state, {2, 0, 0})->components == block_entity_at(r->state, {2, 0, 0})->components,
                    "the hopper's name round-trips");
            require(block_entity_at(again->state, {3, 0, 0})->saved == block_entity_at(r->state, {3, 0, 0})->saved,
                    "the sign's data round-trips");
        }
        const auto* h = block_entity_at(r->state, {2, 0, 0});
        require(h && h->cooldown == 3 && h->components.added().size() == 1, "hopper cooldown and name");
        const auto* sign_be = block_entity_at(r->state, {3, 0, 0});
        require(sign_be && sign_be->kind == BlockType::other && !sign_be->saved.empty(), "the sign keeps its saved data");
        require(r->state.overworld().entities.size() == 1 && r->state.overworld().entities[0].stack == ItemStack{items::wheat, 3} &&
                    r->state.overworld().entities[0].position == Vec3{4.5, 1.25, 4.5} &&
                    r->state.overworld().entities[0].age == 20,
                "item entity");
        require(r->state.overworld().block_entities.size() == 3, "the sign's block entity is kept");
    });

    suite.run("malformed worlds fail cleanly", [&] {
        const auto good = chunk({0, 0}, {block("minecraft:air"), block("minecraft:stone")});
        const auto expect_error = [&](const fs::path& dir, std::string_view what, std::string_view label) {
            const auto r = anvil::import_world(dir, bundle());
            require(!r && r.error().find(what) != std::string::npos, std::format("{}: {}", label, r ? "imported" : r.error()));
        };
        expect_error(hand_made_world(root / "old", {{{0, 0}, good}}, level_dat(4189)), "DataVersion 4189", "older world");
        expect_error(root / "missing", "not a world directory", "missing directory");
        // Day time and game rules: kept when valid, rules the version lacks ignored.
        const auto with_rules = [](std::vector<std::pair<std::string, std::string>> rules, std::int64_t day) {
            auto level = level_dat();
            auto data = *level.find("Data");
            data.set("DayTime", Tag::long_(day));
            auto tag = Tag::compound();
            for (auto& [name, value] : rules) tag.set(name, Tag::string(value));
            data.set("GameRules", std::move(tag));
            level.set("Data", std::move(data));
            return level;
        };
        {
            const auto dir = hand_made_world(root / "rules", {{{0, 0}, good}},
                                             with_rules({{"doDaylightCycle", "false"}, {"randomTickSpeed", "3"}, {"spawnChunkRadius", "5"},
                                                         {"someModRule", "x"}, {"minecartMaxSpeed", "8"}},
                                                        4294967318));
            const auto r = anvil::import_world(dir, bundle());
            require(r && r->state.day_time == 4294967318
                        && r->state.game_rules == std::map<std::string, std::int32_t>{{"doDaylightCycle", 0}, {"spawnChunkRadius", 5}}
                        && std::ranges::any_of(r->report.notes, [](const std::string& n) { return n.starts_with("2 game rules"); })
                        && r->state.difficulty == 2,
                    "day time and game rules imported; normal when the file names no difficulty");
        }
        expect_error(hand_made_world(root / "rule-bounds", {{{0, 0}, good}}, with_rules({{"spawnChunkRadius", "33"}}, 0)),
                     "game rule spawnChunkRadius has the invalid value 33", "a rule out of bounds");
        expect_error(hand_made_world(root / "rule-bool", {{{0, 0}, good}}, with_rules({{"keepInventory", "1"}}, 0)),
                     "game rule keepInventory has the invalid value 1", "a boolean rule");
        expect_error(hand_made_world(root / "day", {{{0, 0}, good}}, with_rules({}, -1)), "negative day time", "a negative day time");
        {
            auto level = level_dat();
            auto data = *level.find("Data");
            data.set("Difficulty", Tag::byte(4));
            level.set("Data", std::move(data));
            expect_error(hand_made_world(root / "difficulty", {{{0, 0}, good}}, level), "invalid difficulty", "a difficulty above hard");
        }
        {
            auto bad = good;
            bad.set("xPos", Tag::int_(5));
            expect_error(hand_made_world(root / "moved", {{{0, 0}, bad}}), "does not match", "misplaced chunk");
        }
        {
            auto bad = chunk({0, 0}, {block("minecraft:air"), block("minecraft:stone")});
            auto sections = Tag::list(TagType::compound);
            auto s = Tag::compound();
            s.set("Y", Tag::byte(0));
            auto states = Tag::compound();
            auto pal = Tag::list(TagType::compound);
            pal.push(block("minecraft:air"));
            pal.push(block("minecraft:stone"));
            states.set("palette", std::move(pal));
            states.set("data", Tag::long_array({1, 2, 3}));
            s.set("block_states", std::move(states));
            sections.push(std::move(s));
            bad.set("sections", std::move(sections));
            expect_error(hand_made_world(root / "short", {{{0, 0}, bad}}), "wrong length", "short block array");
        }
        {
            auto bad = chunk({0, 0}, {block("minecraft:air"), block("minecraft:stone")});
            auto sections = Tag::list(TagType::compound);
            auto s = Tag::compound();
            s.set("Y", Tag::byte(0));
            auto states = Tag::compound();
            auto pal = Tag::list(TagType::compound);
            pal.push(block("minecraft:air"));
            pal.push(block("minecraft:stone"));
            states.set("palette", std::move(pal));
            states.set("data", Tag::long_array(std::vector<std::int64_t>(256, 0x0F0F0F0F0F0F0F0F)));
            s.set("block_states", std::move(states));
            sections.push(std::move(s));
            bad.set("sections", std::move(sections));
            expect_error(hand_made_world(root / "index", {{{0, 0}, bad}}), "outside its palette", "palette index");
        }
        {
            const auto dir = hand_made_world(root / "lz4", {{{0, 0}, good}});
            auto bytes = read(dir / "region" / "r.0.0.mca");
            bytes[8192 + 4] = std::byte{4};
            write(dir / "region" / "r.0.0.mca", bytes);
            expect_error(dir, "LZ4", "LZ4 chunk");
            bytes[8192 + 4] = std::byte{9};
            write(dir / "region" / "r.0.0.mca", bytes);
            expect_error(dir, "compression type 9", "unknown compression");
            bytes.resize(8192 + 100);
            write(dir / "region" / "r.0.0.mca", bytes);
            expect_error(dir, "outside the file", "truncated region");
        }
        // Chunks the game never re-saved keep their old version: unsupported.
        {
            auto old = good;
            old.set("DataVersion", Tag::int_(4189));
            const auto r = anvil::import_world(hand_made_world(root / "old-chunk", {{{0, 0}, old}}), bundle());
            require(!r && r.error().find("chunk of DataVersion 4189") != std::string::npos, "old chunk");
        }
        // Random damage to a valid region file never crashes the reader.
        const auto dir = hand_made_world(root / "fuzz", {{{0, 0}, good}, {{1, 0}, chunk({1, 0}, {block("minecraft:stone")})}});
        const auto original = read(dir / "region" / "r.0.0.mca");
        circuit::gameplay::Rng rng(3);
        std::size_t failures = 0;
        for (int round = 0; round < 400; ++round) {
            auto bytes = original;
            for (int e = 0; e < 1 + static_cast<int>(rng.bounded(6)); ++e)
                bytes[rng.bounded(static_cast<std::uint32_t>(bytes.size()))] = static_cast<std::byte>(rng.bounded(256));
            write(dir / "region" / "r.0.0.mca", bytes);
            if (!anvil::import_world(dir, bundle())) ++failures;
        }
        std::cout << std::format("  {} of 400 damaged region files were refused\n", failures);
    });

    suite.run("exports never overwrite and leave no partial world", [&] {
        Engine engine(bundle(), 1);
        engine.advance({{1, AddTicket{1, {0, 0}, 1}}});
        engine.advance({{2, SetBlock{{1, 1, 1}, blocks::stone().raw(), std::nullopt}}});
        const auto state = engine.dump();
        const auto target = root / "exists";
        fs::create_directories(target);
        require(!anvil::export_world(state, bundle(), target), "existing target refused");
        require(fs::is_empty(target), "existing target untouched");
        auto pending = state;
        pending.level(circuit::contracts::DimensionId::overworld).block_events.push_back({{0, 0, 0}, BlockType::piston, 0, 0});
        require(!anvil::export_world(pending, bundle(), root / "pending"), "pending block events refused");
        require(!fs::exists(root / "pending") && !fs::exists(root / "pending.partial"), "no partial world");
    });

    suite.run("world tools import into and export from world directories", [&] {
        Engine engine(bundle(), 7);
        engine.advance({{1, AddTicket{1, {0, 0}, 1}}});
        engine.advance({{2, SetBlock{{0, -61, 0}, blocks::stone().raw(), std::nullopt}},
                        {3, SetBlock{{1, -61, 0}, blocks::stone().raw(), std::nullopt}},
                        {4, SetBlock{{0, -60, 0}, blocks::lever(true, Direction::north, blocks::LeverFace::floor).raw(), std::nullopt}},
                        {5, SetBlock{{1, -60, 0}, blocks::wire().raw(), std::nullopt}}});
        const auto source = root / "tool-source";
        require(anvil::export_world(engine.dump(), bundle(), source, {.seed = 7}).has_value(), "source world");
        circuit::server::ServerConfig config;
        config.world_directory = root / "tool-world";
        const auto imported = circuit::server::import_anvil_world(source, config);
        require(imported.has_value(), std::format("import: {}", imported ? "" : imported.error()));
        require(!circuit::server::import_anvil_world(source, config), "an existing world is not overwritten");
        const auto exported = circuit::server::export_anvil_world(config, root / "tool-export");
        require(exported.has_value(), std::format("export: {}", exported ? "" : exported.error()));
        const auto again = anvil::import_world(root / "tool-export", bundle());
        require(again.has_value(), std::format("re-import: {}", again ? "" : again.error()));
        require(state_at(again->state, {0, -60, 0}).is(BlockType::lever) && state_at(again->state, {1, -60, 0}).is(BlockType::redstone_wire),
                std::format("blocks survive both tools: {} blocks, {} {}", again->state.overworld().blocks.size(),
                            state_at(again->state, {0, -60, 0}).raw(), state_at(again->state, {1, -60, 0}).raw()));
        // A server opens the imported world.
        config.port = 0;
        config.bind_address = "127.0.0.1";
        config.online_mode = false;
        circuit::server::Server server(config);
        server.start();
        server.stop();
    });

    std::error_code ec;
    fs::remove_all(root, ec);
    return suite.finish();
}
