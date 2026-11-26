// SPDX-License-Identifier: AGPL-3.0-only
#include "oracle_engine.hpp"
#include "../support/light_reference.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace circuit::oracle {
namespace {
using contracts::BlockPos;
using contracts::ChunkPos;
using contracts::DimensionId;
using contracts::ErrorCode;

const gameplay::LevelDump* find_level(const gameplay::WorldDump& d, DimensionId level) {
    for (const auto& l : d.levels)
        if (l.dimension == level) return &l;
    return nullptr;
}

class OracleEngine final : public runtime::Engine {
public:
    OracleEngine(const contracts::VersionBundle& bundle, std::uint64_t seed, const runtime::EngineOptions& options,
                 const gameplay::WorldDump* state)
        : bundle_(bundle),
          oracle_(state ? std::make_unique<Oracle>(bundle, seed, *state, options.data, options.level_random_seed, options.sequence_seed)
                        : std::make_unique<Oracle>(bundle, seed, options.data, options.level_random_seed, options.sequence_seed)),
          after_(oracle_->dump()), before_(after_) {}

    void advance(const std::vector<contracts::Command>& commands) override {
        oracle_->advance(commands);
        before_ = std::move(after_);
        after_ = oracle_->dump();
        feed_ = runtime::changes_between(before_, after_);
        light_.clear();
    }
    contracts::Tick time() const override { return oracle_->time(); }
    const std::vector<contracts::TraceRecord>& trace() const override { return oracle_->trace(); }
    gameplay::WorldDump dump() const override { return after_; }
    const runtime::ChangeFeed& changes() const override { return feed_; }

    contracts::Result<gameplay::BlockState> block(DimensionId level, BlockPos pos) const override {
        const auto& shape = bundle_.shape(level);
        if (!shape.contains_y(pos.y)) return std::unexpected(ErrorCode::out_of_bounds);
        const auto* l = find_level(after_, level);
        if (!l || !loaded(*l, contracts::chunk_of(pos))) return std::unexpected(ErrorCode::unloaded_chunk);
        const auto it = std::ranges::lower_bound(l->blocks, pos, {}, [](const auto& e) { return e.first; });
        if (it == l->blocks.end() || it->first != pos) return gameplay::blocks::air();
        return *gameplay::BlockState::decode(it->second);
    }
    contracts::Result<std::optional<gameplay::BlockEntity>> block_entity(DimensionId level, BlockPos pos) const override {
        const auto& shape = bundle_.shape(level);
        if (!shape.contains_y(pos.y)) return std::unexpected(ErrorCode::out_of_bounds);
        const auto* l = find_level(after_, level);
        if (!l || !loaded(*l, contracts::chunk_of(pos))) return std::unexpected(ErrorCode::unloaded_chunk);
        const auto it = std::ranges::lower_bound(l->block_entities, pos, {}, [](const auto& e) { return e.first; });
        if (it == l->block_entities.end() || it->first != pos) return std::optional<gameplay::BlockEntity>{};
        return std::optional(it->second);
    }
    std::optional<gameplay::Entity> entity(DimensionId level, contracts::EntityId id) const override {
        const auto* l = find_level(after_, level);
        if (!l) return std::nullopt;
        const auto it = std::ranges::lower_bound(l->entities, id, {}, &gameplay::Entity::id);
        if (it == l->entities.end() || it->id != id || it->returns != 0) return std::nullopt;
        return *it;
    }
    // From the blocks at the end of the tick before the last, from scratch
    // around the column read (a column's light depends only on the columns
    // around it).
    std::pair<std::uint8_t, std::uint8_t> light(DimensionId level, BlockPos pos) const override {
        auto& cache = light_[level];
        if (!cache.world) {
            cache.world.emplace(before_, level);
            const auto& shape = bundle_.shape(level);
            cache.top = std::min(shape.min_y + shape.height - 1, std::max(cache.world->highest(), shape.min_y) + 16);
        }
        const auto chunk = contracts::chunk_of(pos);
        if (!cache.world->loaded(chunk)) return {0, 0};
        if (pos.y > cache.top) return {0, 15};
        auto it = cache.columns.find(chunk);
        if (it == cache.columns.end()) {
            std::set<ChunkPos> around;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dz = -1; dz <= 1; ++dz)
                    if (cache.world->loaded({chunk.x + dx, chunk.z + dz})) around.insert({chunk.x + dx, chunk.z + dz});
            it = cache.columns.try_emplace(chunk, *cache.world, around, bundle_.shape(level).min_y, cache.top).first;
        }
        return {it->second.get(gameplay::LightKind::block, pos), it->second.get(gameplay::LightKind::sky, pos)};
    }

private:
    static bool loaded(const gameplay::LevelDump& l, ChunkPos chunk) {
        const auto it = std::ranges::lower_bound(l.chunks, chunk, {}, &gameplay::LevelDump::Chunk::pos);
        return it != l.chunks.end() && it->pos == chunk && it->level != gameplay::ChunkLevel::unloaded;
    }
    struct LightCache {
        std::optional<test::DumpLightWorld> world;
        std::int32_t top{};
        std::map<ChunkPos, test::ReferenceLight> columns;
    };

    const contracts::VersionBundle& bundle_;
    std::unique_ptr<Oracle> oracle_;
    gameplay::WorldDump after_, before_;
    runtime::ChangeFeed feed_;
    mutable std::map<DimensionId, LightCache> light_;
};
} // namespace

runtime::EngineFactory oracle_engine_factory() {
    return [](const contracts::VersionBundle& bundle, std::uint64_t seed, const runtime::EngineOptions& options,
              const gameplay::WorldDump* state) -> std::unique_ptr<runtime::Engine> {
        return std::make_unique<OracleEngine>(bundle, seed, options, state);
    };
}

} // namespace circuit::oracle
