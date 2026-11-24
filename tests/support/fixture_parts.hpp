// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Walks over the large fixture directories, divided between several cases.
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <vector>

namespace circuit::test {

// The entries of directories in the order of their file names, each directory
// after the one before it.
inline std::vector<std::filesystem::directory_entry> sorted_entries(std::initializer_list<std::filesystem::path> directories) {
    namespace fs = std::filesystem;
    std::vector<fs::directory_entry> all;
    for (const auto& directory : directories) {
        std::vector<fs::directory_entry> entries{fs::directory_iterator(directory), fs::directory_iterator()};
        std::ranges::sort(entries, {}, [](const fs::directory_entry& e) { return e.path().filename().string(); });
        all.insert(all.end(), entries.begin(), entries.end());
    }
    return all;
}

// A case that walks every recorded fixture is `fixture_parts` cases, so that
// shards of a program (CIRCUIT_TEST_SHARD) divide the walk between them.
inline constexpr std::size_t fixture_parts = 4;

// Part `part` of `parts`: every `parts`-th entry from entry `part` on. Every
// entry is in exactly one part. Neighbors in the order of names go to
// different parts, so that the parts take about equally long although the
// fixtures of one mechanism, which share a prefix, differ much from those of
// another.
inline std::vector<std::filesystem::directory_entry> part_of(const std::vector<std::filesystem::directory_entry>& all, std::size_t part,
                                                             std::size_t parts) {
    std::vector<std::filesystem::directory_entry> mine;
    for (std::size_t i = part; i < all.size(); i += parts) mine.push_back(all[i]);
    return mine;
}

} // namespace circuit::test
