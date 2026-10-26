// SPDX-License-Identifier: AGPL-3.0-only
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/level_random.hpp>
#include <circuit/gameplay/physics.hpp>
#include <circuit/protocol/codec.hpp>
#include <circuit/protocol/framing.hpp>
#include <circuit/storage/checkpoint.hpp>
#include <set>
#include <unordered_map>
#include <utility>
#include <format>
#include <openssl/evp.h>

namespace circuit::storage {
using gameplay::WorldDump;
using protocol::Reader;
using protocol::Writer;

namespace {
constexpr std::string_view magic{"CIRCKPT\0", 8};
// 41: player inventories carry an open enchanting table's slots and offers
// and the player's enchantment seed;
// 40: player inventories carry an open anvil's slots, name and cost;
// 39: block entities carry the recipes a furnace cooked;
// 38: entities carry their own random;
// 37: player inventories carry the player's experience;
// 36: entities carry an experience orb's experience and orbs;
// 35: brewing stands have rules of their own (their block entities keep
// their fuel, brew time and ingredient in the furnace times);
// 34: player inventories carry an open loom's slots and pattern;
// 33: player inventories carry an open grindstone's slots;
// 32: crafters have rules of their own; block entities carry a crafter's
// disabled slots and crafting ticks;
// 31: player inventories carry an open smithing table's slots;
// 30: player inventories carry an open stonecutter's slots and recipe;
// 29: player inventories carry an open crafting table's result and grid;
// 28: furnaces, smokers and blast furnaces carry their times;
// 27: levels carry the state random ticks pick positions from;
// 26: the world carries its difficulty;
// 25: entities carry how far they have fallen;
// 24: levels carry their loot tables' random sequences, the world the settings
// new sequences start with;
// 22: levels carry their redstone torches' recent turn-offs;
// 21: entities carry how far pistons pushed them in their last pushed tick;
// 20: boats carry their kind, last state and last vertical move, minecarts
// whether they were in water;
// 19: arrows carry whether they are in the ground, their life and fire;
// 18: items carry their health;
// 17: primed TNT carries its fuse;
// 16: falling blocks carry their block state and fall time;
// 15: entities saved with a column that loaded again record when they come back;
// 14: entities count their ticks, and an item's age may be -32768 (never);
// 13: entities have a kind, positions and velocities are doubles (before:
// 1/4096 block units), and levels carry their random sequence's state;
// 12: the world's weather;
// 11: the world's day time;
// 10: the world's game rules set by commands;
// 9: columns carry the tick their pending ticks wait from (saved) and the
// tick they run again after a reload (versions before 9: saved columns wait
// from the checkpoint's tick);
// 8: tickets carry their type, level and expiry, columns their load and
// simulation levels, and levels their players and last active tick (versions
// before 8 read tickets as forced squares, ticking columns as level 31 and
// border columns as level 33);
// 7: the state is per level (each with its columns, block entities, entities,
// scheduled ticks, block events and counters), and tickets name their level;
// 6: stacks carry their data components; 5: the header names the block
// registry; 4: stacks carry damage, and player inventories follow the
// tickets; 3: hopper block entities carry the tick they last ticked; 2:
// tickets carry a load radius. Versions 1 to 6 are still read, as the
// overworld's state: versions before 5 predate the block registry and hold
// only states that circuit-blocks-1 keeps; versions before 6 have stacks
// without components.
constexpr std::uint32_t version = 41;
constexpr std::string_view version_4_registry = "circuit-blocks-1";
constexpr std::int32_t max_chunk = 1 << 26;       // block coordinates within ±2^30
constexpr std::int32_t max_section_y = 1 << 26;

std::array<unsigned char, 32> sha256(std::span<const std::byte> bytes) {
    std::array<unsigned char, 32> out{};
    unsigned int length = 0;
    EVP_Digest(bytes.data(), bytes.size(), out.data(), &length, EVP_sha256(), nullptr);
    return out;
}

void pos(Writer& w, contracts::BlockPos p) { w.i32(p.x).i32(p.y).i32(p.z); }

// Packs indices with a fixed width, no entry spanning two longs.
void packed(Writer& w, const std::array<std::uint32_t, 4096>& values, unsigned bits) {
    const unsigned per_long = 64 / bits;
    for (std::size_t base = 0; base < values.size(); base += per_long) {
        std::uint64_t word = 0;
        for (unsigned k = 0; k < per_long && base + k < values.size(); ++k)
            word |= static_cast<std::uint64_t>(values[base + k]) << (k * bits);
        w.i64(static_cast<std::int64_t>(word));
    }
}

// Palette and packed indices of one section, without its y. Empty when the
// section holds only air (value 0), which the format leaves out.
template <class Raw>
std::vector<std::byte> encode_section(Raw raw) {
    std::vector<std::uint32_t> palette;
    std::unordered_map<std::uint32_t, std::uint32_t> index;
    std::array<std::uint32_t, 4096> values{};
    bool any = false;
    for (std::size_t i = 0; i < 4096; ++i) {
        const std::uint32_t v = raw(i);
        any = any || v != 0;
        std::uint32_t at = 0;
        // Palettes are usually tiny: search them before hashing.
        if (palette.size() <= 8) {
            at = static_cast<std::uint32_t>(std::ranges::find(palette, v) - palette.begin());
            if (at == palette.size()) {
                palette.push_back(v);
                if (palette.size() > 8)
                    for (std::uint32_t k = 0; k < palette.size(); ++k) index.emplace(palette[k], k);
            }
        } else {
            const auto [it, fresh] = index.try_emplace(v, static_cast<std::uint32_t>(palette.size()));
            if (fresh) palette.push_back(v);
            at = it->second;
        }
        values[i] = at;
    }
    if (!any) return {};
    Writer w;
    w.varint(static_cast<std::int32_t>(palette.size()));
    for (const auto v : palette) w.varint(static_cast<std::int32_t>(v));
    const auto bits = std::max(1u, static_cast<unsigned>(std::bit_width(palette.size() - 1)));
    if (palette.size() > 1) packed(w, values, bits);
    return w.take();
}

void stack(Writer& w, const contracts::ItemStack& s) {
    w.u16(s.item).u8(s.count).u16(s.damage);
    std::vector<std::byte> patch;
    contracts::write_patch(patch, s.components);
    w.raw(patch);
}

// Writes one level: counters, the column table, block entities, entities,
// scheduled ticks and block events. `blocks(i)` gives column i's non-empty
// sections as (section y, encoding) in ascending y.
template <class Blocks>
void encode_level(Writer& w, const gameplay::LevelDump& l, Blocks blocks) {
    w.u8(static_cast<std::uint8_t>(l.dimension));
    w.varlong(static_cast<std::int64_t>(l.next_suborder)).varlong(static_cast<std::int64_t>(l.next_block_entity_order));
    w.varlong(static_cast<std::int64_t>(l.last_active)).varlong(static_cast<std::int64_t>(l.random));
    w.i32(static_cast<std::int32_t>(l.random_tick));
    w.varint(static_cast<std::int32_t>(l.sequences.size()));
    for (const auto& [id, sequence] : l.sequences)
        w.string(id, 256).i64(static_cast<std::int64_t>(sequence.low())).i64(static_cast<std::int64_t>(sequence.high()));
    w.varint(static_cast<std::int32_t>(l.presences.size()));
    for (const auto& p : l.presences)
        w.i64(static_cast<std::int64_t>(p.player.hi)).i64(static_cast<std::int64_t>(p.player.lo)).f64(p.position.x).f64(p.position.y)
            .f64(p.position.z).boolean(p.spectator);
    w.varint(static_cast<std::int32_t>(l.chunks.size()));
    for (std::size_t i = 0; i < l.chunks.size(); ++i) {
        const auto& ch = l.chunks[i];
        w.i32(ch.pos.x).i32(ch.pos.z).varint(static_cast<std::int32_t>(ch.generation)).u8(static_cast<std::uint8_t>(ch.level));
        w.u8(ch.load).u8(ch.simulation).varlong(static_cast<std::int64_t>(ch.saved_at)).varlong(static_cast<std::int64_t>(ch.resumes));
        const auto sections = blocks(i);
        w.varint(static_cast<std::int32_t>(sections.size()));
        for (const auto& [sy, bytes] : sections) w.i32(sy).raw(bytes);
    }
    w.varint(static_cast<std::int32_t>(l.block_entities.size()));
    for (const auto& [p, be] : l.block_entities) {
        pos(w, p);
        w.u8(static_cast<std::uint8_t>(be.kind)).varlong(static_cast<std::int64_t>(be.order)).varint(static_cast<std::int32_t>(be.slots.size()));
        for (const auto& st : be.slots) stack(w, st);
        w.u8(be.cooldown).u8(be.signal).i32(static_cast<std::int32_t>(be.moving)).u8(be.progress).u8(be.motion);
        w.varlong(static_cast<std::int64_t>(be.ticked));
        std::vector<std::byte> patch;
        contracts::write_patch(patch, be.components);
        w.raw(patch).prefixed_bytes(be.saved.bytes());
        w.i32(be.lit_time).i32(be.lit_total).i32(be.cook_time).i32(be.cook_total);
        w.u16(be.disabled).i32(be.crafting_ticks);
        w.varint(static_cast<std::int32_t>(be.recipes_used.size()));
        for (const auto& [id, count] : be.recipes_used) w.string(id, 256).varint(count);
    }
    w.varint(static_cast<std::int32_t>(l.entities.size()));
    for (const auto& e : l.entities) {
        w.varlong(static_cast<std::int64_t>(e.id.value)).u8(static_cast<std::uint8_t>(e.kind)).varlong(static_cast<std::int64_t>(e.tick_count));
        w.f64(e.position.x).f64(e.position.y).f64(e.position.z).f64(e.velocity.x).f64(e.velocity.y).f64(e.velocity.z);
        stack(w, e.stack);
        w.varint(e.age).varlong(static_cast<std::int64_t>(e.last_tick));
        w.boolean(e.on_ground);
        w.u16(e.pickup_delay);
        w.varlong(static_cast<std::int64_t>(e.returns));
        w.i32(static_cast<std::int32_t>(e.carried.raw())).varlong(static_cast<std::int64_t>(e.fall_time));
        w.varint(e.fuse).varint(e.health);
        w.boolean(e.in_ground).varint(e.life).varint(e.fire);
        w.u8(e.variant).u8(e.status).f64(e.last_dy).boolean(e.in_water);
        w.varlong(static_cast<std::int64_t>(e.piston_tick)).f64(e.piston_push.x).f64(e.piston_push.y).f64(e.piston_push.z);
        w.f32(e.yaw).boolean(e.flipped);
        w.f64(e.fall_distance);
        w.varint(e.experience).varint(e.orbs);
        w.boolean(e.seeded).i64(static_cast<std::int64_t>(e.own_random));
    }
    w.varint(static_cast<std::int32_t>(l.ticks.size()));
    for (const auto& t : l.ticks) {
        w.u8(static_cast<std::uint8_t>(t.list));
        pos(w, t.tick.pos);
        w.u8(static_cast<std::uint8_t>(t.tick.block)).varlong(static_cast<std::int64_t>(t.tick.due)).i8(t.tick.priority);
        w.varlong(static_cast<std::int64_t>(t.tick.suborder));
    }
    w.varint(static_cast<std::int32_t>(l.block_events.size()));
    for (const auto& e : l.block_events) {
        pos(w, e.pos);
        w.u8(static_cast<std::uint8_t>(e.block)).u8(e.event).u8(e.param);
    }
    w.varint(static_cast<std::int32_t>(l.turn_offs.size()));
    for (const auto& [p, when] : l.turn_offs) {
        pos(w, p);
        w.varlong(static_cast<std::int64_t>(when));
    }
}

// Writes the body: time, the entity id counter, the levels, tickets, players
// and extras. `blocks(level, i)` gives the sections of column i of a level.
template <class Blocks>
void encode_body(Writer& w, const Checkpoint& c, Blocks blocks) {
    const auto& s = c.state;
    w.varlong(static_cast<std::int64_t>(s.time)).varlong(static_cast<std::int64_t>(s.next_entity_id));
    w.varlong(static_cast<std::int64_t>(s.started));
    w.varint(static_cast<std::int32_t>(s.game_rules.size()));
    for (const auto& [name, value] : s.game_rules) w.string(name, 64).i32(value);
    w.varlong(s.day_time);
    const auto& weather = s.weather;
    w.varint(weather.clear_time).varint(weather.rain_time).varint(weather.thunder_time);
    w.u8(static_cast<std::uint8_t>(weather.raining) | static_cast<std::uint8_t>(weather.thundering) << 1);
    w.i32(std::bit_cast<std::int32_t>(weather.rain_level)).i32(std::bit_cast<std::int32_t>(weather.thunder_level));
    w.i32(s.sequence_settings.salt)
        .u8(static_cast<std::uint8_t>(s.sequence_settings.include_world_seed) | static_cast<std::uint8_t>(s.sequence_settings.include_sequence_id) << 1);
    w.u8(s.difficulty);
    w.varint(static_cast<std::int32_t>(s.levels.size()));
    for (std::size_t k = 0; k < s.levels.size(); ++k)
        encode_level(w, s.levels[k], [&](std::size_t i) { return blocks(k, i); });
    w.varint(static_cast<std::int32_t>(s.tickets.size()));
    for (const auto& t : s.tickets)
        w.i32(static_cast<std::int32_t>(t.id)).i32(t.center.x).i32(t.center.z).u8(t.radius).u8(static_cast<std::uint8_t>(t.dimension))
            .u8(static_cast<std::uint8_t>(t.type)).u8(t.level).varlong(static_cast<std::int64_t>(t.expires));
    w.varint(static_cast<std::int32_t>(s.players.size()));
    for (const auto& p : s.players) {
        w.i64(static_cast<std::int64_t>(p.player.hi)).i64(static_cast<std::int64_t>(p.player.lo));
        stack(w, p.cursor);
        for (const auto& st : p.slots) stack(w, st);
        w.u8(p.drag_stage).u8(p.drag_type).varint(static_cast<std::int32_t>(p.drag_slots.size()));
        for (const auto slot : p.drag_slots) w.i16(slot);
        for (const auto& st : p.table) stack(w, st);
        for (const auto& st : p.cutter) stack(w, st);
        w.i16(p.cutter_recipe);
        for (const auto& st : p.smith) stack(w, st);
        for (const auto& st : p.grind) stack(w, st);
        for (const auto& st : p.loom) stack(w, st);
        w.i16(p.loom_pattern);
        w.varint(p.xp_level).f32(p.xp_progress).varint(p.xp_total).varlong(static_cast<std::int64_t>(p.xp_ready));
        for (const auto& st : p.anvil) stack(w, st);
        w.string(p.anvil_name, 256).varint(p.anvil_cost);
        for (const auto& st : p.enchant) stack(w, st);
        for (std::size_t i = 0; i < 3; ++i) w.varint(p.enchant_costs[i]).varint(p.enchant_clues[i]).varint(p.enchant_clue_levels[i]);
        w.i32(p.xp_seed);
    }
    w.varint(static_cast<std::int32_t>(c.extras.size()));
    for (const auto& [name, bytes] : c.extras) w.string(name, 256).prefixed_bytes(bytes);
}

struct BodyReader {
    Reader r;
    std::string error;
    std::int32_t version{};

    bool stack(contracts::ItemStack& s) {
        if (!get(r.u16(), s.item) || !get(r.u8(), s.count)) return false;
        if (version >= 4 && !get(r.u16(), s.damage)) return false;
        if (version < 6) return true;
        auto rest = r.rest();
        const auto size = rest.size();
        auto patch = contracts::read_patch(rest);
        if (!patch || !r.raw(size - rest.size())) {
            error = "malformed item components";
            return false;
        }
        s.components = std::move(*patch);
        return true;
    }

    template <class T>
    bool get(protocol::Decoded<T> v, T& out) {
        if (!v) {
            error = "truncated or malformed body";
            return false;
        }
        out = *v;
        return true;
    }
    bool u64(std::uint64_t& out) {
        std::int64_t v{};
        if (!get(r.varlong(), v)) return false;
        out = static_cast<std::uint64_t>(v);
        return true;
    }
    bool vec(contracts::Vec3& v) { return get(r.f64(), v.x) && get(r.f64(), v.y) && get(r.f64(), v.z); }
    // Before version 13: 1/4096 block units, converted exactly.
    bool fixed_vec(contracts::Vec3& v) {
        std::int64_t x{}, y{}, z{};
        if (!get(r.i64(), x) || !get(r.i64(), y) || !get(r.i64(), z)) return false;
        v = {static_cast<double>(x) / 4096.0, static_cast<double>(y) / 4096.0, static_cast<double>(z) / 4096.0};
        return true;
    }
    bool count(std::size_t& out, std::size_t min_bytes) {
        return get(r.length(std::size_t{1} << 28, min_bytes), out);
    }
    bool pos(contracts::BlockPos& p) { return get(r.i32(), p.x) && get(r.i32(), p.y) && get(r.i32(), p.z); }
};

// Reads a level's column table, block entities, entities, scheduled ticks,
// block events and torch turn-offs into `l`.
bool decode_level_lists(BodyReader& b, gameplay::LevelDump& l) {
    auto& s = l;
    std::size_t chunks = 0;
    if (!b.count(chunks, 11)) return false;
    for (std::size_t i = 0; i < chunks; ++i) {
        WorldDump::Chunk ch;
        std::int32_t generation{};
        std::uint8_t level{};
        std::size_t secs = 0;
        if (!b.get(b.r.i32(), ch.pos.x) || !b.get(b.r.i32(), ch.pos.z) || !b.get(b.r.varint(), generation) || !b.get(b.r.u8(), level))
            return false;
        if (b.version >= 8 && (!b.get(b.r.u8(), ch.load) || !b.get(b.r.u8(), ch.simulation))) return false;
        if (b.version >= 9 && (!b.u64(ch.saved_at) || !b.u64(ch.resumes))) return false;
        if (!b.count(secs, 6)) return false;
        // Column and section positions stay within the coordinates block
        // positions can express; the engine checks them against the world.
        if (level > (b.version >= 8 ? 3 : 2) || generation < 0 || std::abs(ch.pos.x) > max_chunk || std::abs(ch.pos.z) > max_chunk) {
            b.error = "invalid column";
            return false;
        }
        ch.generation = static_cast<std::uint32_t>(generation);
        if (b.version >= 8) {
            ch.level = static_cast<gameplay::ChunkLevel>(level);
        } else {
            // Before version 8: unloaded, loaded (border) or ticking.
            ch.level = level == 2 ? gameplay::ChunkLevel::entity_ticking : level == 1 ? gameplay::ChunkLevel::border : gameplay::ChunkLevel::unloaded;
            ch.load = ch.simulation = level == 2 ? 31 : level == 1 ? 33 : contracts::unloaded_level;
            if (level == 1) ch.simulation = contracts::unloaded_level;
        }
        s.chunks.push_back(ch);
        for (std::size_t k = 0; k < secs; ++k) {
            std::int32_t sy{};
            std::size_t palette_size = 0;
            if (!b.get(b.r.i32(), sy) || !b.count(palette_size, 1)) return false;
            if (std::abs(sy) > max_section_y) {
                b.error = "invalid section";
                return false;
            }
            if (palette_size == 0 || palette_size > 4096) {
                b.error = "invalid palette";
                return false;
            }
            std::vector<std::uint32_t> palette(palette_size);
            for (auto& p : palette) {
                std::int32_t raw{};
                if (!b.get(b.r.varint(), raw)) return false;
                p = static_cast<std::uint32_t>(raw);
            }
            std::array<std::uint32_t, 4096> values{};
            if (palette_size > 1) {
                const auto bits = static_cast<unsigned>(std::bit_width(palette_size - 1));
                const unsigned per_long = 64 / bits;
                for (std::size_t base = 0; base < 4096; base += per_long) {
                    std::int64_t word{};
                    if (!b.get(b.r.i64(), word)) return false;
                    for (unsigned j = 0; j < per_long && base + j < 4096; ++j) {
                        values[base + j] = static_cast<std::uint32_t>((static_cast<std::uint64_t>(word) >> (j * bits)) & ((1ULL << bits) - 1));
                        if (values[base + j] >= palette_size) {
                            b.error = "palette index out of range";
                            return false;
                        }
                    }
                }
            }
            for (std::size_t idx = 0; idx < 4096; ++idx) {
                const auto raw = palette[values[idx]];
                if (raw == 0) continue; // air
                s.blocks.push_back({contracts::BlockPos{ch.pos.x * 16 + static_cast<std::int32_t>(idx & 15),
                                                        sy * 16 + static_cast<std::int32_t>(idx >> 8),
                                                        ch.pos.z * 16 + static_cast<std::int32_t>((idx >> 4) & 15)},
                                    raw});
            }
        }
    }
    std::size_t n = 0;
    if (!b.count(n, 20)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        contracts::BlockPos p;
        gameplay::BlockEntity be;
        std::uint8_t kind{};
        std::size_t slots = 0;
        if (!b.pos(p) || !b.get(b.r.u8(), kind) || !b.u64(be.order) || !b.count(slots, 3)) return false;
        be.kind = static_cast<gameplay::BlockType>(kind);
        be.slots.resize(slots);
        for (auto& st : be.slots)
            if (!b.stack(st)) return false;
        std::int32_t moving{};
        if (!b.get(b.r.u8(), be.cooldown) || !b.get(b.r.u8(), be.signal) || !b.get(b.r.i32(), moving) || !b.get(b.r.u8(), be.progress)
            || !b.get(b.r.u8(), be.motion))
            return false;
        if (b.version >= 3) {
            std::int64_t ticked{};
            if (!b.get(b.r.varlong(), ticked) || ticked < 0) return false;
            be.ticked = static_cast<gameplay::Tick>(ticked);
        }
        if (b.version >= 6) {
            auto rest = b.r.rest();
            const auto size = rest.size();
            auto patch = contracts::read_patch(rest);
            if (!patch || !b.r.raw(size - rest.size())) return false;
            be.components = std::move(*patch);
            protocol::Bytes saved;
            if (!b.get(b.r.prefixed_bytes(1u << 21), saved)) return false;
            be.saved = contracts::Blob(std::move(saved));
        }
        if (b.version >= 28) {
            if (!b.get(b.r.i32(), be.lit_time) || !b.get(b.r.i32(), be.lit_total) || !b.get(b.r.i32(), be.cook_time)
                || !b.get(b.r.i32(), be.cook_total))
                return false;
            if (be.lit_time < 0 || be.lit_total < 0 || be.cook_time < 0 || be.cook_total < 0) return false;
        }
        if (b.version >= 32) {
            if (!b.get(b.r.u16(), be.disabled) || !b.get(b.r.i32(), be.crafting_ticks)) return false;
            if (be.disabled > 0x1FF || be.crafting_ticks < 0) return false;
        }
        if (b.version >= 39) {
            std::int32_t recipes{};
            if (!b.get(b.r.varint(), recipes) || recipes < 0 || recipes > 4096) return false;
            for (std::int32_t k = 0; k < recipes; ++k) {
                std::string id;
                std::int32_t count{};
                if (!b.get(b.r.string(256), id) || !b.get(b.r.varint(), count) || count < 1) return false;
                if (!be.recipes_used.empty() && be.recipes_used.back().first >= id) return false; // ascending, once each
                be.recipes_used.emplace_back(std::move(id), count);
            }
        }
        be.moving = static_cast<std::uint32_t>(moving);
        s.block_entities.push_back({p, std::move(be)});
    }
    // Before version 28 furnaces, smokers and blast furnaces, before 32
    // crafters and before 35 brewing stands had no rules of their own: their
    // registry states become the modeled ones, and their block entities
    // start empty (they could hold nothing the rules used).
    const auto newly_modeled = [&](gameplay::BlockType type, std::uint32_t since) {
        if (b.version >= static_cast<std::int32_t>(since)) return;
        std::set<contracts::BlockPos> converted;
        for (auto& [p, raw] : s.blocks) {
            if (static_cast<gameplay::BlockType>(raw & 0xFF) != gameplay::BlockType::other) continue;
            const auto state = raw >> 8;
            const auto block = gameplay::registry::block_of(state);
            if (!block || !gameplay::modeled_block(*block)) continue;
            if (const auto now = gameplay::physics::modeled_state_of(state); now && now->is(type)) {
                raw = now->raw();
                converted.insert(p);
            }
        }
        for (auto& [p, be] : s.block_entities)
            if (converted.contains(p) && be.kind == gameplay::BlockType::other)
                be = gameplay::BlockEntity{type, be.order, std::vector<contracts::ItemStack>(gameplay::container_size(type)), 0};
    };
    newly_modeled(gameplay::BlockType::furnace, 28);
    newly_modeled(gameplay::BlockType::crafter, 32);
    newly_modeled(gameplay::BlockType::brewing_stand, 35);
    if (!b.count(n, 55)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        gameplay::Entity e;
        std::int32_t age{};
        if (!b.u64(e.id.value)) return false;
        std::uint64_t ticks{};
        if (b.version >= 13) {
            std::uint8_t kind{};
            if (!b.get(b.r.u8(), kind) || (b.version >= 14 && !b.u64(ticks)) || !b.vec(e.position) || !b.vec(e.velocity)) return false;
            e.kind = static_cast<gameplay::EntityKind>(kind);
        } else if (!b.fixed_vec(e.position) || !b.fixed_vec(e.velocity)) {
            return false;
        }
        if (!b.stack(e.stack) || !b.get(b.r.varint(), age) || !b.u64(e.last_tick) || !b.get(b.r.boolean(), e.on_ground)) return false;
        if (b.version >= 4 && !b.get(b.r.u16(), e.pickup_delay)) return false;
        if (b.version >= 15 && !b.u64(e.returns)) return false;
        if (b.version >= 16) {
            std::int32_t carried{};
            std::uint64_t fall_time{};
            if (!b.get(b.r.i32(), carried) || !b.u64(fall_time)) return false;
            const auto state = gameplay::BlockState::decode(static_cast<std::uint32_t>(carried));
            if (!state || fall_time > UINT32_MAX) {
                b.error = "invalid falling block";
                return false;
            }
            e.carried = *state;
            e.fall_time = static_cast<std::uint32_t>(fall_time);
        }
        if (b.version >= 17 && !b.get(b.r.varint(), e.fuse)) return false;
        if (b.version >= 18 && !b.get(b.r.varint(), e.health)) return false;
        if (b.version >= 19 && (!b.get(b.r.boolean(), e.in_ground) || !b.get(b.r.varint(), e.life) || !b.get(b.r.varint(), e.fire))) return false;
        if (b.version >= 20 && (!b.get(b.r.u8(), e.variant) || !b.get(b.r.u8(), e.status) || !b.get(b.r.f64(), e.last_dy) || !b.get(b.r.boolean(), e.in_water))) return false;
        if (b.version >= 21 && (!b.u64(e.piston_tick) || !b.vec(e.piston_push))) return false;
        if (b.version >= 23 && (!b.get(b.r.f32(), e.yaw) || !b.get(b.r.boolean(), e.flipped))) return false;
        if (b.version >= 25 && !b.get(b.r.f64(), e.fall_distance)) return false;
        if (b.version >= 36 && (!b.get(b.r.varint(), e.experience) || !b.get(b.r.varint(), e.orbs))) return false;
        if (b.version >= 38) {
            std::int64_t state{};
            if (!b.get(b.r.boolean(), e.seeded) || !b.get(b.r.i64(), state)) return false;
            e.own_random = static_cast<std::uint64_t>(state);
            if (e.own_random >> 48 || (!e.seeded && e.own_random != 0)) {
                b.error = "invalid entity random";
                return false;
            }
        }
        if (e.experience < 0 || e.orbs < 1) {
            b.error = "invalid experience orb";
            return false;
        }
        e.age = age;
        // Before version 14 nothing reset an item's age, which counted every
        // tick since it was spawned.
        if (b.version < 14) ticks = static_cast<std::uint32_t>(std::max(age, 0));
        if (ticks > UINT32_MAX) {
            b.error = "entity tick count out of range";
            return false;
        }
        e.tick_count = static_cast<std::uint32_t>(ticks);
        s.entities.push_back(e);
    }
    if (!b.count(n, 17)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        WorldDump::PendingTick t;
        std::uint8_t list{}, block{};
        if (!b.get(b.r.u8(), list) || !b.pos(t.tick.pos) || !b.get(b.r.u8(), block) || !b.u64(t.tick.due)
            || !b.get(b.r.i8(), t.tick.priority) || !b.u64(t.tick.suborder))
            return false;
        if (list > 1) {
            b.error = "invalid tick list";
            return false;
        }
        t.list = static_cast<contracts::TickList>(list);
        t.tick.block = static_cast<gameplay::BlockType>(block);
        s.ticks.push_back(t);
    }
    if (!b.count(n, 15)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        gameplay::BlockEvent e;
        std::uint8_t block{};
        if (!b.pos(e.pos) || !b.get(b.r.u8(), block) || !b.get(b.r.u8(), e.event) || !b.get(b.r.u8(), e.param)) return false;
        e.block = static_cast<gameplay::BlockType>(block);
        s.block_events.push_back(e);
    }
    if (b.version >= 22) {
        if (!b.count(n, 13)) return false;
        for (std::size_t i = 0; i < n; ++i) {
            std::pair<contracts::BlockPos, contracts::Tick> t;
            if (!b.pos(t.first) || !b.u64(t.second)) return false;
            s.turn_offs.push_back(t);
        }
    }
    return true;
}

bool decode_body(BodyReader& b, Checkpoint& c) {
    auto& s = c.state;
    if (b.version >= 7) {
        std::size_t levels = 0;
        if (!b.u64(s.time) || !b.u64(s.next_entity_id) || (b.version >= 8 && !b.u64(s.started))) return false;
        if (b.version >= 10) {
            std::size_t rules = 0;
            if (!b.count(rules, 5)) return false;
            for (std::size_t k = 0; k < rules; ++k) {
                std::string name;
                std::int32_t value{};
                if (!b.get(b.r.string(64), name) || !b.get(b.r.i32(), value)) return false;
                if (!s.game_rules.emplace(std::move(name), value).second) {
                    b.error = "duplicate game rule";
                    return false;
                }
            }
        }
        if (b.version >= 11 && !b.get(b.r.varlong(), s.day_time)) return false;
        if (b.version >= 12) {
            auto& weather = s.weather;
            std::uint8_t flags{};
            std::int32_t rain{}, thunder{};
            if (!b.get(b.r.varint(), weather.clear_time) || !b.get(b.r.varint(), weather.rain_time) || !b.get(b.r.varint(), weather.thunder_time)
                || !b.get(b.r.u8(), flags) || !b.get(b.r.i32(), rain) || !b.get(b.r.i32(), thunder))
                return false;
            if (flags > 3) {
                b.error = "invalid weather";
                return false;
            }
            weather.raining = (flags & 1) != 0;
            weather.thundering = (flags & 2) != 0;
            weather.rain_level = std::bit_cast<float>(rain);
            weather.thunder_level = std::bit_cast<float>(thunder);
        }
        if (b.version >= 24) {
            std::uint8_t flags{};
            if (!b.get(b.r.i32(), s.sequence_settings.salt) || !b.get(b.r.u8(), flags)) return false;
            if (flags > 3) {
                b.error = "invalid random sequence settings";
                return false;
            }
            s.sequence_settings.include_world_seed = (flags & 1) != 0;
            s.sequence_settings.include_sequence_id = (flags & 2) != 0;
        }
        if (b.version >= 26) {
            if (!b.get(b.r.u8(), s.difficulty)) return false;
            if (s.difficulty > contracts::max_difficulty) {
                b.error = "invalid difficulty";
                return false;
            }
        }
        if (!b.count(levels, 4)) return false;
        for (std::size_t k = 0; k < levels; ++k) {
            std::uint8_t dimension{};
            if (!b.get(b.r.u8(), dimension)) return false;
            if (!contracts::valid_dimension(dimension) || (k > 0 && static_cast<std::uint8_t>(s.levels.back().dimension) >= dimension)) {
                b.error = "invalid level";
                return false;
            }
            auto& l = s.levels.emplace_back();
            l.dimension = static_cast<contracts::DimensionId>(dimension);
            if (!b.u64(l.next_suborder) || !b.u64(l.next_block_entity_order)) return false;
            if (b.version >= 8) {
                std::size_t presences = 0;
                if (!b.u64(l.last_active)) return false;
                if (b.version >= 13 && !b.u64(l.random)) return false;
                if (b.version >= 27) {
                    std::int32_t state{};
                    if (!b.get(b.r.i32(), state)) return false;
                    l.random_tick = static_cast<std::uint32_t>(state);
                }
                if (b.version >= 24) {
                    std::size_t sequences = 0;
                    if (!b.count(sequences, 17)) return false;
                    for (std::size_t q = 0; q < sequences; ++q) {
                        std::string id;
                        std::int64_t low{}, high{};
                        if (!b.get(b.r.string(256), id) || !b.get(b.r.i64(), low) || !b.get(b.r.i64(), high)) return false;
                        if (id.empty() || (!l.sequences.empty() && l.sequences.back().first >= id)) {
                            b.error = "invalid random sequence";
                            return false;
                        }
                        l.sequences.emplace_back(std::move(id), gameplay::RandomSequence(static_cast<std::uint64_t>(low), static_cast<std::uint64_t>(high)));
                    }
                }
                if (!b.count(presences, 41)) return false;
                for (std::size_t p = 0; p < presences; ++p) {
                    gameplay::Presence presence;
                    std::int64_t hi{}, lo{};
                    if (!b.get(b.r.i64(), hi) || !b.get(b.r.i64(), lo)
                        || !(b.version >= 13 ? b.vec(presence.position) : b.fixed_vec(presence.position))
                        || !b.get(b.r.boolean(), presence.spectator))
                        return false;
                    presence.player = {static_cast<std::uint64_t>(hi), static_cast<std::uint64_t>(lo)};
                    l.presences.push_back(presence);
                }
            }
            if (!decode_level_lists(b, l)) return false;
        }
    } else {
        // Before version 7: the overworld alone, with the counters first.
        auto& l = s.level(contracts::DimensionId::overworld);
        if (!b.u64(s.time) || !b.u64(l.next_suborder) || !b.u64(s.next_entity_id) || !b.u64(l.next_block_entity_order)
            || !decode_level_lists(b, l))
            return false;
    }
    if (b.version < 9 && s.time < UINT64_MAX)
        for (auto& l : s.levels)
            for (auto& ch : l.chunks)
                if (ch.level == gameplay::ChunkLevel::unloaded) ch.saved_at = s.time + 1;
    std::size_t n = 0;
    if (!b.count(n, 13)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        gameplay::Ticket t;
        std::int32_t id{};
        if (!b.get(b.r.i32(), id) || !b.get(b.r.i32(), t.center.x) || !b.get(b.r.i32(), t.center.z) || !b.get(b.r.u8(), t.radius)) return false;
        std::uint8_t load_radius{}; // before version 8; a forced square keeps none
        if (b.version >= 2 && b.version < 8 && !b.get(b.r.u8(), load_radius)) return false;
        if (b.version >= 7) {
            std::uint8_t dimension{};
            if (!b.get(b.r.u8(), dimension)) return false;
            if (!contracts::valid_dimension(dimension)) {
                b.error = "invalid ticket level";
                return false;
            }
            t.dimension = static_cast<contracts::DimensionId>(dimension);
        }
        if (b.version >= 8) {
            std::uint8_t type{};
            if (!b.get(b.r.u8(), type) || !b.get(b.r.u8(), t.level) || !b.u64(t.expires)) return false;
            if (type >= contracts::ticket_type_count) {
                b.error = "invalid ticket type";
                return false;
            }
            t.type = static_cast<contracts::TicketType>(type);
        }
        t.id = static_cast<std::uint32_t>(id);
        s.tickets.push_back(t);
    }
    if (b.version >= 4) {
        if (!b.count(n, 16 + 5 * (contracts::player_slots + 1))) return false;
        for (std::size_t i = 0; i < n; ++i) {
            gameplay::PlayerInventory p;
            std::int64_t hi{}, lo{};
            if (!b.get(b.r.i64(), hi) || !b.get(b.r.i64(), lo) || !b.stack(p.cursor)) return false;
            for (auto& st : p.slots)
                if (!b.stack(st)) return false;
            std::size_t drag = 0;
            if (!b.get(b.r.u8(), p.drag_stage) || !b.get(b.r.u8(), p.drag_type) || !b.count(drag, 2)) return false;
            p.drag_slots.resize(drag);
            for (auto& slot : p.drag_slots)
                if (!b.get(b.r.i16(), slot)) return false;
            if (b.version >= 29)
                for (auto& st : p.table)
                    if (!b.stack(st)) return false;
            if (b.version >= 30) {
                for (auto& st : p.cutter)
                    if (!b.stack(st)) return false;
                if (!b.get(b.r.i16(), p.cutter_recipe)) return false;
            }
            if (b.version >= 31)
                for (auto& st : p.smith)
                    if (!b.stack(st)) return false;
            if (b.version >= 33)
                for (auto& st : p.grind)
                    if (!b.stack(st)) return false;
            if (b.version >= 34) {
                for (auto& st : p.loom)
                    if (!b.stack(st)) return false;
                if (!b.get(b.r.i16(), p.loom_pattern)) return false;
            }
            if (b.version >= 37 && (!b.get(b.r.varint(), p.xp_level) || !b.get(b.r.f32(), p.xp_progress) || !b.get(b.r.varint(), p.xp_total)
                                    || !b.u64(p.xp_ready)))
                return false;
            if (b.version >= 40) {
                for (auto& st : p.anvil)
                    if (!b.stack(st)) return false;
                if (!b.get(b.r.string(256), p.anvil_name) || !b.get(b.r.varint(), p.anvil_cost) || p.anvil_cost < 0) return false;
            }
            if (b.version >= 41) {
                for (auto& st : p.enchant)
                    if (!b.stack(st)) return false;
                for (std::size_t k = 0; k < 3; ++k)
                    if (!b.get(b.r.varint(), p.enchant_costs[k]) || !b.get(b.r.varint(), p.enchant_clues[k])
                        || !b.get(b.r.varint(), p.enchant_clue_levels[k]) || p.enchant_costs[k] < 0 || p.enchant_clues[k] < -1
                        || p.enchant_clue_levels[k] < -1)
                        return false;
                if (!b.get(b.r.i32(), p.xp_seed)) return false;
            }
            p.player = {static_cast<std::uint64_t>(hi), static_cast<std::uint64_t>(lo)};
            s.players.push_back(p);
        }
    }
    if (!b.count(n, 2)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        std::string name;
        protocol::Bytes bytes;
        if (!b.get(b.r.string(256), name) || !b.get(b.r.prefixed_bytes(std::size_t{1} << 30), bytes)) return false;
        c.extras.emplace(std::move(name), std::move(bytes));
    }
    if (b.r.remaining() != 0) {
        b.error = "trailing body data";
        return false;
    }
    gameplay::canonicalize(s);
    return true;
}
} // namespace

std::string sha256_hex(std::span<const std::byte> bytes) {
    std::string out;
    for (const auto b : sha256(bytes)) out += std::format("{:02x}", b);
    return out;
}

namespace {
// The body of a checkpoint whose blocks are listed in its dump.
std::vector<std::byte> dump_body(const Checkpoint& checkpoint) {
    const auto& levels = checkpoint.state.levels;
    std::vector<std::map<contracts::ChunkPos, std::map<std::int32_t, std::array<std::uint32_t, 4096>>>> sections(levels.size());
    for (std::size_t k = 0; k < levels.size(); ++k)
        for (const auto& [p, raw] : levels[k].blocks) sections[k][contracts::chunk_of(p)][p.y >> 4][contracts::section_local_index(p)] = raw;
    Writer body;
    encode_body(body, checkpoint, [&](std::size_t k, std::size_t i) {
        std::vector<std::pair<std::int32_t, std::vector<std::byte>>> out;
        if (const auto it = sections[k].find(levels[k].chunks[i].pos); it != sections[k].end())
            for (const auto& [sy, values] : it->second)
                if (auto bytes = encode_section([&](std::size_t n) { return values[n]; }); !bytes.empty()) out.emplace_back(sy, std::move(bytes));
        return out;
    });
    return body.take();
}

std::vector<std::byte> seal(const Checkpoint& checkpoint, const std::vector<std::byte>& body, const CheckpointOptions& options) {
    const auto compressed = protocol::deflate_bytes(body, options.compression_level);
    Writer w;
    w.raw(protocol::as_bytes(magic)).i32(static_cast<std::int32_t>(version));
    w.string(checkpoint.bundle_id, 64).string(checkpoint.rules_id, 128).string(gameplay::registry::id(), 64);
    w.i64(static_cast<std::int64_t>(checkpoint.seed));
    w.i64(static_cast<std::int64_t>(checkpoint.state.time));
    w.i64(static_cast<std::int64_t>(body.size())).i64(static_cast<std::int64_t>(compressed.size()));
    w.raw(compressed);
    const auto digest = sha256(w.bytes());
    w.raw(std::as_bytes(std::span(digest)));
    return w.take();
}
} // namespace

std::vector<std::byte> checkpoint_body(const Checkpoint& checkpoint) { return dump_body(checkpoint); }

std::vector<std::byte> encode_checkpoint(const Checkpoint& checkpoint, const CheckpointOptions& options) {
    return seal(checkpoint, dump_body(checkpoint), options);
}

struct SectionCache::Entry {
    std::shared_ptr<const SectionBlocks> section; // kept alive, so its address identifies it
    std::vector<std::byte> bytes;
    std::uint64_t used{};
};
SectionCache::SectionCache() = default;
SectionCache::~SectionCache() = default;
std::size_t SectionCache::size() const noexcept { return entries_.size(); }
std::vector<std::shared_ptr<const SectionBlocks>> SectionCache::take_dropped() { return std::exchange(dropped_, {}); }

std::vector<std::byte> encode_checkpoint(const Checkpoint& checkpoint, std::span<const std::vector<ColumnSections>> levels,
                                         const contracts::VersionBundle& bundle, SectionCache* cache, const CheckpointOptions& options) {
    const auto& state = checkpoint.state;
    bool shaped = levels.size() == state.levels.size();
    for (std::size_t k = 0; shaped && k < levels.size(); ++k)
        shaped = state.levels[k].blocks.empty() && levels[k].size() == state.levels[k].chunks.size();
    if (!shaped) throw std::invalid_argument("checkpoint: sections must replace the block lists, one entry per column");
    const auto round = cache ? ++cache->round_ : 0;
    Writer body;
    encode_body(body, checkpoint, [&](std::size_t level, std::size_t i) {
        std::vector<std::pair<std::int32_t, std::vector<std::byte>>> out;
        const auto& list = levels[level][i];
        const auto min_y = bundle.shape(state.levels[level].dimension).min_y;
        for (std::size_t k = 0; k < list.size(); ++k) {
            const auto& section = list[k];
            if (!section) continue;
            const auto sy = (min_y >> 4) + static_cast<std::int32_t>(k);
            const auto encode = [&] { return encode_section([&](std::size_t n) { return (*section)[n].raw(); }); };
            if (!cache) {
                if (auto bytes = encode(); !bytes.empty()) out.emplace_back(sy, std::move(bytes));
                continue;
            }
            auto [it, fresh] = cache->entries_.try_emplace(section.get());
            if (fresh) it->second = {section, encode(), 0};
            it->second.used = round;
            if (!it->second.bytes.empty()) out.emplace_back(sy, it->second.bytes);
        }
        return out;
    });
    // Sections no longer in the world leave the cache.
    if (cache)
        std::erase_if(cache->entries_, [&](auto& e) {
            if (e.second.used == round) return false;
            cache->dropped_.push_back(std::move(e.second.section));
            return true;
        });
    return seal(checkpoint, body.take(), options);
}


std::expected<Checkpoint, std::string> decode_checkpoint(std::span<const std::byte> bytes, const CheckpointLimits& limits) {
    if (bytes.size() > limits.max_bytes) return std::unexpected("checkpoint too large");
    if (bytes.size() < magic.size() + 4 + 32) return std::unexpected("checkpoint truncated");
    const auto content = bytes.first(bytes.size() - 32);
    const auto digest = sha256(content);
    if (!std::equal(digest.begin(), digest.end(), reinterpret_cast<const unsigned char*>(bytes.data() + content.size())))
        return std::unexpected("checkpoint digest mismatch");
    Reader r(content);
    const auto m = r.raw(magic.size());
    if (!m || !std::equal(m->begin(), m->end(), protocol::as_bytes(magic).begin())) return std::unexpected("not a checkpoint");
    const auto v = r.i32();
    if (!v || *v < 1 || *v > static_cast<std::int32_t>(version)) return std::unexpected("unsupported checkpoint version");
    Checkpoint c;
    const auto bundle = r.string(64), rules = r.string(128);
    std::string registry(version_4_registry);
    if (*v >= 5) {
        const auto named = r.string(64);
        if (!named) return std::unexpected("checkpoint header truncated");
        registry = *named;
    }
    // Block states are stored in the registry's encoding.
    if (registry != gameplay::registry::id())
        return std::unexpected(std::format("checkpoint uses block registry {}, not {}", registry, gameplay::registry::id()));
    const auto seed = r.i64(), tick = r.i64(), raw_size = r.i64(), compressed_size = r.i64();
    if (!bundle || !rules || !seed || !tick || !raw_size || !compressed_size) return std::unexpected("checkpoint header truncated");
    if (*raw_size < 0 || static_cast<std::uint64_t>(*raw_size) > limits.max_raw_bytes || *compressed_size < 0
        || static_cast<std::uint64_t>(*compressed_size) != r.remaining())
        return std::unexpected("checkpoint sizes do not match");
    c.bundle_id = *bundle;
    c.rules_id = *rules;
    c.seed = static_cast<std::uint64_t>(*seed);
    auto body = protocol::inflate_exact(r.rest(), static_cast<std::size_t>(*raw_size));
    if (!body) return std::unexpected("checkpoint body does not inflate");
    BodyReader b{Reader(*body), {}, *v};
    if (!decode_body(b, c)) return std::unexpected("checkpoint body: " + b.error);
    if (*v < 13) // no level random saved: each level's starts as in a new world
        for (auto& l : c.state.levels) l.random = gameplay::initial_level_random(c.seed, l.dimension, {}).state();
    if (c.state.time != static_cast<std::uint64_t>(*tick)) return std::unexpected("checkpoint tick mismatch");
    return c;
}

} // namespace circuit::storage
