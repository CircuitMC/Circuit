// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/runtime/engine.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Server state kept in checkpoints next to the engine state.
namespace circuit::server {

struct WorldMeta {
    std::int32_t floor_y{};          // flat generator layer for columns created later
    contracts::BlockPos spawn{};     // where new players appear (standing on this block's top)
    std::uint64_t next_admission{1}; // command admissions continue across restarts
    std::uint32_t next_ticket{1000};
    std::string data{"none"};        // fingerprint of the data packs the log was written with
    // False for a world written before tickets had types: its tickets were
    // all spawn and player tickets, saved as forced ones. Opening or
    // migrating it removes them.
    bool typed_tickets{true};
};

inline constexpr std::string_view meta_extra = "circuit:server";
inline constexpr std::string_view players_extra = "circuit:players";

// A new world's metadata: spawn above the origin of the floor.
WorldMeta fresh_meta(std::int32_t floor_y);
std::vector<std::byte> encode_meta(const WorldMeta& meta);
std::optional<WorldMeta> decode_meta(std::span<const std::byte> bytes);

// Engine options for a world whose new columns get one stone layer at
// `floor_y` (none when it is outside the world height).
runtime::EngineOptions world_engine_options(const contracts::VersionBundle& bundle, std::int32_t floor_y, unsigned workers);

} // namespace circuit::server
