// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>
#include <expected>
#include <string_view>

namespace circuit::contracts {

// Stable error codes. Numeric values are part of replay and diagnostic
// records: never renumber or reuse a value.
enum class ErrorCode : std::uint16_t {
    invalid_argument = 1,
    out_of_bounds = 2,
    unloaded_chunk = 3,
    stale_generation = 4,
    unknown_entity = 5,
    invalid_block_state = 6,
    not_a_container = 7,
    invalid_slot = 8,
    item_mismatch = 9,
    stack_overflow = 10,
    unknown_version = 11,
    unsupported_version = 12,
    capacity_exceeded = 13,
    counter_overflow = 14,
    malformed_record = 15,
    record_too_large = 16,
    unknown_ticket = 17,
    duplicate_ticket = 18,
    stale_view = 19,
    not_usable = 20,
    unknown_command = 21,
    rules_mismatch = 22,
    out_of_reach = 23,
    occupied = 24,
    unsupported = 25,
    not_breakable = 26,
};

constexpr std::string_view error_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::out_of_bounds: return "out_of_bounds";
    case ErrorCode::unloaded_chunk: return "unloaded_chunk";
    case ErrorCode::stale_generation: return "stale_generation";
    case ErrorCode::unknown_entity: return "unknown_entity";
    case ErrorCode::invalid_block_state: return "invalid_block_state";
    case ErrorCode::not_a_container: return "not_a_container";
    case ErrorCode::invalid_slot: return "invalid_slot";
    case ErrorCode::item_mismatch: return "item_mismatch";
    case ErrorCode::stack_overflow: return "stack_overflow";
    case ErrorCode::unknown_version: return "unknown_version";
    case ErrorCode::unsupported_version: return "unsupported_version";
    case ErrorCode::capacity_exceeded: return "capacity_exceeded";
    case ErrorCode::counter_overflow: return "counter_overflow";
    case ErrorCode::malformed_record: return "malformed_record";
    case ErrorCode::record_too_large: return "record_too_large";
    case ErrorCode::unknown_ticket: return "unknown_ticket";
    case ErrorCode::duplicate_ticket: return "duplicate_ticket";
    case ErrorCode::stale_view: return "stale_view";
    case ErrorCode::not_usable: return "not_usable";
    case ErrorCode::unknown_command: return "unknown_command";
    case ErrorCode::rules_mismatch: return "rules_mismatch";
    case ErrorCode::out_of_reach: return "out_of_reach";
    case ErrorCode::occupied: return "occupied";
    case ErrorCode::unsupported: return "unsupported";
    case ErrorCode::not_breakable: return "not_breakable";
    }
    return "unknown_error";
}

template <class T>
using Result = std::expected<T, ErrorCode>;

} // namespace circuit::contracts
