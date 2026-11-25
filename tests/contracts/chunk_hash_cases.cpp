// SPDX-License-Identifier: AGPL-3.0-only
#include "../support/check.hpp"
#include <algorithm>
#include <circuit/contracts/ids.hpp>
#include <cstdint>
#include <format>
#include <set>
#include <unordered_set>

using namespace circuit::contracts;
using circuit::test::require;

namespace {
// The largest number of keys that share one bucket.
template <class Key>
std::size_t longest_chain(const std::unordered_set<Key>& set) {
    std::size_t longest = 0;
    for (std::size_t b = 0; b < set.bucket_count(); ++b) longest = std::max(longest, set.bucket_size(b));
    return longest;
}
} // namespace

// Chunk coordinates are small neighbouring integers. A hash that only packs
// them into one word leaves every column of a diagonal in one bucket of a
// table whose size divides 2^32 - 1 (257 buckets hold 196 columns that way).
CIRCUIT_TEST("chunk keys of a square area spread over the buckets") {
    for (const std::int32_t origin : {0, -3, -7, 1000, -30000000 / 16}) {
        for (const std::int32_t side : {5, 14, 21, 37, 70}) {
            std::unordered_set<ChunkKey> keys;
            std::unordered_set<ChunkPos> positions;
            for (std::int32_t x = origin; x < origin + side; ++x) {
                for (std::int32_t z = origin; z < origin + side; ++z) {
                    keys.insert({DimensionId::overworld, {x, z}});
                    positions.insert({x, z});
                }
            }
            // A table at most as full as its bucket count, filled by a hash
            // that behaves randomly, has no chain near this length.
            require(longest_chain(keys) <= 8 && longest_chain(positions) <= 8,
                    std::format("origin {} side {}: chains of {} and {} in {} buckets", origin, side, longest_chain(keys),
                                longest_chain(positions), keys.bucket_count()));
        }
    }
}

CIRCUIT_TEST("chunk key hashes differ by coordinate, by axis and by dimension") {
    const std::hash<ChunkKey> hash;
    std::set<std::size_t> seen;
    std::size_t count = 0;
    for (const auto dimension : level_order) {
        for (std::int32_t x = -40; x <= 40; ++x) {
            for (std::int32_t z = -40; z <= 40; ++z) {
                seen.insert(hash({dimension, {x, z}}));
                ++count;
            }
        }
    }
    require(seen.size() == count, std::format("{} hashes for {} keys", seen.size(), count));
    // The low bits, which pick the bucket in a table of a power-of-two size,
    // take many values along a row, a column and a diagonal.
    for (const auto& step : {ChunkPos{1, 0}, ChunkPos{0, 1}, ChunkPos{1, 1}, ChunkPos{1, -1}}) {
        std::set<std::size_t> low;
        for (std::int32_t i = 0; i < 256; ++i) low.insert(hash({DimensionId::overworld, {i * step.x, i * step.z}}) & 1023);
        require(low.size() >= 200, std::format("step {} {}: {} low-bit values for 256 keys", step.x, step.z, low.size()));
    }
}
