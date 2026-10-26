// SPDX-License-Identifier: AGPL-3.0-only
// Durable storage: checkpoint encoding, the world directory, and recovery
// after a crash or an I/O error at every file operation.
#include "../sim/scenarios.hpp"
#include "../support/check.hpp"
#include "memory_fs.hpp"
#include <cmath>
#include <limits>
#include <circuit/gameplay/level_random.hpp>
#include <circuit/gameplay/rng.hpp>
#include <circuit/protocol/framing.hpp>
#include <circuit/runtime/engine.hpp>
#include <circuit/storage/world_directory.hpp>
#include <climits>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

using namespace circuit::test;
using circuit::runtime::Engine;
using circuit::storage::Checkpoint;
using circuit::storage::Recovery;
using circuit::storage::WorldDirectory;

namespace {
const VersionBundle& bundle() { return **BundleRegistry::select_for_simulation("java-1.21.8"); }
const std::string bundle_id = "java-1.21.8";
std::string rules_id() { return std::string(bundle().rules->id); }
// The rules of the release before typed tickets.
constexpr std::string_view previous_rules = "circuit-reference-rules-v7";

// A scenario's inputs and the engine state after every tick.
struct History {
    std::string name;
    std::uint64_t seed{};
    std::vector<std::vector<Command>> inputs; // index t-1
    std::vector<WorldDump> dumps;             // index t
    [[nodiscard]] Tick ticks() const { return inputs.size(); }
};
History record(const Scenario& s, Tick limit) {
    History h{s.name, s.seed, {}, {}};
    Engine engine(bundle(), s.seed);
    Admissions admit;
    h.dumps.push_back(engine.dump());
    for (Tick t = 1; t <= std::min(limit, s.ticks); ++t) {
        h.inputs.push_back(admitted(admit, s, t));
        engine.advance(h.inputs.back());
        h.dumps.push_back(engine.dump());
    }
    return h;
}

Checkpoint checkpoint_at(const History& h, Tick t) {
    Checkpoint c{bundle_id, rules_id(), h.seed, h.dumps[t], {}};
    const auto label = std::format("players at {}", t);
    c.extras["players"] = std::vector<std::byte>(std::as_bytes(std::span(label)).begin(), std::as_bytes(std::span(label)).end());
    return c;
}
TickInput input_at(const History& h, Tick t) { return {t, h.inputs[t - 1]}; }

// Whether a checkpoint written by the previous release (`legacy`) reads as
// the state the current rules reach (`current`): the same blocks, entities
// and pending work; the same tickets, now forced ones of level 31; its
// ticking chunks at levels 31 and its border chunks at 33, not simulated.
bool legacy_reads_as(const Checkpoint& legacy, const Checkpoint& current) {
    if (legacy.bundle_id != current.bundle_id || legacy.rules_id != previous_rules || legacy.seed != current.seed
        || legacy.extras != current.extras)
        return false;
    const auto& a = legacy.state;
    const auto& b = current.state;
    if (a.time != b.time || a.next_entity_id != b.next_entity_id || a.started != 0 || a.tickets != b.tickets || a.players != b.players)
        return false;
    const auto& la = a.overworld();
    const auto& lb = b.overworld();
    // Entities: the same ones; their motion followed the rules of the release
    // that wrote the world (1/4096 block units, read exactly), and so did
    // their ages, which count the ticks they spent in entity-ticking chunks
    // (pistons now push items across a chunk edge a tick later).
    const auto same_entity = [](const circuit::gameplay::Entity& x, const circuit::gameplay::Entity& y) {
        const auto whole = [](double v) { return v * 4096.0 == std::floor(v * 4096.0); };
        return x.id == y.id && x.kind == y.kind && x.stack == y.stack && std::abs(x.age - y.age) <= 1 && x.pickup_delay == y.pickup_delay
               && whole(x.position.x) && whole(x.position.y) && whole(x.position.z) && whole(x.velocity.x) && whole(x.velocity.y);
    };
    if (la.entities.size() != lb.entities.size() || !std::ranges::equal(la.entities, lb.entities, same_entity)) return false;
    if (la.blocks != lb.blocks || la.block_entities != lb.block_entities || la.ticks != lb.ticks
        || la.block_events != lb.block_events || la.next_suborder != lb.next_suborder
        || la.next_block_entity_order != lb.next_block_entity_order || !la.presences.empty() || la.last_active != 0)
        return false;
    // No level random was saved: it starts as in a new world.
    if (la.random != circuit::gameplay::initial_level_random(legacy.seed, la.dimension, {}).state()) return false;
    std::map<ChunkPos, std::uint32_t> ticking_a, ticking_b;
    for (const auto& c : la.chunks) {
        if (c.load == 31 && c.simulation == 31 && c.level == ChunkLevel::entity_ticking) ticking_a[c.pos] = c.generation;
        else if (c.load != 33 || c.simulation != 34 || c.level != ChunkLevel::border) return false;
    }
    for (const auto& c : lb.chunks)
        if (c.load <= 31) ticking_b[c.pos] = c.generation;
    return ticking_a == ticking_b;
}

// Checks that a recovery is a prefix of `h` and replays it on an engine.
// Returns the tick it reaches.
Tick check_recovery(const History& h, const Recovery& r, std::string_view label) {
    const auto base = r.checkpoint.state.time;
    require(base <= h.ticks() && r.checkpoint == checkpoint_at(h, base), std::format("{}: checkpoint at {} is from the history", label, base));
    for (std::size_t i = 0; i < r.inputs.size(); ++i)
        require(base + 1 + i <= h.ticks() && r.inputs[i] == input_at(h, base + 1 + i), std::format("{}: input {} after {}", label, i, base));
    const auto end = base + r.inputs.size();
    Engine engine(bundle(), r.checkpoint.seed, r.checkpoint.state);
    for (const auto& in : r.inputs) engine.advance(in.commands);
    require(engine.dump() == h.dumps[end], std::format("{}: replayed state at {}", label, end));
    return end;
}

struct Policy {
    Tick sync_every{3};
    Tick checkpoint_every{7};
    // Finish each checkpoint this many ticks after beginning it, logging in
    // between, as a background writer does; 0 checkpoints at once.
    Tick finish_after{0};
};
struct Outcome {
    Tick durable{};          // every tick up to here was confirmed durable
    Tick attempted{};        // the last tick written in any form
    bool saved{false};       // a checkpoint call succeeded
};
// The server's use of a world directory: an initial checkpoint, one record
// per tick, periodic checkpoints. Stops at the first failure, as the server
// fails closed.
Outcome run(circuit::storage::Filesystem& fs, const History& h, Policy p, Tick from = 0) {
    WorldDirectory dir(fs, "/world", bundle_id, rules_id());
    Outcome o;
    if (from > 0) {
        const auto r = dir.recover();
        require(r && *r, "the world recovers before continuing");
        o.saved = true;
        o.durable = o.attempted = from;
    }
    if (!dir.checkpoint(checkpoint_at(h, from))) return o;
    o.saved = true;
    o.durable = from;
    std::optional<circuit::storage::PendingCheckpoint> pending;
    for (Tick t = from + 1; t <= h.ticks(); ++t) {
        const bool sync = t % p.sync_every == 0;
        o.attempted = t;
        if (!dir.append(input_at(h, t), sync)) return o;
        if (sync) o.durable = t;
        if (pending && t >= pending->tick + p.finish_after) {
            if (!dir.finish_checkpoint(*pending, circuit::storage::encode_checkpoint(checkpoint_at(h, pending->tick)))) return o;
            o.durable = std::max(o.durable, pending->tick);
            pending.reset();
        }
        if (t % p.checkpoint_every == 0 && !pending) {
            if (p.finish_after == 0) {
                if (!dir.checkpoint(checkpoint_at(h, t))) return o;
                o.durable = t;
            } else {
                auto begun = dir.begin_checkpoint(t);
                if (!begun) return o;
                pending = std::move(*begun);
            }
        }
    }
    if (pending && !dir.finish_checkpoint(*pending, circuit::storage::encode_checkpoint(checkpoint_at(h, pending->tick)))) return o;
    return o;
}

// Recovery by a new owner; nothing else may own the directory.
std::expected<std::optional<Recovery>, std::string> recover(MemoryFilesystem& fs) {
    WorldDirectory dir(fs, "/world", bundle_id, rules_id());
    return dir.recover();
}
// Recovery after the current owner's process lost power: from the durable
// image, while the owner object in this test still exists.
std::expected<std::optional<Recovery>, std::string> recover_image(const MemoryFilesystem& fs) {
    MemoryFilesystem image = fs.crashed();
    return recover(image);
}
// Every file and its current content.
std::map<std::string, std::vector<std::byte>> contents(const circuit::storage::Filesystem& fs, const circuit::storage::Path& dir) {
    std::map<std::string, std::vector<std::byte>> out;
    const auto names = fs.list(dir).value();
    for (const auto& n : names) out[n] = fs.read_file(dir / n).value();
    return out;
}

// After a crash or failure: recovery must succeed and hold every durable
// tick. With `continue_run`, the world then continues to the end of the
// history and recovers again.
void check_after(MemoryFilesystem fs, const History& h, const Outcome& o, std::string_view label, bool continue_run = true) {
    const auto r = recover(fs);
    require(r.has_value(), std::format("{}: recovery fails: {}", label, r ? "" : r.error()));
    if (!*r) {
        require(!o.saved, std::format("{}: a saved world was lost", label));
        return;
    }
    const auto end = check_recovery(h, **r, label);
    require(end >= o.durable && end <= o.attempted, std::format("{}: recovered tick {} outside [{}, {}]", label, end, o.durable, o.attempted));
    if (!continue_run) return;
    // Continue the history from the recovered tick, then recover again.
    const auto done = run(fs, h, {}, end);
    require(done.attempted == h.ticks(), std::format("{}: continuation", label));
    const auto again = recover(fs);
    require(again && *again, std::format("{}: second recovery: {}", label, again ? "nothing saved" : again.error()));
    const auto reached = check_recovery(h, **again, label);
    std::string notes;
    for (const auto& n : (*again)->notes) notes += n + "; ";
    require(reached == h.ticks(), std::format("{}: second recovery reaches {} from {}: {}", label, reached, (*again)->checkpoint.state.time, notes));
}

std::vector<std::byte> read_fixture(std::string_view name) {
    std::ifstream in(std::filesystem::path(CIRCUIT_SOURCE_DIR) / "tests/fixtures/storage" / name, std::ios::binary);
    const std::vector<char> raw{std::istreambuf_iterator<char>(in), {}};
    require(!raw.empty(), std::format("fixture {}", name));
    std::vector<std::byte> out(raw.size());
    std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

std::vector<std::byte> bytes(std::string_view s) {
    const auto b = std::as_bytes(std::span(s));
    return {b.begin(), b.end()};
}
} // namespace

int main() {
    Suite suite("storage_tests");
    std::vector<History> histories;
    for (const auto& s : {clock_scenario(), comparator_chest_scenario(), drops_scenario(), piston_chain_scenario()})
        histories.push_back(record(s, 40));

    suite.run("checkpoints round-trip scenario states", [&] {
        bool rules = false, weather = false;
        for (const auto& s : all_scenarios()) {
            const auto h = record(s, s.ticks);
            for (const Tick t : {Tick{0}, h.ticks() / 2, h.ticks()}) {
                const auto c = checkpoint_at(h, t);
                rules |= !c.state.game_rules.empty();
                weather |= c.state.weather.thunder_level > 0.0f && c.state.weather.thunder_level < 1.0f;
                const auto encoded = circuit::storage::encode_checkpoint(c);
                const auto decoded = circuit::storage::decode_checkpoint(encoded);
                require(decoded.has_value(), std::format("{} at {}: {}", s.name, t, decoded ? "" : decoded.error()));
                require(*decoded == c, std::format("{} at {}: decoded checkpoint differs", s.name, t));
                require(circuit::storage::encode_checkpoint(*decoded) == encoded, std::format("{} at {}: encoding is canonical", s.name, t));
            }
        }
        require(rules && weather, "a scenario sets a game rule and changes the weather");
    });

    suite.run("checkpoints keep random sequences, their settings and the difficulty", [&] {
        // A loot table's sequence as the official 1.21.8 server saved it (the
        // loot_many skeleton's grass), one more, and settings that are not the
        // defaults; ids are kept in ascending order.
        auto c = checkpoint_at(histories[1], histories[1].ticks());
        c.state.sequence_settings = {-7, false, true};
        c.state.difficulty = 3;
        c.state.level(circuit::contracts::DimensionId::overworld).random_tick = 0xDEADBEEF;
        // A furnace's times.
        {
            circuit::gameplay::BlockEntity furnace{circuit::gameplay::BlockType::furnace, 99, std::vector<circuit::contracts::ItemStack>(3), 0};
            furnace.lit_time = 1234;
            furnace.lit_total = 1600;
            furnace.cook_time = 77;
            furnace.cook_total = 200;
            furnace.recipes_used = {{"minecraft:iron_ingot", 7}, {"minecraft:stone", 2}};
            auto& bes = c.state.level(circuit::contracts::DimensionId::overworld).block_entities;
            bes.push_back({{29999, 0, 29999}, furnace});
            circuit::gameplay::BlockEntity crafter{circuit::gameplay::BlockType::crafter, 98, std::vector<circuit::contracts::ItemStack>(9), 0};
            crafter.disabled = 0x105;
            crafter.crafting_ticks = 4;
            bes.push_back({{29998, 0, 29999}, crafter});
            std::ranges::sort(bes, {}, &std::pair<circuit::contracts::BlockPos, circuit::gameplay::BlockEntity>::first);
        }
        // A player's open crafting table, stonecutter and smithing table.
        {
            circuit::gameplay::PlayerInventory p;
            p.player = {0, 77};
            p.table[0] = {circuit::gameplay::items::oak_planks, 4};
            p.table[5] = {circuit::gameplay::items::oak_log, 2};
            p.cutter[0] = {circuit::gameplay::items::stone, 9};
            p.cutter_recipe = 3;
            p.smith[1] = {circuit::gameplay::items::diamond_pickaxe, 1, 17};
            p.grind[0] = {circuit::gameplay::items::iron_pickaxe, 1, 30};
            p.loom[1] = {circuit::gameplay::items::stone, 3};
            p.loom_pattern = 5;
            p.xp_level = 9;
            p.xp_progress = 0.6000000834465027f;
            p.xp_total = 150;
            p.xp_ready = 12;
            c.state.players.insert(std::ranges::upper_bound(c.state.players, p.player, {}, &circuit::gameplay::PlayerInventory::player), p);
        }
        c.state.level(circuit::contracts::DimensionId::overworld).sequences = {
            {"circuit:fresh", circuit::gameplay::RandomSequence{1, 2}},
            {"minecraft:blocks/short_grass", circuit::gameplay::RandomSequence{2022789406501655404ULL, 17977990001259141514ULL}}};
        const auto encoded = circuit::storage::encode_checkpoint(c);
        const auto decoded = circuit::storage::decode_checkpoint(encoded);
        require(decoded.has_value(), decoded ? "" : decoded.error());
        require(*decoded == c && circuit::storage::encode_checkpoint(*decoded) == encoded, "the sequences, settings, furnace and crafter block entities, player menus and experience round-trip");
        auto swapped = c;
        auto& sequences = swapped.state.level(circuit::contracts::DimensionId::overworld).sequences;
        std::swap(sequences[0], sequences[1]);
        require(!circuit::storage::decode_checkpoint(circuit::storage::encode_checkpoint(swapped)), "ids out of order are rejected");
        auto beyond = c;
        beyond.state.difficulty = 4;
        require(!circuit::storage::decode_checkpoint(circuit::storage::encode_checkpoint(beyond)), "a difficulty above hard is rejected");
    });

    suite.run("snapshots encode like dumps and stay fixed while the engine runs", [&] {
        circuit::storage::SectionCache cache;
        for (const auto& s : all_scenarios()) {
            Engine engine(bundle(), s.seed);
            Admissions admit;
            for (Tick t = 1; t <= s.ticks; ++t) {
                engine.advance(admitted(admit, s, t));
                if (t % std::max<Tick>(1, s.ticks / 3) != 0) continue;
                const auto expected = circuit::storage::encode_checkpoint({bundle_id, rules_id(), s.seed, engine.dump(), {}});
                auto snap = engine.snapshot();
                // The engine keeps running; copy-on-write keeps the snapshot as it was.
                for (Tick k = 1; k <= 5 && t + k <= s.ticks; ++k) engine.advance(admit(s.inputs(t + k)));
                Checkpoint c{bundle_id, rules_id(), s.seed, std::move(snap.state), {}};
                const auto plain = circuit::storage::encode_checkpoint(c, snap.sections, bundle(), nullptr);
                const auto cached = circuit::storage::encode_checkpoint(c, snap.sections, bundle(), &cache);
                const auto again = circuit::storage::encode_checkpoint(c, snap.sections, bundle(), &cache);
                require(plain == expected && cached == expected && again == expected, std::format("{} at {}: snapshot encoding", s.name, t));
                t = std::min(s.ticks, t + 5); // those ticks already ran
            }
        }
        require(cache.size() > 0, "the cache keeps the last world's sections");
    });

    suite.run("damaged checkpoints are rejected", [&] {
        const auto c = checkpoint_at(histories[1], histories[1].ticks());
        const auto good = circuit::storage::encode_checkpoint(c);
        for (std::size_t i = 0; i < good.size(); ++i) {
            auto bad = good;
            bad[i] ^= std::byte{0x10};
            require(!circuit::storage::decode_checkpoint(bad), std::format("flipped byte {}", i));
        }
        for (std::size_t n = 0; n < good.size(); ++n)
            require(!circuit::storage::decode_checkpoint(std::span(good).first(n)), std::format("truncated to {}", n));
        auto longer = good;
        longer.push_back(std::byte{0});
        require(!circuit::storage::decode_checkpoint(longer), "trailing byte");
        require(!circuit::storage::decode_checkpoint(good, {.max_bytes = good.size() - 1}), "size limit");
        require(!circuit::storage::decode_checkpoint(good, {.max_raw_bytes = 16}), "raw size limit");
    });

    suite.run("checkpoints name their block registry", [&] {
        // A version 6 checkpoint written by the previous release (the clock
        // history's last state; tests/fixtures/storage) reads as the
        // overworld's. Version 5 named the registry the same way, and version
        // 4 predates it and is read as circuit-blocks-1 (the scenario holds no
        // item stacks, whose encoding changed in version 6). Another registry
        // is refused.
        const auto c = checkpoint_at(histories[0], histories[0].ticks());
        const auto good = read_fixture("checkpoint-v6-clock.bin");
        const auto legacy = circuit::storage::decode_checkpoint(good).value();
        require(legacy_reads_as(legacy, c), "a version 6 checkpoint reads as the overworld's");
        const std::span<const std::byte> sealed(good.data(), good.size() - 32);
        circuit::protocol::Reader header(sealed);
        (void)header.raw(12);
        (void)header.string(64);
        (void)header.string(128);
        const auto registry_at = sealed.size() - header.remaining();
        require(header.string(64).value() == "circuit-blocks-1", "the registry is named");
        const auto rest_at = sealed.size() - header.remaining();
        const auto reseal = [&](std::int32_t version, std::optional<std::string_view> registry) {
            circuit::protocol::Writer w;
            w.raw(sealed.first(8)).i32(version).raw(sealed.subspan(12, registry_at - 12));
            if (registry) w.string(*registry, 64);
            w.raw(sealed.subspan(rest_at));
            auto out = w.take();
            const auto hex = circuit::storage::sha256_hex(out);
            for (std::size_t i = 0; i < 32; ++i) out.push_back(static_cast<std::byte>(std::stoi(hex.substr(2 * i, 2), nullptr, 16)));
            return out;
        };
        require(circuit::storage::decode_checkpoint(reseal(5, "circuit-blocks-1")).value() == legacy, "version 5");
        require(circuit::storage::decode_checkpoint(reseal(4, std::nullopt)).value() == legacy, "version 4 reads as circuit-blocks-1");
        const auto other = circuit::storage::decode_checkpoint(reseal(6, "circuit-blocks-0"));
        require(!other && other.error().find("block registry circuit-blocks-0") != std::string::npos, "another registry is refused");
        // The current version writes the levels and reads back the same state.
        require(circuit::storage::decode_checkpoint(circuit::storage::encode_checkpoint(c)).value() == c, "current version round trip");
        // An item that never ages, with a large tick count.
        auto never = c;
        circuit::gameplay::Entity item;
        item.id = circuit::contracts::EntityId{never.state.next_entity_id++};
        item.position = {0.5, 1.0, 0.5};
        item.stack = {circuit::gameplay::items::stone, 1};
        item.age = circuit::gameplay::item_age_never;
        item.tick_count = 4000000000U;
        item.health = 3; // hurt by an explosion
        never.state.level(circuit::contracts::DimensionId::overworld).entities.push_back(item);
        // A falling block, with its block and fall time.
        circuit::gameplay::Entity falling;
        falling.id = circuit::contracts::EntityId{never.state.next_entity_id++};
        falling.kind = circuit::gameplay::EntityKind::falling_block;
        falling.position = {2.5, 4.0, 2.5};
        falling.velocity = {0.0, -0.5, 0.0};
        falling.carried = registry_block("red_sand");
        falling.fall_time = 42;
        never.state.level(circuit::contracts::DimensionId::overworld).entities.push_back(falling);
        // Primed TNT, with its fuse.
        circuit::gameplay::Entity primed;
        primed.id = circuit::contracts::EntityId{never.state.next_entity_id++};
        primed.kind = circuit::gameplay::EntityKind::primed_tnt;
        primed.position = {4.5, 1.0, 4.5};
        primed.velocity = {0.01, 0.2, -0.01};
        primed.fuse = 37;
        never.state.level(circuit::contracts::DimensionId::overworld).entities.push_back(primed);
        // A burning arrow stuck in the ground.
        circuit::gameplay::Entity arrow;
        arrow.id = circuit::contracts::EntityId{never.state.next_entity_id++};
        arrow.kind = circuit::gameplay::EntityKind::arrow;
        arrow.position = {6.5, 1.05, 6.5};
        arrow.in_ground = true;
        arrow.life = 345;
        arrow.fire = 12;
        never.state.level(circuit::contracts::DimensionId::overworld).entities.push_back(arrow);
        // A boat afloat after a fall, and a minecart in water.
        circuit::gameplay::Entity boat;
        boat.id = circuit::contracts::EntityId{never.state.next_entity_id++};
        boat.kind = circuit::gameplay::EntityKind::boat;
        boat.position = {8.5, 1.5385, 8.5};
        boat.variant = 9;
        boat.status = 1;
        boat.last_dy = -0.0012501467973757997;
        never.state.level(circuit::contracts::DimensionId::overworld).entities.push_back(boat);
        circuit::gameplay::Entity cart;
        cart.id = circuit::contracts::EntityId{never.state.next_entity_id++};
        cart.kind = circuit::gameplay::EntityKind::minecart;
        cart.position = {10.5, 1.0, 8.5};
        cart.in_water = true;
        never.state.level(circuit::contracts::DimensionId::overworld).entities.push_back(cart);
        // An experience orb that took in two others.
        circuit::gameplay::Entity orb;
        orb.id = circuit::contracts::EntityId{never.state.next_entity_id++};
        orb.kind = circuit::gameplay::EntityKind::experience_orb;
        orb.position = {12.5, 1.0, 8.5};
        orb.experience = 37;
        orb.orbs = 3;
        orb.age = 120;
        orb.seeded = true;
        orb.own_random = 0x123456789ABCULL;
        never.state.level(circuit::contracts::DimensionId::overworld).entities.push_back(orb);
        // And one waiting to come back with its reloaded chunk.
        auto returning = item;
        returning.id = circuit::contracts::EntityId{never.state.next_entity_id++};
        returning.returns = never.state.time + 1;
        never.state.level(circuit::contracts::DimensionId::overworld).entities.push_back(returning);
        require(circuit::storage::decode_checkpoint(circuit::storage::encode_checkpoint(never)).value() == never,
                "ages, tick counts, falling blocks and fuses round trip");
        require(circuit::storage::decode_checkpoint(circuit::storage::encode_checkpoint(legacy)).value() == legacy,
                "a converted checkpoint round trip");
    });

    suite.run("a world written by the previous release keeps its rules", [&] {
        // The drops history's world directory as the previous release wrote it
        // under circuit-reference-rules-v7: version 6 checkpoints at ticks 0
        // and 25, and format 1 log segments whose commands name no level.
        // Under other rules it is refused (the server migrates it); under its
        // own, the old formats are read and new records use the current ones.
        MemoryFilesystem fs;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(CIRCUIT_SOURCE_DIR) / "tests/fixtures/storage")) {
            const auto name = entry.path().filename().string();
            if (!name.starts_with("world-")) continue;
            require(fs.write_file("/world/" + name.substr(6), read_fixture(name)).has_value(), "fixture copied");
        }
        const auto refused = recover(fs);
        require(!refused && refused.error().find(std::string(previous_rules)) != std::string::npos,
                refused ? "recovered under other rules" : refused.error());
        const auto own = [&] {
            WorldDirectory dir(fs, "/world", bundle_id, std::string(previous_rules));
            return dir.recover();
        };
        const auto r = own();
        require(r && *r, r ? "no world" : r.error());
        const auto& h = histories[2];
        const auto base = (*r)->checkpoint.state.time;
        require(base == 25 && legacy_reads_as((*r)->checkpoint, checkpoint_at(h, base)), "the last checkpoint");
        require(base + (*r)->inputs.size() == h.ticks(), "the whole log is read");
        for (std::size_t i = 0; i < (*r)->inputs.size(); ++i)
            require((*r)->inputs[i] == input_at(h, base + 1 + i), std::format("input {} in the overworld, tickets forced", i));
        // The next checkpoint and log records use the current formats.
        auto next = (*r)->checkpoint;
        next.state.time = h.ticks();
        {
            WorldDirectory dir(fs, "/world", bundle_id, std::string(previous_rules));
            require(dir.recover().has_value() && dir.checkpoint(next).has_value() && dir.append({h.ticks() + 1, input_at(h, 1).commands}, true).has_value(),
                    "a new checkpoint and record");
        }
        const auto again = own();
        require(again && *again && (*again)->checkpoint == next && (*again)->inputs.size() == 1
                    && (*again)->inputs[0].commands == input_at(h, 1).commands,
                "recovers again");
    });

    suite.run("bodies with valid digests are decoded safely", [&] {
        // Re-seals mutated bodies so that only the body decoder and the engine's
        // restore checks stand between the data and the engine.
        const auto c = checkpoint_at(histories[2], histories[2].ticks());
        const auto good = circuit::storage::encode_checkpoint(c);
        const std::span<const std::byte> sealed(good.data(), good.size() - 32);
        circuit::protocol::Reader header(sealed);
        (void)header.raw(12);
        (void)header.string(64);
        (void)header.string(128);
        (void)header.string(64);
        (void)header.i64();
        (void)header.i64();
        const auto raw = static_cast<std::size_t>(header.i64().value());
        (void)header.i64();
        const auto prefix = good.size() - 32 - header.remaining();
        const auto body = circuit::protocol::inflate_exact(std::span(good).subspan(prefix, header.remaining()), raw).value();
        const auto seal = [&](const std::vector<std::byte>& b) {
            circuit::protocol::Writer w;
            w.raw(std::span(good).first(prefix - 16)).i64(static_cast<std::int64_t>(b.size()));
            const auto z = circuit::protocol::deflate_bytes(b);
            w.i64(static_cast<std::int64_t>(z.size())).raw(z);
            auto out = w.take();
            const auto hex = circuit::storage::sha256_hex(out);
            for (std::size_t i = 0; i < 32; ++i) out.push_back(static_cast<std::byte>(std::stoi(hex.substr(2 * i, 2), nullptr, 16)));
            return out;
        };
        require(circuit::storage::decode_checkpoint(seal(body)).value() == c, "resealed body decodes");
        circuit::gameplay::Rng rng(7);
        std::size_t decoded = 0, restored = 0;
        for (int round = 0; round < 3000; ++round) {
            auto b = body;
            const auto below = [&](std::size_t n) { return static_cast<std::size_t>(rng.bounded(static_cast<std::uint32_t>(n))); };
            const auto edits = 1 + below(4);
            for (std::size_t e = 0; e < edits && !b.empty(); ++e) {
                switch (below(3)) {
                case 0: b[below(b.size())] = static_cast<std::byte>(below(256)); break;
                case 1: b.resize(b.size() - below(std::min<std::size_t>(b.size(), 64) + 1)); break;
                default: b.insert(b.begin() + static_cast<std::ptrdiff_t>(below(b.size() + 1)), static_cast<std::byte>(below(256)));
                }
            }
            const auto d = circuit::storage::decode_checkpoint(seal(b));
            if (!d) continue;
            ++decoded;
            try {
                Engine engine(bundle(), d->seed, d->state);
                engine.advance({});
                ++restored;
            } catch (const std::invalid_argument&) {
            }
        }
        std::cout << std::format("  mutated bodies: {} decoded, {} restored and ticked\n", decoded, restored);
    });

    suite.run("restored entities stay within the bounds the rules keep", [&] {
        // A checkpoint with a valid digest can still hold values the rules never
        // produce; restoring must refuse them instead of overflowing later.
        const auto& h = histories[2]; // drops: item entities
        Tick t = 0;
        while (t < h.ticks() && h.dumps[t].overworld().entities.empty()) ++t;
        require(!h.dumps[t].overworld().entities.empty(), "the history has item entities");
        const auto refused = [&](auto edit, std::string_view what) {
            auto state = h.dumps[t];
            edit(state.level(circuit::contracts::DimensionId::overworld).entities.front());
            try {
                Engine engine(bundle(), h.seed, state);
            } catch (const std::invalid_argument& e) {
                require(std::string_view(e.what()).find(what) != std::string_view::npos, std::format("refused for {}: {}", what, e.what()));
                return;
            }
            throw std::runtime_error(std::format("a restored entity with a bad {} was accepted", what));
        };
        refused([](circuit::gameplay::Entity& e) { e.velocity.x = -1e9; }, "velocity");
        refused([](circuit::gameplay::Entity& e) { e.velocity.y = std::nan(""); }, "velocity");
        refused([](circuit::gameplay::Entity& e) { e.position.y = 1e18; }, "height");
        refused([](circuit::gameplay::Entity& e) { e.kind = static_cast<circuit::gameplay::EntityKind>(0); }, "kind");
        refused([](circuit::gameplay::Entity& e) { e.stack.count = 0; }, "stack");
        refused([](circuit::gameplay::Entity& e) { e.stack.item = 9999; }, "stack");
        Engine fine(bundle(), h.seed, h.dumps[t]);
        fine.advance({});
    });

    suite.run("world directory saves, logs and recovers", [&] {
        const auto& h = histories[0];
        MemoryFilesystem fs;
        WorldDirectory dir(fs, "/world", bundle_id, rules_id());
        const auto none = dir.recover();
        require(none && !*none, "a new world has nothing to recover");
        require(!dir.append(input_at(h, 1), true), "no log before the first checkpoint");
        require(dir.checkpoint(checkpoint_at(h, 0)).has_value(), "first checkpoint");
        require(!dir.append(input_at(h, 2), true), "gap refused");
        for (Tick t = 1; t <= 10; ++t) require(dir.append(input_at(h, t), t % 2 == 0).has_value(), "append");
        require(!dir.checkpoint(checkpoint_at(h, 9)), "checkpoint behind the log refused");
        Checkpoint other = checkpoint_at(h, 10);
        other.rules_id = "other-rules";
        require(!dir.checkpoint(other), "checkpoint of other rules refused");
        const auto r = recover_image(fs);
        require(r && *r && (*r)->checkpoint.state.time == 0 && (*r)->inputs.size() == 10 && (*r)->notes.empty(), "log replayed");
        require(check_recovery(h, **r, "log") == 10, "replayed to 10");
        require(dir.checkpoint(checkpoint_at(h, 10)).has_value(), "second checkpoint");
        for (Tick t = 11; t <= 15; ++t) require(dir.append(input_at(h, t), true).has_value(), "append");
        require(dir.checkpoint(checkpoint_at(h, 15)).has_value(), "third checkpoint");
        require(dir.checkpoint(checkpoint_at(h, 15)).has_value(), "checkpoint at the same tick");
        const auto names = fs.list("/world").value();
        require(names.size() == 5, std::format("manifest, two checkpoints and two segments remain, not {}", names.size()));
        const auto again = recover_image(fs);
        require(again && *again && check_recovery(h, **again, "again") == 15, "recovered at 15");
        for (const auto& [bundle_name, rules] : {std::pair<std::string, std::string>{"java-1.21.4", rules_id()}, {bundle_id, "other"}}) {
            MemoryFilesystem image = fs.crashed();
            WorldDirectory foreign(image, "/world", bundle_name, rules);
            const auto refused = foreign.recover();
            require(!refused && refused.error().find("in use") == std::string::npos,
                    std::format("a world of {} / {} is refused", bundle_name, rules));
        }
    });

    suite.run("damaged current checkpoints fall back to the previous one", [&] {
        const auto& h = histories[1];
        MemoryFilesystem fs;
        const auto o = run(fs, h, {.sync_every = 1, .checkpoint_every = 10});
        require(o.durable == h.ticks(), "complete run");
        const auto corrupt_current = [&] {
            const auto names = fs.list("/world").value();
            std::string newest;
            for (const auto& n : names)
                if (n.starts_with("ckpt-") && n > newest) newest = n;
            (*fs.bytes("/world/" + newest))[40] ^= std::byte{1};
            return newest;
        };
        const auto damaged = corrupt_current();
        {
            WorldDirectory dir(fs, "/world", bundle_id, rules_id());
            const auto r = dir.recover();
            require(r && *r && (*r)->notes.size() == 2, "fallback noted");
            require(check_recovery(h, **r, "fallback") == h.ticks(), "fallback replays to the end");
            // The next checkpoint keeps the readable one as its fallback.
            require(dir.checkpoint(checkpoint_at(h, h.ticks())).has_value(), "checkpoint after fallback");
            require(!fs.exists("/world/" + damaged) && fs.exists("/world/damaged-" + damaged), "damaged checkpoint kept aside");
        }
        corrupt_current();
        const auto second = recover(fs);
        require(second && *second && check_recovery(h, **second, "second fallback") == h.ticks(), "second fallback");
        // Both checkpoints damaged: recovery refuses instead of starting empty.
        const auto names = fs.list("/world").value();
        for (const auto& n : names)
            if (n.starts_with("ckpt-")) (*fs.bytes("/world/" + n))[40] ^= std::byte{2};
        const auto none = recover(fs);
        require(!none && none.error().find("no usable checkpoint") != std::string::npos, "no usable checkpoint is an error");
    });

    suite.run("torn and damaged log tails", [&] {
        const auto& h = histories[2];
        MemoryFilesystem base;
        const auto o = run(base, h, {.sync_every = 1, .checkpoint_every = 1000});
        require(o.durable == h.ticks(), "complete run");
        std::string segment;
        const auto names = base.list("/world").value();
        for (const auto& n : names)
            if (n.starts_with("wal-")) segment = "/world/" + n;
        const auto full = base.read_file(segment).value();
        // Record boundaries, from the record lengths.
        std::vector<std::size_t> ends{16};
        while (ends.back() < full.size()) {
            std::uint32_t length = 0;
            for (int i = 0; i < 4; ++i) length |= std::to_integer<std::uint32_t>(full[ends.back() + static_cast<std::size_t>(i)]) << (8 * i);
            ends.push_back(ends.back() + 8 + length);
        }
        require(ends.size() == h.ticks() + 1 && ends.back() == full.size(), "one record per tick");
        for (std::size_t n = 0; n <= full.size(); ++n) {
            MemoryFilesystem fs = base.crashed();
            fs.bytes(segment)->resize(n);
            const auto r = recover(fs);
            const auto complete = static_cast<std::size_t>(std::ranges::upper_bound(ends, n) - ends.begin()) - 1;
            if (n < 16) {
                require(r && *r && (*r)->inputs.empty() && !(*r)->notes.empty(), std::format("header cut at {}", n));
                continue;
            }
            require(r && *r && (*r)->inputs.size() == complete, std::format("cut at {}: {} records", n, complete));
            require((*r)->notes.empty() == (n == ends[complete]), std::format("cut at {}: discarded tail noted", n));
        }
        // A damaged record ends the replay there.
        {
            MemoryFilesystem fs = base.crashed();
            (*fs.bytes(segment))[ends[5] + 9] ^= std::byte{4};
            const auto r = recover(fs);
            require(r && *r && (*r)->inputs.size() == 5, "replay stops before the damaged record");
        }
        // A record with a valid checksum but invalid content is corruption.
        {
            MemoryFilesystem fs = base.crashed();
            auto& data = *fs.bytes(segment);
            data.resize(ends[3]);
            const std::vector<std::byte> junk{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
            const auto crc = static_cast<std::uint32_t>(::crc32(0L, reinterpret_cast<const Bytef*>(junk.data()), 3));
            for (const std::uint32_t v : {3u, crc})
                for (int i = 0; i < 4; ++i) data.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
            data.insert(data.end(), junk.begin(), junk.end());
            require(!recover(fs), "undecodable record with a valid checksum is an error");
        }
    });

    suite.run("crash at every file operation keeps durable history", [&] {
        std::mt19937_64 rng(11);
        std::size_t cases = 0;
        // Scheduled ticks (clock) and entities (drops); the other histories
        // only add block entities, which the round-trip cases cover.
        for (const auto* h_ptr : {&histories[0], &histories[2]}) {
            const auto& h = *h_ptr;
            for (const Policy p : {Policy{3, 7}, Policy{1, 5}, Policy{1000, 4}, Policy{2, 6, 3}}) {
                MemoryFilesystem clean;
                const auto all = run(clean, h, p);
                require(all.attempted == h.ticks(), "clean run completes");
                const auto total = clean.operations();
                for (std::size_t k = 0; k <= total; ++k) {
                    MemoryFilesystem fs;
                    fs.crash_at(k);
                    const auto o = run(fs, h, p);
                    for (int variant = 0; variant < 2; ++variant) {
                        auto after = variant == 0 ? fs.crashed() : fs.crashed(&rng);
                        // Torn images continue too: a discarded tail must not
                        // hide the records logged after recovery.
                        check_after(std::move(after), h, o, std::format("{} sync {} ckpt {} crash at {}/{} variant {}", h.name, p.sync_every,
                                                                        p.checkpoint_every, k, total, variant),
                                    variant == 1 || k % 2 == 0);
                        ++cases;
                    }
                }
            }
        }
        std::cout << std::format("  {} crash images recovered and continued\n", cases);
    });

    suite.run("I/O errors fail closed and leave a recoverable world", [&] {
        const auto& h = histories[3];
        const Policy p{2, 6};
        MemoryFilesystem clean;
        (void)run(clean, h, p);
        for (std::size_t k = 0; k < clean.operations(); ++k) {
            MemoryFilesystem fs;
            fs.fail_once(k, "no space left on device");
            const auto o = run(fs, h, p);
            // The process keeps running (no crash): its own view, and the view
            // after a later power loss, both recover.
            check_after(fs.crashed(), h, o, std::format("error at {} after power loss", k));
            check_after(std::move(fs), h, o, std::format("error at {}", k));
        }
    });

    suite.run("logging resumes after a checkpoint and the manifest state is re-read after a failed switch", [&] {
        const auto& h = histories[0];
        // A failed append: logging stops until a checkpoint at the last logged tick.
        {
            MemoryFilesystem fs;
            WorldDirectory dir(fs, "/world", bundle_id, rules_id());
            require(dir.checkpoint(checkpoint_at(h, 0)).has_value(), "checkpoint");
            require(dir.append(input_at(h, 1), true).has_value(), "append 1");
            fs.fail_once(fs.operations(), "disk full");
            require(!dir.append(input_at(h, 2), true) && !dir.logging(), "append 2 fails and logging stops");
            require(!dir.append(input_at(h, 2), true), "no append after a failure");
            require(dir.checkpoint(checkpoint_at(h, 1)).has_value() && dir.logging(), "checkpoint at tick 1 restarts logging");
            for (Tick t = 2; t <= 5; ++t) require(dir.append(input_at(h, t), true).has_value(), "append");
            const auto r = recover_image(fs);
            require(r && *r && check_recovery(h, **r, "resumed") == 5, "history continues");
        }
        // After recovery, a checkpoint may end the history before inputs that
        // could not be applied, but not before the recovered checkpoint.
        {
            MemoryFilesystem fs;
            {
                WorldDirectory dir(fs, "/world", bundle_id, rules_id());
                require(dir.checkpoint(checkpoint_at(h, 4)).has_value(), "checkpoint at 4");
                for (Tick t = 5; t <= 9; ++t) require(dir.append(input_at(h, t), true).has_value(), "append");
            }
            WorldDirectory again(fs, "/world", bundle_id, rules_id());
            const auto r = again.recover();
            require(r && *r && (*r)->inputs.size() == 5, "five logged inputs");
            require(!again.checkpoint(checkpoint_at(h, 3)), "no checkpoint before the recovered one");
            require(again.checkpoint(checkpoint_at(h, 6)).has_value(), "checkpoint abandoning ticks 7-9");
            require(!again.checkpoint(checkpoint_at(h, 5)), "logging resumed: the history is at 6");
            require(again.append(input_at(h, 7), true).has_value(), "logging continues after 6");
            const auto after = recover_image(fs);
            require(after && *after && check_recovery(h, **after, "abandoned") == 7, "the abandoned inputs are gone");
        }
        // A failed manifest switch: further checkpoints are refused until recovery.
        {
            MemoryFilesystem probe;
            WorldDirectory measure(probe, "/world", bundle_id, rules_id());
            (void)measure.checkpoint(checkpoint_at(h, 0));
            const auto before = probe.operations();
            (void)measure.checkpoint(checkpoint_at(h, 0));
            const auto per_checkpoint = probe.operations() - before; // up to the removals
            for (std::size_t step = 0; step < per_checkpoint; ++step) {
                MemoryFilesystem fs;
                WorldDirectory dir(fs, "/world", bundle_id, rules_id());
                require(dir.checkpoint(checkpoint_at(h, 0)).has_value(), "first");
                fs.fail_once(fs.operations() + step, "I/O error");
                if (dir.checkpoint(checkpoint_at(h, 0))) continue; // the failing step was a cleanup
                // Before the manifest switch the directory is unchanged and a
                // retry works; after it, the state must be read again.
                if (!dir.checkpoint(checkpoint_at(h, 0))) {
                    const auto r = dir.recover();
                    require(r && *r, std::format("step {}: recovery after a failed checkpoint", step));
                    require(dir.checkpoint(checkpoint_at(h, 0)).has_value(), std::format("step {}: checkpoint after recovery", step));
                }
                require(dir.append(input_at(h, 1), true).has_value(), std::format("step {}: logging after recovery", step));
            }
        }
    });

    suite.run("a world directory has one owner", [&] {
        const auto& h = histories[0];
        MemoryFilesystem fs;
        std::optional<WorldDirectory> first(std::in_place, fs, "/world", bundle_id, rules_id());
        require(first->checkpoint(checkpoint_at(h, 0)).has_value() && first->owned(), "the first object owns the world");
        for (Tick t = 1; t <= 3; ++t) require(first->append(input_at(h, t), true).has_value(), "append");
        const auto before = contents(fs, "/world");
        {
            // The second owner is refused before it reads or writes anything.
            WorldDirectory second(fs, "/world", bundle_id, rules_id());
            const auto r = second.recover();
            require(!r && r.error().find("in use") != std::string::npos, "recovery refused");
            require(!second.checkpoint(checkpoint_at(h, 3)) && !second.begin_checkpoint(3), "checkpoints refused");
            require(!second.append(input_at(h, 4), true) && !second.owned(), "appends refused");
            require(contents(fs, "/world") == before, "the refused owner changed no file");
        }
        require(first->append(input_at(h, 4), true).has_value(), "the first owner continues");
        first.reset();
        WorldDirectory next(fs, "/world", bundle_id, rules_id());
        const auto r = next.recover();
        require(r && *r && check_recovery(h, **r, "next owner") == 4, "a new owner recovers after the first is gone");
    });

    suite.run("posix filesystem", [&] {
        char templ[] = "/tmp/circuit-storage-XXXXXX";
        const auto* made = ::mkdtemp(templ);
        require(made != nullptr, "temporary directory");
        const std::filesystem::path root = std::filesystem::path(made) / "world";
        circuit::storage::PosixFilesystem fs;
        const auto& h = histories[1];
        {
            WorldDirectory disk(fs, root, bundle_id, rules_id());
            require(disk.checkpoint(checkpoint_at(h, 0)).has_value(), "checkpoint on disk");
            for (Tick t = 1; t <= h.ticks(); ++t) require(disk.append(input_at(h, t), t % 4 == 0).has_value(), "append on disk");
        }
        std::optional<WorldDirectory> reader(std::in_place, fs, root, bundle_id, rules_id());
        const auto r = reader->recover();
        require(r && *r && check_recovery(h, **r, "disk") == h.ticks(), "recovered from disk");
        // Ownership between objects and between processes. flock belongs to
        // the open file, so a second object of this process conflicts too.
        {
            WorldDirectory other(fs, root, bundle_id, rules_id());
            const auto refused = other.recover();
            require(!refused && refused.error().find(std::format("process {}", ::getpid())) != std::string::npos,
                    "a second object is refused and the holder named");
        }
        const auto before = contents(fs, root);
        const auto child_recovers = [&] {
            const pid_t pid = ::fork();
            if (pid == 0) {
                WorldDirectory d(fs, root, bundle_id, rules_id());
                const auto r = d.recover();
                ::_exit(r && *r ? 0 : (!r && r.error().find("in use") != std::string::npos ? 3 : 1));
            }
            int status = 0;
            ::waitpid(pid, &status, 0);
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        };
        require(child_recovers() == 3, "another process is refused while the world is owned");
        require(contents(fs, root) == before, "the refused process changed no file");
        reader.reset();
        require(child_recovers() == 0, "another process recovers once the owner is gone");
        require(fs.write_file(root / "x", bytes("abc")).has_value() && fs.read_file(root / "x").value() == bytes("abc"), "write and read");
        require(!fs.read_file(root / "missing") && !fs.append(root / "missing", bytes("a"), true), "missing files fail");
        std::filesystem::remove_all(made);
    });

    suite.run("levels survive checkpoints, snapshots, logs and recovery", [&] {
        // The same machines in the overworld and the Nether, raised into both
        // levels' height (tests/sim/scenarios.hpp).
        const auto drops = raised(drops_scenario(), 8);
        const auto h = record(in_levels(drops, DimensionId::overworld, drops, DimensionId::the_nether), 60);
        require(h.dumps.back().levels.size() == 2, "two levels hold state");
        for (const Tick t : {Tick{1}, Tick{30}, h.ticks()}) {
            const auto c = checkpoint_at(h, t);
            const auto encoded = circuit::storage::encode_checkpoint(c);
            const auto decoded = circuit::storage::decode_checkpoint(encoded);
            require(decoded && *decoded == c, std::format("checkpoint at {} round-trips both levels", t));
            Engine engine(bundle(), h.seed, c.state);
            require(engine.dump() == c.state, std::format("restored at {}", t));
            auto snap = engine.snapshot();
            const Checkpoint from_snapshot{bundle_id, rules_id(), h.seed, std::move(snap.state), c.extras};
            const auto via_snapshot = circuit::storage::encode_checkpoint(from_snapshot, snap.sections, bundle(), nullptr);
            require(via_snapshot == encoded, std::format("snapshot at {} encodes like the dump", t));
        }
        // Log records keep each command's level: recovery replays to the same state.
        MemoryFilesystem fs;
        const auto o = run(fs, h, {.sync_every = 2, .checkpoint_every = 25});
        require(o.durable == h.ticks(), "complete run");
        const auto r = recover(fs);
        require(r && *r && check_recovery(h, **r, "levels") == h.ticks(), "recovered with both levels");
        bool nether = false;
        for (const auto& in : (*r)->inputs)
            for (const auto& c : in.commands) nether |= c.dimension == DimensionId::the_nether;
        require(nether, "log records name the Nether");
    });

    suite.run("typed tickets, presences and idle levels survive logs and recovery", [&] {
        // tests/sim/scenarios.hpp: tickets of several types and lifetimes, a
        // player's presence in each level, and a Nether that idles.
        const auto h = record(chunk_levels_scenario(), 450);
        const auto& last = h.dumps.back();
        require(last.level(DimensionId::the_nether).last_active == 419 && h.dumps[100].overworld().presences.size() == 1
                    && h.dumps[410].level(DimensionId::the_nether).presences.size() == 1 && h.dumps[410].overworld().presences.empty(),
                "the state to keep");
        MemoryFilesystem fs;
        const auto o = run(fs, h, {.sync_every = 3, .checkpoint_every = 100});
        require(o.durable == h.ticks(), "complete run");
        const auto r = recover(fs);
        require(r && *r && check_recovery(h, **r, "chunk levels") == h.ticks(), "recovered");
        for (const Tick t : {Tick{20}, Tick{31}, Tick{65}, Tick{92}, Tick{405}}) {
            Engine engine(bundle(), h.seed, h.dumps[t]);
            require(engine.dump() == h.dumps[t], std::format("restored at {}", t));
            for (Tick next = t + 1; next <= std::min<Tick>(t + 20, h.ticks()); ++next) engine.advance(h.inputs[next - 1]);
            require(engine.dump() == h.dumps[std::min<Tick>(t + 20, h.ticks())], std::format("continued from {}", t));
        }
    });

    return suite.finish();
}
