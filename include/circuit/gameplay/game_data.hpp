// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/gameplay/block_registry.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>

// Game data that data packs define, in the form the rules use. An engine
// without it uses the public registry data (block_registry.hpp). It is fixed
// for the life of an engine, so a log replays under the data it was written
// with (the fingerprint names it).
namespace circuit::gameplay {

struct BlockTool {
    registry::Tool tool{};
    std::uint8_t tier{}; // lowest tool tier that harvests the block; 0: any
};

struct GameData {
    std::string fingerprint{"none"};
    // Per internal registry block: the tool that mines it faster and the
    // tier its drops need, from the mineable and needs-tool block tags.
    std::unordered_map<std::uint32_t, BlockTool> block_tools;
};

} // namespace circuit::gameplay
