// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/protocol/packets.hpp>
#include <vector>

namespace circuit::protocol {

struct PacketIds::Table {
    struct Entry {
        State state;
        Bound bound;
        std::int32_t id;
        std::string_view name;
    };
    std::int32_t protocol;
    std::int32_t revision;
    std::vector<Entry> entries;
};

namespace detail {
using PacketTable = PacketIds::Table;
extern const PacketTable packets_772;
extern const PacketTable packets_769;
} // namespace detail

} // namespace circuit::protocol
