// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// circuit-trace fixtures run on the oracle (evidence::FixtureExecutor).
#include "oracle.hpp"
#include "../support/light_reference.hpp"
#include "../support/scenario.hpp"
#include <circuit/evidence/fixture_runner.hpp>
#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace circuit::oracle {

class OracleFixtureExecutor final : public evidence::FixtureExecutor {
public:
    // The overworld's level random starts from 0, as the reference harness
    // seeds it, and random sequences from the world seed 0 of its worlds.
    OracleFixtureExecutor(const contracts::VersionBundle& bundle, std::uint64_t seed, std::shared_ptr<const gameplay::GameData> data)
        : data_(std::move(data)), oracle_(bundle, seed, data_, std::int64_t{0}, std::int64_t{0}), shape_(bundle.world) {}
    const gameplay::GameData* data() const override { return data_.get(); }
    void advance(const std::vector<contracts::Command>& commands) override {
        before_ = std::move(dump_);
        oracle_.advance(commands);
        dump_ = oracle_.dump();
        light_.clear();
        world_.reset();
    }
    const std::vector<contracts::TraceRecord>& trace() const override { return oracle_.trace(); }
    gameplay::BlockState block(contracts::BlockPos pos) const override { return test::state_at(dump_, pos); }
    std::optional<contracts::ItemStack> slot(contracts::BlockPos pos, std::uint8_t slot) const override {
        const auto* be = test::block_entity_at(dump_, pos);
        if (!be || slot >= be->slots.size()) return std::nullopt;
        return be->slots[slot];
    }
    std::vector<gameplay::Entity> entities() const override { return dump_.overworld().entities; }
    // Light during a tick follows the blocks at the end of the one before: it
    // is computed from scratch from that dump.
    std::optional<std::pair<std::uint8_t, std::uint8_t>> light(contracts::BlockPos pos) const override {
        if (!world_) {
            world_.emplace(before_, contracts::DimensionId::overworld);
            top_ = std::min(shape_.min_y + shape_.height - 1, std::max(world_->highest(), shape_.min_y) + 16);
        }
        const auto chunk = contracts::chunk_of(pos);
        if (!world_->loaded(chunk)) return std::pair<std::uint8_t, std::uint8_t>{0, 0};
        if (pos.y > top_) return std::pair<std::uint8_t, std::uint8_t>{0, 15};
        // A column's light depends only on cells within 15 blocks: the
        // loaded columns around it.
        auto it = light_.find(chunk);
        if (it == light_.end()) {
            std::set<contracts::ChunkPos> around;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dz = -1; dz <= 1; ++dz)
                    if (world_->loaded({chunk.x + dx, chunk.z + dz})) around.insert({chunk.x + dx, chunk.z + dz});
            it = light_.try_emplace(chunk, *world_, around, shape_.min_y, top_).first;
        }
        using gameplay::LightKind;
        return std::pair{it->second.get(LightKind::block, pos), it->second.get(LightKind::sky, pos)};
    }

private:
    std::shared_ptr<const gameplay::GameData> data_;
    Oracle oracle_;
    contracts::WorldShape shape_;
    gameplay::WorldDump dump_, before_;
    mutable std::optional<test::DumpLightWorld> world_;
    mutable std::int32_t top_{};
    mutable std::map<contracts::ChunkPos, test::ReferenceLight> light_;
};

// `data`: the data packs' data the rules run with; none by default.
inline evidence::ExecutorFactory oracle_executor(evidence::DataSource data = {}) {
    return [data](const contracts::VersionBundle& b, std::uint64_t seed) -> std::unique_ptr<evidence::FixtureExecutor> {
        return std::make_unique<OracleFixtureExecutor>(b, seed, data ? data(b) : nullptr);
    };
}

} // namespace circuit::oracle
