// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/evidence/content.hpp>
#include <circuit/protocol/codec.hpp>
#include <circuit/protocol/content.hpp>
#include <circuit/runtime/engine.hpp>
#include <functional>
#include <unordered_map>

// Translation of Circuit world state to wire data: block-state ids of the
// global palette and chunk packets. Display-only properties that the reference
// rules do not model (dust connections, lever mounting) are derived here and
// never feed back into the simulation.
namespace circuit::server {

using gameplay::BlockState;
using protocol::Bytes;

class BlockPalette {
public:
    // Maps every valid reference state through the target version's notation.
    BlockPalette(const protocol::ContentTables& tables, const evidence::ContentMapping& mapping);

    [[nodiscard]] std::int32_t wire(BlockState state) const;
    // Dust with display connections: each value is 0 up, 1 side, 2 none.
    [[nodiscard]] std::int32_t dust(std::uint8_t power, int east, int north, int south, int west) const;
    // Reference state for a target-version state id, when the rules model it.
    [[nodiscard]] std::optional<BlockState> reference(std::int32_t wire_id) const;
    [[nodiscard]] std::size_t size() const noexcept { return to_wire_.size(); }

private:
    std::unordered_map<std::uint32_t, std::int32_t> to_wire_;
    std::unordered_map<std::int32_t, BlockState> from_wire_;
    std::int32_t dust_base_{};
};

// Block lookup outside a column (for display properties at column edges).
using NeighborLookup = std::function<BlockState(contracts::BlockPos)>;

// Wire id of the block at `pos`, with display properties derived from its
// neighbors.
std::int32_t display_state(const BlockPalette& palette, const NeighborLookup& at, contracts::BlockPos pos, BlockState s);

struct WorldShape {
    std::int32_t min_y;
    std::int32_t height;
};

// Fields of a level_chunk_with_light packet after its id: coordinates,
// heightmaps, section data, block entities and light. The world is lit fully
// by sky light; Circuit has no lighting engine yet. Protocol 772 (1.21.5+)
// sends heightmaps as a typed list and implies data array lengths; protocol
// 769 (1.21.4) sends heightmaps as NBT and prefixes data arrays with their
// length.
Bytes encode_chunk(const runtime::ColumnSnapshot& column, const BlockPalette& palette, const NeighborLookup& at,
                   WorldShape shape, std::int32_t biome_id, std::int32_t protocol);

// Section data of one 16x16x16 section as sent in chunk packets (block count,
// block states, biomes), exposed for tests.
void encode_section(protocol::Writer& w, const std::array<std::int32_t, 4096>& states, std::int32_t biome_id,
                    std::int32_t state_bits_direct, bool length_prefixed);

// Packs `values` with `bits` bits each, no entry spanning two longs.
std::vector<std::int64_t> pack_entries(std::span<const std::uint32_t> values, unsigned bits);

} // namespace circuit::server
