// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/runtime/engine.hpp>
#include <map>

namespace circuit::runtime {
namespace {
using contracts::BlockPos;
using contracts::DimensionId;

// Walks two sequences sorted by `key` and calls `differs(key)` for every key
// present in only one of them or with unequal values.
template <class T, class Key, class Out>
void diff_sorted(const std::vector<T>& a, const std::vector<T>& b, Key key, Out out) {
    std::size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if (j == b.size() || (i < a.size() && key(a[i]) < key(b[j]))) out(key(a[i++]));
        else if (i == a.size() || key(b[j]) < key(a[i])) out(key(b[j++]));
        else {
            if (!(a[i] == b[j])) out(key(a[i]));
            ++i;
            ++j;
        }
    }
}

const gameplay::LevelDump& level_or_empty(const gameplay::WorldDump& d, DimensionId level) {
    static const gameplay::LevelDump empty{};
    for (const auto& l : d.levels)
        if (l.dimension == level) return l;
    return empty;
}
} // namespace

ChangeFeed changes_between(const gameplay::WorldDump& before, const gameplay::WorldDump& after) {
    ChangeFeed feed;
    for (const auto level : contracts::level_order) {
        const auto& a = level_or_empty(before, level);
        const auto& b = level_or_empty(after, level);
        // Blocks: absent means air. A block that disappears from the list
        // became air.
        std::map<BlockPos, gameplay::BlockState> now;
        for (const auto& [pos, raw] : b.blocks) now.emplace(pos, *gameplay::BlockState::decode(raw));
        diff_sorted(a.blocks, b.blocks, [](const auto& e) { return e.first; }, [&](BlockPos pos) {
            const auto it = now.find(pos);
            feed.blocks.push_back({level, pos, it == now.end() ? gameplay::blocks::air() : it->second});
        });
        diff_sorted(a.block_entities, b.block_entities, [](const auto& e) { return e.first; },
                    [&](BlockPos pos) { feed.block_entities.emplace_back(level, pos); });
        diff_sorted(a.entities, b.entities, [](const gameplay::Entity& e) { return e.id; },
                    [&](contracts::EntityId id) { feed.entities.emplace_back(level, id); });
        diff_sorted(a.chunks, b.chunks, [](const gameplay::LevelDump::Chunk& c) { return c.pos; },
                    [&](contracts::ChunkPos pos) { feed.chunks.emplace_back(level, pos); });
    }
    feed.world = before.day_time != after.day_time || !(before.weather == after.weather) || before.difficulty != after.difficulty
        || before.game_rules != after.game_rules || before.tickets != after.tickets || before.players != after.players
        || before.maps != after.maps || before.last_map_id != after.last_map_id;
    return feed;
}

} // namespace circuit::runtime
