// SPDX-License-Identifier: AGPL-3.0-only
#include "world_codec.hpp"
#include <algorithm>
#include <bit>
#include <format>
#include <stdexcept>

namespace circuit::server {
using contracts::BlockPos;
using contracts::Direction;
using gameplay::BlockType;

BlockPalette::BlockPalette(const protocol::ContentTables& tables, const evidence::ContentMapping& mapping) {
    for (std::uint32_t raw = 0; raw < (1u << 18); ++raw) {
        const auto state = BlockState::decode(raw);
        if (!state) continue;
        const auto id = tables.state_id(mapping.notation(*state));
        if (!id) throw std::runtime_error(std::format("server: no wire state for {}", mapping.notation(*state)));
        to_wire_.emplace(raw, *id);
        // The reverse map keeps the first reference state for each id; display
        // properties make several ids map to one state.
        from_wire_.try_emplace(*id, *state);
    }
    const auto base = tables.state_id("minecraft:redstone_wire[east=up,north=up,power=0,south=up,west=up]");
    if (!base) throw std::runtime_error("server: no redstone_wire states");
    dust_base_ = *base;
    // Every dust state, with every connection shape, maps back to its power.
    for (std::uint8_t power = 0; power < 16; ++power)
        for (int e = 0; e < 3; ++e)
            for (int n = 0; n < 3; ++n)
                for (int s = 0; s < 3; ++s)
                    for (int w = 0; w < 3; ++w)
                        from_wire_.try_emplace(dust(power, e, n, s, w), gameplay::blocks::wire(power));
}

std::int32_t BlockPalette::wire(BlockState state) const {
    const auto it = to_wire_.find(state.raw());
    return it == to_wire_.end() ? 0 : it->second;
}

std::int32_t BlockPalette::dust(std::uint8_t power, int east, int north, int south, int west) const {
    // Properties east, north, power, south, west; the last varies fastest.
    return dust_base_ + (((east * 3 + north) * 16 + power) * 3 + south) * 3 + west;
}

std::optional<BlockState> BlockPalette::reference(std::int32_t wire_id) const {
    const auto it = from_wire_.find(wire_id);
    if (it == from_wire_.end()) return std::nullopt;
    return it->second;
}

namespace {
constexpr int conn_up = 0, conn_side = 1, conn_none = 2;

// Whether dust visually connects to a non-dust neighbor in direction `d`.
bool connects(BlockState n, Direction d) {
    switch (n.type()) {
    case BlockType::lever: case BlockType::redstone_block: return true;
    case BlockType::repeater: case BlockType::comparator:
        return n.facing() == d || n.facing() == contracts::opposite(d);
    case BlockType::observer: return n.facing() == d; // its output (back) faces the dust
    default: return false;
    }
}

int dust_shape(const NeighborLookup& at, BlockPos pos, Direction d) {
    const auto side = pos.relative(d);
    const auto n = at(side);
    if (n.is(BlockType::redstone_wire) || connects(n, d)) return conn_side;
    const auto above = at(pos.relative(Direction::up));
    if (!gameplay::is_solid(above.type()) && at(side.relative(Direction::up)).is(BlockType::redstone_wire)) return conn_up;
    if (!gameplay::is_solid(n.type()) && at(side.relative(Direction::down)).is(BlockType::redstone_wire)) return conn_side;
    return conn_none;
}
} // namespace

std::int32_t display_state(const BlockPalette& palette, const NeighborLookup& at, BlockPos pos, BlockState s) {
    if (!s.is(BlockType::redstone_wire)) return palette.wire(s);
    int e = dust_shape(at, pos, Direction::east), n = dust_shape(at, pos, Direction::north);
    int so = dust_shape(at, pos, Direction::south), w = dust_shape(at, pos, Direction::west);
    const bool ew = e != conn_none || w != conn_none, ns = n != conn_none || so != conn_none;
    if (!ew && !ns) {
        e = n = so = w = conn_side; // An isolated dust is drawn as a cross.
    } else {
        // A single connection on an axis extends to the opposite side.
        if (e != conn_none && w == conn_none && !ns) w = conn_side;
        if (w != conn_none && e == conn_none && !ns) e = conn_side;
        if (n != conn_none && so == conn_none && !ew) so = conn_side;
        if (so != conn_none && n == conn_none && !ew) n = conn_side;
    }
    return palette.dust(s.level(), e, n, so, w);
}

std::vector<std::int64_t> pack_entries(std::span<const std::uint32_t> values, unsigned bits) {
    if (bits == 0) return {};
    const unsigned per_long = 64 / bits;
    std::vector<std::int64_t> out((values.size() + per_long - 1) / per_long);
    const std::uint64_t mask = bits == 64 ? ~0ULL : ((1ULL << bits) - 1);
    for (std::size_t i = 0; i < values.size(); ++i) {
        auto& word = reinterpret_cast<std::uint64_t&>(out[i / per_long]);
        word |= (static_cast<std::uint64_t>(values[i]) & mask) << ((i % per_long) * bits);
    }
    return out;
}

void encode_section(protocol::Writer& w, const std::array<std::int32_t, 4096>& states, std::int32_t biome_id,
                    std::int32_t state_bits_direct) {
    std::int16_t non_air = 0;
    std::vector<std::int32_t> palette;
    std::unordered_map<std::int32_t, std::uint32_t> index;
    for (const auto id : states) {
        if (id != 0) ++non_air; // State 0 is air; cave and void air are never produced.
        if (index.size() <= 256 && !index.contains(id)) {
            index.emplace(id, static_cast<std::uint32_t>(palette.size()));
            palette.push_back(id);
        }
    }
    w.i16(non_air);
    if (palette.size() == 1) {
        w.u8(0).varint(palette[0]); // single valued, no data array
    } else {
        std::array<std::uint32_t, 4096> values{};
        unsigned bits;
        if (palette.size() <= 256) {
            bits = std::max(4u, static_cast<unsigned>(std::bit_width(palette.size() - 1)));
            w.u8(static_cast<std::uint8_t>(bits)).varint(static_cast<std::int32_t>(palette.size()));
            for (const auto id : palette) w.varint(id);
            for (std::size_t i = 0; i < 4096; ++i) values[i] = index.at(states[i]);
        } else {
            bits = static_cast<unsigned>(state_bits_direct);
            w.u8(static_cast<std::uint8_t>(bits));
            for (std::size_t i = 0; i < 4096; ++i) values[i] = static_cast<std::uint32_t>(states[i]);
        }
        // Protocol 772 (1.21.5+): the data array length is implied.
        for (const auto v : pack_entries(values, bits)) w.i64(v);
    }
    w.u8(0).varint(biome_id); // one biome per section
}

Bytes encode_chunk(const runtime::ColumnSnapshot& column, const BlockPalette& palette, const NeighborLookup& at,
                   WorldShape shape, std::int32_t biome_id) {
    const auto sections = static_cast<std::size_t>(shape.height / 16);
    const std::int32_t direct_bits = 15; // ceil(log2(27946)) for the 1.21.8 palette
    const std::int32_t ox = column.pos.x * 16, oz = column.pos.z * 16;
    std::array<std::int32_t, 256> top{}; // highest occupied y index + 1 per column (x fastest)

    protocol::Writer data;
    for (std::size_t s = 0; s < sections; ++s) {
        std::array<std::int32_t, 4096> states{};
        const auto& blocks = s < column.sections.size() ? column.sections[s] : nullptr;
        if (blocks) {
            const auto base_y = shape.min_y + static_cast<std::int32_t>(s) * 16;
            for (std::size_t i = 0; i < 4096; ++i) {
                const auto st = (*blocks)[i];
                if (st.is_air()) continue;
                const BlockPos p{ox + static_cast<std::int32_t>(i & 15), base_y + static_cast<std::int32_t>(i >> 8),
                                 oz + static_cast<std::int32_t>((i >> 4) & 15)};
                // Neighbors inside this column come from the snapshot.
                const auto local = [&](BlockPos q) -> BlockState {
                    if ((q.x >> 4) == column.pos.x && (q.z >> 4) == column.pos.z && q.y >= shape.min_y
                        && q.y < shape.min_y + shape.height) {
                        const auto& sec = column.sections[static_cast<std::size_t>((q.y - shape.min_y) >> 4)];
                        return sec ? (*sec)[contracts::section_local_index(q)] : gameplay::blocks::air();
                    }
                    return at(q);
                };
                states[i] = display_state(palette, local, p, st);
                const auto column_index = i & 255;
                top[column_index] = std::max(top[column_index], static_cast<std::int32_t>(s * 16 + (i >> 8) + 1));
            }
        }
        encode_section(data, states, biome_id, direct_bits);
    }

    protocol::Writer w;
    w.i32(column.pos.x).i32(column.pos.z);
    // Heightmaps (1.21.5+): world surface (1) and motion blocking (4), 9 bits for 385 heights.
    const unsigned height_bits = static_cast<unsigned>(std::bit_width(static_cast<unsigned>(shape.height)));
    std::array<std::uint32_t, 256> heights{};
    for (std::size_t i = 0; i < 256; ++i) heights[i] = static_cast<std::uint32_t>(top[i]);
    const auto packed = pack_entries(heights, height_bits);
    w.varint(2);
    for (const std::int32_t type : {1, 4}) {
        w.varint(type).varint(static_cast<std::int32_t>(packed.size()));
        for (const auto v : packed) w.i64(v);
    }
    w.prefixed_bytes(data.bytes());
    w.varint(0); // block entities: the client creates them from block states
    // Light: sky light 15 everywhere, no block light, for every section plus one below and above.
    const auto light_sections = sections + 2;
    const std::uint64_t all = light_sections >= 64 ? ~0ULL : ((1ULL << light_sections) - 1);
    const std::array<std::uint64_t, 1> mask{all}, none{0};
    w.bitset(mask).bitset(none).bitset(none).bitset(mask);
    w.varint(static_cast<std::int32_t>(light_sections));
    const Bytes full(2048, std::byte{0xFF});
    for (std::size_t i = 0; i < light_sections; ++i) w.prefixed_bytes(full);
    w.varint(0);
    return w.take();
}

} // namespace circuit::server
