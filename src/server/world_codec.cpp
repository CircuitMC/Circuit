// SPDX-License-Identifier: AGPL-3.0-only
#include "world_codec.hpp"
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/light.hpp>
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

void write_light(protocol::Writer& w, const ColumnLight* light, WorldShape shape, std::span<const std::int32_t> sections) {
    const auto count = shape.height / 16;
    const auto words = static_cast<std::size_t>((count + 2 + 63) / 64);
    std::vector<std::uint64_t> sky_mask(words), block_mask(words), sky_empty(words), block_empty(words);
    std::vector<const std::array<std::uint8_t, 2048>*> sky_arrays, block_arrays;
    static const std::array<std::uint8_t, 2048> full = [] {
        std::array<std::uint8_t, 2048> a;
        a.fill(0xFF);
        return a;
    }();
    const auto bit = [](std::vector<std::uint64_t>& mask, std::int32_t index) {
        mask[static_cast<std::size_t>(index / 64)] |= std::uint64_t{1} << (index % 64);
    };
    const auto dark = [](const std::array<std::uint8_t, 2048>& a) {
        return std::ranges::all_of(a, [](std::uint8_t v) { return v == 0; });
    };
    for (const auto s : sections) {
        const auto index = s + 1; // the light sections start one below the world
        if (s < 0) {
            bit(sky_empty, index);
            bit(block_empty, index);
        } else if (s >= count || !light) {
            bit(sky_mask, index);
            sky_arrays.push_back(&full);
            bit(block_empty, index);
        } else {
            const auto& l = (*light)[static_cast<std::size_t>(s)];
            if (dark(l.sky)) bit(sky_empty, index);
            else {
                bit(sky_mask, index);
                sky_arrays.push_back(&l.sky);
            }
            if (dark(l.block)) bit(block_empty, index);
            else {
                bit(block_mask, index);
                block_arrays.push_back(&l.block);
            }
        }
    }
    w.bitset(sky_mask).bitset(block_mask).bitset(sky_empty).bitset(block_empty);
    for (const auto* arrays : {&sky_arrays, &block_arrays}) {
        w.varint(static_cast<std::int32_t>(arrays->size()));
        for (const auto* a : *arrays) w.prefixed_bytes(std::as_bytes(std::span(*a)));
    }
}

} // namespace circuit::server
