// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/trace.hpp>
#include <circuit/gameplay/light.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

// Light computed from scratch, for checking the incremental light field:
// every cell of the given columns from `min_y` to `top`, with every emitter
// and the sky above `top` as sources. Exact for the whole world height when
// the cells above `top` are air and no emitter is within 15 of it.
namespace circuit::test {

class ReferenceLight {
public:
    ReferenceLight(const gameplay::LightWorld& world, std::set<contracts::ChunkPos> columns, std::int32_t min_y, std::int32_t top)
        : columns_(std::move(columns)), min_y_(min_y), top_(top) {
        const auto cells = static_cast<std::size_t>(256 * (top - min_y + 1));
        for (const auto c : columns_)
            for (auto& kind : levels_) kind[contracts::pack_chunk(c)].assign(cells, 0);
        using gameplay::LightKind;
        for (const auto kind : {LightKind::block, LightKind::sky}) {
            std::array<std::vector<contracts::BlockPos>, 16> buckets;
            for (const auto c : columns_)
                for (auto y = min_y_; y <= top_; ++y)
                    for (int i = 0; i < 256; ++i) {
                        const contracts::BlockPos p{c.x * 16 + (i & 15), y, c.z * 16 + (i >> 4)};
                        const auto info = gameplay::light_info(world.block(p));
                        const auto own = kind == LightKind::block ? info.emission
                                       : y == top_ ? gameplay::light_passed(kind, 15, {}, info, contracts::Direction::down)
                                                   : std::uint8_t{0};
                        if (own > 0) {
                            at(kind, p) = own;
                            buckets[own].push_back(p);
                        }
                    }
            for (int level = 15; level > 0; --level)
                while (!buckets[static_cast<std::size_t>(level)].empty()) {
                    const auto q = buckets[static_cast<std::size_t>(level)].back();
                    buckets[static_cast<std::size_t>(level)].pop_back();
                    if (at(kind, q) != level) continue;
                    const auto from = gameplay::light_info(world.block(q));
                    for (const auto d : {contracts::Direction::down, contracts::Direction::up, contracts::Direction::north,
                                         contracts::Direction::south, contracts::Direction::west, contracts::Direction::east}) {
                        const auto n = q.relative(d);
                        if (!inside(n)) continue;
                        const auto give = gameplay::light_passed(kind, static_cast<std::uint8_t>(level), from,
                                                                 gameplay::light_info(world.block(n)), d);
                        if (give > at(kind, n)) {
                            at(kind, n) = give;
                            buckets[give].push_back(n);
                        }
                    }
                }
        }
    }

    [[nodiscard]] bool inside(contracts::BlockPos p) const {
        return p.y >= min_y_ && p.y <= top_ && columns_.contains(contracts::chunk_of(p));
    }
    [[nodiscard]] std::uint8_t get(gameplay::LightKind kind, contracts::BlockPos p) const {
        if (!inside(p)) return 0;
        return levels_[static_cast<std::size_t>(kind)].at(contracts::pack_chunk(contracts::chunk_of(p)))[index(p)];
    }

private:
    [[nodiscard]] std::size_t index(contracts::BlockPos p) const {
        return static_cast<std::size_t>((p.y - min_y_) * 256 + (p.z & 15) * 16 + (p.x & 15));
    }
    std::uint8_t& at(gameplay::LightKind kind, contracts::BlockPos p) {
        return levels_[static_cast<std::size_t>(kind)].at(contracts::pack_chunk(contracts::chunk_of(p)))[index(p)];
    }

    std::set<contracts::ChunkPos> columns_;
    std::int32_t min_y_, top_;
    std::array<std::unordered_map<std::uint64_t, std::vector<std::uint8_t>>, 2> levels_;
};

// The blocks of one level of a dump, for light.
class DumpLightWorld final : public gameplay::LightWorld {
public:
    DumpLightWorld(const gameplay::WorldDump& dump, contracts::DimensionId level) {
        const auto& l = dump.level(level);
        for (const auto& c : l.chunks)
            if (c.level != gameplay::ChunkLevel::unloaded) loaded_.insert(c.pos);
        for (const auto& [p, raw] : l.blocks)
            if (loaded_.contains(contracts::chunk_of(p))) {
                blocks_[p] = gameplay::BlockState::decode(raw).value();
                highest_ = std::max(highest_, p.y);
            }
    }
    gameplay::BlockState block(contracts::BlockPos p) const override {
        if (!loaded_.contains(contracts::chunk_of(p))) return gameplay::BlockState::unloaded_sentinel();
        const auto it = blocks_.find(p);
        return it == blocks_.end() ? gameplay::blocks::air() : it->second;
    }
    bool loaded(contracts::ChunkPos c) const override { return loaded_.contains(c); }
    [[nodiscard]] const std::set<contracts::ChunkPos>& columns() const { return loaded_; }
    // The highest non-air block; light above it plus 16 is the sky's alone.
    [[nodiscard]] std::int32_t highest() const { return highest_; }

private:
    std::set<contracts::ChunkPos> loaded_;
    std::map<contracts::BlockPos, gameplay::BlockState> blocks_;
    std::int32_t highest_{INT32_MIN / 2};
};

} // namespace circuit::test
