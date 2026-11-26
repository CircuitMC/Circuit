// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/evidence/content.hpp>
#include <circuit/protocol/codec.hpp>
#include <circuit/protocol/content.hpp>
#include <array>
#include <span>
#include <vector>
#include <functional>
#include <unordered_map>

// Translation of Circuit world state to wire data: block-state ids of the
// global palette and chunk packets. Every displayed property is part of the
// rule state (dust shapes, lever mounting, repeater locks).
namespace circuit::server {

using gameplay::BlockState;
using protocol::Bytes;

class BlockPalette {
public:
    // Maps the modeled states through the target version's notation and the
    // registry states through the registry's wire table.
    BlockPalette(const protocol::ContentTables& tables, const evidence::ContentMapping& mapping);

    [[nodiscard]] std::int32_t wire(BlockState state) const;
    // The state of a target-version state id.
    [[nodiscard]] std::optional<BlockState> reference(std::int32_t wire_id) const;

private:
    std::string_view release_;
    std::unordered_map<std::uint32_t, std::int32_t> to_wire_;
    std::unordered_map<std::int32_t, BlockState> from_wire_;
};

// Block lookup outside a column (for display properties at column edges).
using NeighborLookup = std::function<BlockState(contracts::BlockPos)>;

// Wire id of the block at `pos`.
std::int32_t display_state(const BlockPalette& palette, const NeighborLookup& at, contracts::BlockPos pos, BlockState s);

struct WorldShape {
    std::int32_t min_y;
    std::int32_t height;
};

// The light of one section as the protocol packs it (gameplay::LightField::section).
struct SectionLight {
    std::array<std::uint8_t, 2048> block{}, sky{};
};
// A column's light, per section from the lowest.
using ColumnLight = std::vector<SectionLight>;

// Light data as chunk packets and light updates carry it, for the sections
// `sections` (from the lowest; -1 and the count are the ones below and above
// the world): each section's sky and block light, or a mark that it is all
// dark. Below the world nothing is lit; above it, sky light is full. Without
// a column's light (not computed), every section has full sky light.
void write_light(protocol::Writer& w, const ColumnLight* light, WorldShape shape, std::span<const std::int32_t> sections);

// Section data of one 16x16x16 section as sent in chunk packets (block count,
// block states, biomes), exposed for tests. The block count excludes every
// kind of air; without `non_air_blocks` it counts the states other than 0.
void encode_section(protocol::Writer& w, const std::array<std::int32_t, 4096>& states, std::int32_t biome_id,
                    std::int32_t state_bits_direct, bool length_prefixed, std::int32_t non_air_blocks = -1);

// Packs `values` with `bits` bits each, no entry spanning two longs.
std::vector<std::int64_t> pack_entries(std::span<const std::uint32_t> values, unsigned bits);

} // namespace circuit::server
