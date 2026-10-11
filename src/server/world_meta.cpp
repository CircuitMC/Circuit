// SPDX-License-Identifier: AGPL-3.0-only
#include "world_meta.hpp"
#include <circuit/protocol/codec.hpp>

namespace circuit::server {

WorldMeta fresh_meta(std::int32_t floor_y) { return {floor_y, {0, floor_y, 0}, 1, 1000, "none", true}; }

// Version 3 marks typed tickets; version 2 adds the data fingerprint; version
// 1 worlds ran without data packs.
std::vector<std::byte> encode_meta(const WorldMeta& m) {
    protocol::Writer w;
    w.varint(3).i32(m.floor_y).i32(m.spawn.x).i32(m.spawn.y).i32(m.spawn.z);
    w.i64(static_cast<std::int64_t>(m.next_admission)).i32(static_cast<std::int32_t>(m.next_ticket));
    w.string(m.data, 64);
    return w.take();
}

std::optional<WorldMeta> decode_meta(std::span<const std::byte> bytes) {
    protocol::Reader r(bytes);
    const auto version = r.varint();
    const auto floor = r.i32();
    const auto x = r.i32(), y = r.i32(), z = r.i32();
    const auto admission = r.i64();
    const auto ticket = r.i32();
    if (!version || *version < 1 || *version > 3 || !floor || !x || !y || !z || !admission || *admission < 1 || !ticket || *ticket < 2)
        return std::nullopt;
    std::string data = "none";
    if (*version >= 2) {
        auto d = r.string(64);
        if (!d) return std::nullopt;
        data = std::move(*d);
    }
    if (r.remaining() != 0) return std::nullopt;
    return WorldMeta{*floor, {*x, *y, *z}, static_cast<std::uint64_t>(*admission), static_cast<std::uint32_t>(*ticket), std::move(data),
                     *version >= 3};
}

runtime::EngineOptions world_engine_options(const contracts::VersionBundle& bundle, std::int32_t floor_y, unsigned workers) {
    runtime::EngineOptions o;
    o.workers = workers;
    const auto min_y = bundle.world.min_y;
    if (bundle.world.contains_y(floor_y)) {
        // The overworld is a flat stone floor; no other level is generated.
        o.generator = [floor_y, min_y](contracts::ChunkKey key, const auto& blocks) {
            if (key.dimension != contracts::DimensionId::overworld) return;
            auto& section = blocks(static_cast<std::size_t>((floor_y - min_y) >> 4));
            const auto y = static_cast<std::size_t>((floor_y - min_y) & 15);
            for (std::size_t i = 0; i < 256; ++i) section[y * 256 + i] = gameplay::blocks::stone();
        };
    }
    return o;
}

} // namespace circuit::server
