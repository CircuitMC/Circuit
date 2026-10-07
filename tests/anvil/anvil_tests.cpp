// SPDX-License-Identifier: AGPL-3.0-only
// Anvil import and export: round trips of every scenario's state, the
// unsupported-content policies, malformed input, and the world tools.
//
// Set CIRCUIT_ANVIL_EXPORT_DIR to keep the exported scenario worlds, for
// loading them in the game.
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
    std::vector<ChunkPos> chunks;
    std::vector<std::pair<BlockPos, std::uint32_t>> blocks;
    std::vector<std::pair<BlockPos, BlockEntity>> block_entities;
    std::multiset<std::tuple<int, BlockPos, int, Tick, int>> ticks;
    std::multiset<std::tuple<FixedVec, FixedVec, std::uint16_t, int, std::uint32_t, bool>> entities;
    bool operator==(const Kept&) const = default;
};
Kept kept(const WorldDump& d) {
    Kept k{d.time, {}, d.blocks, d.block_entities, {}, {}};
    for (const auto& c : d.chunks) k.chunks.push_back(c.pos);
    for (auto& [pos, be] : k.block_entities) {
        be.order = 0;
        be.ticked = 0;
    }
    for (const auto& t : d.ticks)
        k.ticks.insert({static_cast<int>(t.list), t.tick.pos, static_cast<int>(t.tick.block), t.tick.due, t.tick.priority});
    for (const auto& e : d.entities)
        k.entities.insert({e.position, e.velocity, e.stack.item, e.stack.count, std::min<std::uint32_t>(e.age, 6000), e.on_ground});
    return k;
}
std::string describe(const Kept& a, const Kept& b) {
    if (a.time != b.time) return "time";
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
    return {};
}
} // namespace

int main() {
    Suite suite("anvil_tests");
    const auto root = temporary_directory();
    const char* keep = std::getenv("CIRCUIT_ANVIL_EXPORT_DIR");

    suite.run("scenario states round-trip through Anvil", [&] {
        std::size_t exported = 0, with_ticks = 0, with_entities = 0, with_moving = 0;
        for (const auto& s : all_scenarios()) {
            Engine engine(bundle(), s.seed);
            Admissions admit;
            for (Tick t = 1; t <= s.ticks; ++t) {
                engine.advance(admit(s.inputs(t)));
                if (t % std::max<Tick>(1, s.ticks / 4) != 0 && t != s.ticks) continue;
                const auto state = engine.dump();
                if (!state.block_events.empty()) continue;
                const auto target = root / std::format("{}-{}", s.name, t);
                const auto report = anvil::export_world(state, bundle(), target, {.level_name = s.name, .seed = s.seed});
                require(report.has_value(), std::format("{} at {}: export: {}", s.name, t, report ? "" : report.error()));
                const auto back = anvil::import_world(target, bundle());
                require(back.has_value(), std::format("{} at {}: import: {}", s.name, t, back ? "" : back.error()));
                const auto diff = describe(kept(state), kept(back->state));
                require(diff.empty(), std::format("{} at {}: {}", s.name, t, diff));
                // The imported state is one the engine accepts and continues.
                Engine resumed(bundle(), s.seed, back->state);
                resumed.advance({});
                ++exported;
                with_ticks += state.ticks.empty() ? 0 : 1;
                with_entities += state.entities.empty() ? 0 : 1;
                with_moving += std::ranges::any_of(state.block_entities, [](const auto& b) { return b.second.kind == BlockType::moving_piston; });
                if (keep) fs::copy(target, fs::path(keep) / target.filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            }
        }
        std::cout << std::format("  {} states exported and imported ({} with scheduled ticks, {} with item entities, {} with moving pistons)\n",
                                 exported, with_ticks, with_entities, with_moving);
        require(with_ticks > 0 && with_entities > 0 && with_moving > 0, "the states cover ticks, entities and moving pistons");
    });

    suite.run("unsupported content is rejected or substituted by policy", [&] {
        const auto dir = hand_made_world(root / "unsupported",
                                         {{{0, 0}, chunk({0, 0}, {block("minecraft:air"), block("minecraft:dirt"), block("minecraft:bedrock"),
                                                                  block("minecraft:glass_pane", {{"east", "false"}, {"north", "false"}, {"south", "false"}, {"waterlogged", "false"}, {"west", "false"}}),
                                                                  block("minecraft:redstone_torch", {{"lit", "true"}}),
                                                                  block("minecraft:lever", {{"face", "floor"}, {"facing", "east"}, {"powered", "true"}})},
                                                          12)}});
        const auto rejected = anvil::import_world(dir, bundle());
        require(!rejected, "rejected by default");
        for (const auto* kind : {"block minecraft:dirt", "block minecraft:bedrock", "block minecraft:glass_pane", "block minecraft:redstone_torch"})
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

    suite.run("unsupported block entities, items and entities", [&] {
        auto c = chunk({0, 0}, {block("minecraft:air"), block("minecraft:chest", {{"facing", "north"}, {"type", "single"}, {"waterlogged", "false"}}),
                                block("minecraft:hopper", {{"enabled", "true"}, {"facing", "down"}}),
                                block("minecraft:oak_sign", {{"rotation", "0"}, {"waterlogged", "false"}})});
        // `damage` < 0: a custom name instead of a damage component.
        const auto item = [](std::int8_t slot, std::string id, std::int32_t count, std::optional<std::int32_t> damage = std::nullopt) {
            auto t = Tag::compound();
            t.set("Slot", Tag::byte(slot));
            t.set("id", Tag::string(std::move(id)));
            t.set("count", Tag::int_(count));
            if (damage) {
                auto c = Tag::compound();
                if (*damage < 0) c.set("minecraft:custom_name", Tag::string("named"));
                else c.set("minecraft:damage", Tag::int_(*damage));
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
        for (const auto* kind : {"item examplemod:gizmo", "item with components", "block entity minecraft:hopper with CustomName",
                                 "entity minecraft:pig", "block minecraft:oak_sign"})
            require(rejected.error().find(kind) != std::string::npos, std::format("names {}: {}", kind, rejected.error()));
        require(rejected.error().find("with components") == rejected.error().find("item with components") + 5, "empty component maps are accepted");
        const auto r = anvil::import_world(dir, bundle(), {.unsupported = anvil::Unsupported::substitute});
        require(r.has_value(), "substituted");
        const auto* c0 = block_entity_at(r->state, {1, 0, 0});
        require(c0 && c0->slots[0] == ItemStack{items::redstone, 64} && c0->slots[5].empty() && c0->slots[6].empty(), "supported items kept");
        require(c0->slots[7] == ItemStack{items::iron_pickaxe, 1, 12} && c0->slots[8].empty() && c0->slots[9].empty(), "damage kept when valid");
        // A damaged tool exports with its damage component and comes back.
        {
            auto state = r->state;
            const auto out = root / "damaged-export";
            require(anvil::export_world(state, bundle(), out, {.level_name = "damaged", .seed = 1}).has_value(), "exported");
            const auto again = anvil::import_world(out, bundle());
            require(again && block_entity_at(again->state, {1, 0, 0})->slots[7] == ItemStack{items::iron_pickaxe, 1, 12}, "damage round-trips");
        }
        const auto* h = block_entity_at(r->state, {2, 0, 0});
        require(h && h->cooldown == 3, "hopper cooldown");
        require(r->state.entities.size() == 1 && r->state.entities[0].stack == ItemStack{items::wheat, 3} &&
                    r->state.entities[0].position == FixedVec{4 * 4096 + 2048, 4096 + 1024, 4 * 4096 + 2048} &&
                    r->state.entities[0].age == 20,
                "item entity");
        require(r->state.block_entities.size() == 2, "the sign's block entity is dropped");
    });

    suite.run("malformed worlds fail cleanly", [&] {
        const auto good = chunk({0, 0}, {block("minecraft:air"), block("minecraft:stone")});
        const auto expect_error = [&](const fs::path& dir, std::string_view what, std::string_view label) {
            const auto r = anvil::import_world(dir, bundle());
            require(!r && r.error().find(what) != std::string::npos, std::format("{}: {}", label, r ? "imported" : r.error()));
        };
        expect_error(hand_made_world(root / "old", {{{0, 0}, good}}, level_dat(4189)), "DataVersion 4189", "older world");
        expect_error(root / "missing", "not a world directory", "missing directory");
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
        pending.block_events.push_back({{0, 0, 0}, BlockType::piston, 0, 0});
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
                std::format("blocks survive both tools: {} blocks, {} {}", again->state.blocks.size(),
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
