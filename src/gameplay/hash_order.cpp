// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <numeric>

namespace circuit::gameplay::detail {
namespace {
std::uint32_t position_hash(BlockPos p) {
    // 32-bit wrap-around arithmetic: (y + z * 31) * 31 + x.
    const auto y = static_cast<std::uint32_t>(p.y), z = static_cast<std::uint32_t>(p.z), x = static_cast<std::uint32_t>(p.x);
    return (y + z * 31U) * 31U + x;
}
} // namespace

std::vector<BlockPos> hash_order(std::span<const BlockPos> positions, std::size_t inserted) {
    std::size_t buckets = 16;
    while (inserted * 4 > buckets * 3) buckets *= 2;
    std::vector<std::size_t> index(positions.size());
    std::iota(index.begin(), index.end(), 0);
    const auto bucket = [&](std::size_t i) {
        const auto h = position_hash(positions[i]);
        return (h ^ (h >> 16)) & static_cast<std::uint32_t>(buckets - 1);
    };
    std::ranges::stable_sort(index, {}, bucket);
    std::vector<BlockPos> out;
    out.reserve(positions.size());
    for (const auto i : index) out.push_back(positions[i]);
    return out;
}

} // namespace circuit::gameplay::detail
