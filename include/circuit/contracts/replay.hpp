// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/commands.hpp>
#include <circuit/contracts/errors.hpp>
#include <circuit/contracts/time.hpp>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

// Replay records hold the external inputs that, together with the bundle and
// seed, determine a headless run. The binary encoding is little-endian and
// versioned; decoding validates every length and value against limits.
namespace circuit::contracts {

struct TickInput {
    Tick tick{};
    std::vector<Command> commands;
    bool operator==(const TickInput&) const = default;
};
struct ReplayLog {
    std::string bundle_id;
    std::uint64_t seed{};
    std::vector<TickInput> ticks; // Strictly increasing ticks; admissions strictly increasing overall.
    bool operator==(const ReplayLog&) const = default;
};
struct ReplayLimits {
    std::size_t max_bytes = 64u << 20;
    std::size_t max_ticks = 1u << 20;
    std::size_t max_commands_per_tick = 4096;
    std::size_t max_bundle_id = 64;
};

Result<std::vector<std::byte>> encode_replay(const ReplayLog& log, const ReplayLimits& limits = {});
Result<ReplayLog> decode_replay(std::span<const std::byte> bytes, const ReplayLimits& limits = {});

} // namespace circuit::contracts
