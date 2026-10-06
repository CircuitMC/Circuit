// SPDX-License-Identifier: AGPL-3.0-only
#include "packet_tables.hpp"
#include <format>
#include <stdexcept>

namespace circuit::protocol {

const PacketIds* PacketIds::for_protocol(std::int32_t protocol) noexcept {
    static const PacketIds v772(detail::packets_772), v769(detail::packets_769);
    switch (protocol) {
    case 772: return &v772;
    case 769: return &v769;
    default: return nullptr;
    }
}

std::int32_t PacketIds::protocol() const noexcept { return table_.protocol; }
std::int32_t PacketIds::documentation_revision() const noexcept { return table_.revision; }

std::optional<std::int32_t> PacketIds::find(State state, Bound bound, std::string_view name) const noexcept {
    for (const auto& e : table_.entries)
        if (e.state == state && e.bound == bound && e.name == name) return e.id;
    return std::nullopt;
}

std::int32_t PacketIds::id(State state, Bound bound, std::string_view name) const {
    if (const auto id = find(state, bound, name)) return *id;
    throw std::out_of_range(std::format("protocol {}: undocumented packet `{}`", table_.protocol, name));
}

std::optional<std::string_view> PacketIds::name(State state, Bound bound, std::int32_t id) const noexcept {
    for (const auto& e : table_.entries)
        if (e.state == state && e.bound == bound && e.id == id) return e.name;
    return std::nullopt;
}

} // namespace circuit::protocol
