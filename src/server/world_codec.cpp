// SPDX-License-Identifier: AGPL-3.0-only
#include "world_codec.hpp"
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/protocol/nbt.hpp>
#include <algorithm>
#include <bit>
#include <format>
#include <stdexcept>

namespace circuit::server {
using contracts::BlockPos;
using contracts::Direction;
using gameplay::BlockType;

BlockPalette::BlockPalette(const protocol::ContentTables& tables, const evidence::ContentMapping& mapping)
    : release_(mapping.game_version()) {
    if (gameplay::registry::wire_state_count(release_) != static_cast<std::uint32_t>(tables.state_count()))
        throw std::runtime_error(std::format("server: the block registry and the content table of {} disagree", release_));
    // Modeled states go through their notation; registry states map directly.
    gameplay::for_each_modeled_state([&](BlockState state) {
        const auto id = tables.state_id(mapping.notation(state));
        if (!id) throw std::runtime_error(std::format("server: no wire state for {}", mapping.notation(state)));
        if (!from_wire_.emplace(*id, state).second)
            throw std::runtime_error(std::format("server: two states map to {}", mapping.notation(state)));
        to_wire_.emplace(state.raw(), *id);
    });
}

std::int32_t BlockPalette::wire(BlockState state) const {
    if (state.is(BlockType::other)) return gameplay::registry::to_wire(release_, state.registry_state()).value_or(0);
    const auto it = to_wire_.find(state.raw());
    return it == to_wire_.end() ? 0 : it->second;
}

std::optional<BlockState> BlockPalette::reference(std::int32_t wire_id) const {
    if (const auto it = from_wire_.find(wire_id); it != from_wire_.end()) return it->second;
    const auto internal = gameplay::registry::from_wire(release_, wire_id);
    if (!internal) return std::nullopt;
    const auto s = BlockState::decode(BlockState::registered(*internal).raw());
    return s ? std::optional(*s) : std::nullopt;
}

std::int32_t display_state(const BlockPalette& palette, const NeighborLookup&, BlockPos, BlockState s) {
    // Dust keeps its shape as the rules store it; lever mounting is part of
    // the state. Nothing is derived for display any more.
    return palette.wire(s);
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
                    std::int32_t state_bits_direct, bool length_prefixed, std::int32_t non_air_blocks) {
    std::int16_t non_air = 0;
    std::vector<std::int32_t> palette;
    std::unordered_map<std::int32_t, std::uint32_t> index;
    for (const auto id : states) {
        if (id != 0) ++non_air;
        if (index.size() <= 256 && !index.contains(id)) {
            index.emplace(id, static_cast<std::uint32_t>(palette.size()));
            palette.push_back(id);
        }
    }
    w.i16(non_air_blocks >= 0 ? static_cast<std::int16_t>(non_air_blocks) : non_air);
    if (palette.size() == 1) {
        w.u8(0).varint(palette[0]); // single valued, no data array
        if (length_prefixed) w.varint(0);
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
        const auto longs = pack_entries(values, bits);
        if (length_prefixed) w.varint(static_cast<std::int32_t>(longs.size()));
        for (const auto v : longs) w.i64(v);
    }
    w.u8(0).varint(biome_id); // one biome per section
    if (length_prefixed) w.varint(0);
}

Bytes encode_chunk(const runtime::ColumnSnapshot& column, const BlockPalette& palette, const NeighborLookup& at,
                   WorldShape shape, std::int32_t biome_id, std::int32_t protocol) {
    const auto sections = static_cast<std::size_t>(shape.height / 16);
    const bool modern = protocol >= 770;
    const std::int32_t direct_bits = 15; // ceil(log2(state count)): 27946 states (1.21.8), 27866 (1.21.4)
    const std::int32_t ox = column.pos.x * 16, oz = column.pos.z * 16;
    std::array<std::int32_t, 256> top{}; // highest occupied y index + 1 per column (x fastest)

    protocol::Writer data;
    for (std::size_t s = 0; s < sections; ++s) {
        std::array<std::int32_t, 4096> states{};
        std::int32_t non_air = 0;
        const auto& blocks = s < column.sections.size() ? column.sections[s] : nullptr;
        if (blocks) {
            const auto base_y = shape.min_y + static_cast<std::int32_t>(s) * 16;
            for (std::size_t i = 0; i < 4096; ++i) {
                const auto st = (*blocks)[i];
                if (st == gameplay::blocks::air()) continue;
                if (st.is_air()) {
                    states[i] = palette.wire(st); // cave and void air: sent, but not counted or in heightmaps
                    continue;
                }
                ++non_air;
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
        encode_section(data, states, biome_id, direct_bits, !modern, non_air);
    }

    protocol::Writer w;
    w.i32(column.pos.x).i32(column.pos.z);
    // Heightmaps: world surface and motion blocking, 9 bits for 385 heights.
    const unsigned height_bits = static_cast<unsigned>(std::bit_width(static_cast<unsigned>(shape.height)));
    std::array<std::uint32_t, 256> heights{};
    for (std::size_t i = 0; i < 256; ++i) heights[i] = static_cast<std::uint32_t>(top[i]);
    const auto packed = pack_entries(heights, height_bits);
    if (modern) {
        // 1.21.5+: a list of (type, longs); 1 is WORLD_SURFACE, 4 MOTION_BLOCKING.
        w.varint(2);
        for (const std::int32_t type : {1, 4}) {
            w.varint(type).varint(static_cast<std::int32_t>(packed.size()));
            for (const auto v : packed) w.i64(v);
        }
    } else {
        // 1.21.4: a network NBT compound of long arrays named by heightmap.
        auto maps = protocol::nbt::Tag::compound();
        std::vector<std::int64_t> longs(packed.begin(), packed.end());
        maps.set("MOTION_BLOCKING", protocol::nbt::Tag::long_array(longs));
        maps.set("WORLD_SURFACE", protocol::nbt::Tag::long_array(std::move(longs)));
        protocol::nbt::write_network(w, maps);
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
