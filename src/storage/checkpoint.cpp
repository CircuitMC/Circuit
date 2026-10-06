// SPDX-License-Identifier: AGPL-3.0-only
#include <bit>
#include <cstdlib>
#include <circuit/protocol/codec.hpp>
#include <circuit/protocol/framing.hpp>
#include <circuit/storage/checkpoint.hpp>
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
// 3: hopper block entities carry the tick they last ticked; 2: tickets carry a
// load radius. Versions 1 and 2 are still read.
constexpr std::uint32_t version = 3;
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

// Writes the column table; `blocks(i)` gives column i's non-empty sections as
// (section y, encoding) in ascending y.
template <class Blocks>
void encode_body(Writer& w, const Checkpoint& c, Blocks blocks) {
    const auto& s = c.state;
    w.varlong(static_cast<std::int64_t>(s.time)).varlong(static_cast<std::int64_t>(s.next_suborder));
    w.varlong(static_cast<std::int64_t>(s.next_entity_id)).varlong(static_cast<std::int64_t>(s.next_block_entity_order));
    w.varint(static_cast<std::int32_t>(s.chunks.size()));
    for (std::size_t i = 0; i < s.chunks.size(); ++i) {
        const auto& ch = s.chunks[i];
        w.i32(ch.pos.x).i32(ch.pos.z).varint(static_cast<std::int32_t>(ch.generation)).u8(static_cast<std::uint8_t>(ch.level));
        const auto sections = blocks(i);
        w.varint(static_cast<std::int32_t>(sections.size()));
        for (const auto& [sy, bytes] : sections) w.i32(sy).raw(bytes);
    }
    w.varint(static_cast<std::int32_t>(s.block_entities.size()));
    for (const auto& [p, be] : s.block_entities) {
        pos(w, p);
        w.u8(static_cast<std::uint8_t>(be.kind)).varlong(static_cast<std::int64_t>(be.order)).varint(static_cast<std::int32_t>(be.slots.size()));
        for (const auto& st : be.slots) w.u16(st.item).u8(st.count);
        w.u8(be.cooldown).u8(be.signal).i32(static_cast<std::int32_t>(be.moving)).u8(be.progress).u8(be.motion);
        w.varlong(static_cast<std::int64_t>(be.ticked));
    }
    w.varint(static_cast<std::int32_t>(s.entities.size()));
    for (const auto& e : s.entities) {
        w.varlong(static_cast<std::int64_t>(e.id.value));
        w.i64(e.position.x).i64(e.position.y).i64(e.position.z).i64(e.velocity.x).i64(e.velocity.y).i64(e.velocity.z);
        w.u16(e.stack.item).u8(e.stack.count).varint(static_cast<std::int32_t>(e.age)).varlong(static_cast<std::int64_t>(e.last_tick));
        w.boolean(e.on_ground);
    }
    w.varint(static_cast<std::int32_t>(s.ticks.size()));
    for (const auto& t : s.ticks) {
        w.u8(static_cast<std::uint8_t>(t.list));
        pos(w, t.tick.pos);
        w.u8(static_cast<std::uint8_t>(t.tick.block)).varlong(static_cast<std::int64_t>(t.tick.due)).i8(t.tick.priority);
        w.varlong(static_cast<std::int64_t>(t.tick.suborder));
    }
    w.varint(static_cast<std::int32_t>(s.block_events.size()));
    for (const auto& e : s.block_events) {
        pos(w, e.pos);
        w.u8(static_cast<std::uint8_t>(e.block)).u8(e.event).u8(e.param);
    }
    w.varint(static_cast<std::int32_t>(s.tickets.size()));
    for (const auto& t : s.tickets) w.i32(static_cast<std::int32_t>(t.id)).i32(t.center.x).i32(t.center.z).u8(t.radius).u8(t.load_radius);
    w.varint(static_cast<std::int32_t>(c.extras.size()));
    for (const auto& [name, bytes] : c.extras) w.string(name, 256).prefixed_bytes(bytes);
}

struct BodyReader {
    Reader r;
    std::string error;
    std::int32_t version{};

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
    bool count(std::size_t& out, std::size_t min_bytes) {
        return get(r.length(std::size_t{1} << 28, min_bytes), out);
    }
    bool pos(contracts::BlockPos& p) { return get(r.i32(), p.x) && get(r.i32(), p.y) && get(r.i32(), p.z); }
};

bool decode_body(BodyReader& b, Checkpoint& c) {
    auto& s = c.state;
    if (!b.u64(s.time) || !b.u64(s.next_suborder) || !b.u64(s.next_entity_id) || !b.u64(s.next_block_entity_order)) return false;
    std::size_t chunks = 0;
    if (!b.count(chunks, 11)) return false;
    for (std::size_t i = 0; i < chunks; ++i) {
        WorldDump::Chunk ch;
        std::int32_t generation{};
        std::uint8_t level{};
        std::size_t secs = 0;
        if (!b.get(b.r.i32(), ch.pos.x) || !b.get(b.r.i32(), ch.pos.z) || !b.get(b.r.varint(), generation) || !b.get(b.r.u8(), level)
            || !b.count(secs, 6))
            return false;
        // Column and section positions stay within the coordinates block
        // positions can express; the engine checks them against the world.
        if (level > 2 || generation < 0 || std::abs(ch.pos.x) > max_chunk || std::abs(ch.pos.z) > max_chunk) {
            b.error = "invalid column";
            return false;
        }
        ch.generation = static_cast<std::uint32_t>(generation);
        ch.level = static_cast<gameplay::ChunkLevel>(level);
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
            if (!b.get(b.r.u16(), st.item) || !b.get(b.r.u8(), st.count)) return false;
        std::int32_t moving{};
        if (!b.get(b.r.u8(), be.cooldown) || !b.get(b.r.u8(), be.signal) || !b.get(b.r.i32(), moving) || !b.get(b.r.u8(), be.progress)
            || !b.get(b.r.u8(), be.motion))
            return false;
        if (b.version >= 3) {
            std::int64_t ticked{};
            if (!b.get(b.r.varlong(), ticked) || ticked < 0) return false;
            be.ticked = static_cast<gameplay::Tick>(ticked);
        }
        be.moving = static_cast<std::uint32_t>(moving);
        s.block_entities.push_back({p, std::move(be)});
    }
    if (!b.count(n, 55)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        gameplay::Entity e;
        std::int32_t age{};
        if (!b.u64(e.id.value) || !b.get(b.r.i64(), e.position.x) || !b.get(b.r.i64(), e.position.y) || !b.get(b.r.i64(), e.position.z)
            || !b.get(b.r.i64(), e.velocity.x) || !b.get(b.r.i64(), e.velocity.y) || !b.get(b.r.i64(), e.velocity.z)
            || !b.get(b.r.u16(), e.stack.item) || !b.get(b.r.u8(), e.stack.count) || !b.get(b.r.varint(), age) || !b.u64(e.last_tick)
            || !b.get(b.r.boolean(), e.on_ground))
            return false;
        e.age = static_cast<std::uint32_t>(age);
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
    if (!b.count(n, 13)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        gameplay::Ticket t;
        std::int32_t id{};
        if (!b.get(b.r.i32(), id) || !b.get(b.r.i32(), t.center.x) || !b.get(b.r.i32(), t.center.z) || !b.get(b.r.u8(), t.radius)) return false;
        if (b.version >= 2 && !b.get(b.r.u8(), t.load_radius)) return false;
        t.id = static_cast<std::uint32_t>(id);
        s.tickets.push_back(t);
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
    std::map<contracts::ChunkPos, std::map<std::int32_t, std::array<std::uint32_t, 4096>>> sections;
    for (const auto& [p, raw] : checkpoint.state.blocks) sections[contracts::chunk_of(p)][p.y >> 4][contracts::section_local_index(p)] = raw;
    Writer body;
    encode_body(body, checkpoint, [&](std::size_t i) {
        std::vector<std::pair<std::int32_t, std::vector<std::byte>>> out;
        if (const auto it = sections.find(checkpoint.state.chunks[i].pos); it != sections.end())
            for (const auto& [sy, values] : it->second)
                if (auto bytes = encode_section([&](std::size_t k) { return values[k]; }); !bytes.empty()) out.emplace_back(sy, std::move(bytes));
        return out;
    });
    return body.take();
}

std::vector<std::byte> seal(const Checkpoint& checkpoint, const std::vector<std::byte>& body, const CheckpointOptions& options) {
    const auto compressed = protocol::deflate_bytes(body, options.compression_level);
    Writer w;
    w.raw(protocol::as_bytes(magic)).i32(static_cast<std::int32_t>(version));
    w.string(checkpoint.bundle_id, 64).string(checkpoint.rules_id, 128).i64(static_cast<std::int64_t>(checkpoint.seed));
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

std::vector<std::byte> encode_checkpoint(const Checkpoint& checkpoint, std::span<const ColumnSections> columns, std::int32_t min_y,
                                         SectionCache* cache, const CheckpointOptions& options) {
    if (!checkpoint.state.blocks.empty() || columns.size() != checkpoint.state.chunks.size())
        throw std::invalid_argument("checkpoint: sections must replace the block list, one entry per column");
    const auto round = cache ? ++cache->round_ : 0;
    Writer body;
    encode_body(body, checkpoint, [&](std::size_t i) {
        std::vector<std::pair<std::int32_t, std::vector<std::byte>>> out;
        const auto& list = columns[i];
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
    if (c.state.time != static_cast<std::uint64_t>(*tick)) return std::unexpected("checkpoint tick mismatch");
    return c;
}

} // namespace circuit::storage
