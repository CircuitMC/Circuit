// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

// Packet ids per protocol version, compiled from cited revisions of the public
// protocol documentation by tools/extract_packet_ids.py. Packets are named by
// their resource name, e.g. "level_chunk_with_light".
namespace circuit::protocol {

enum class State : std::uint8_t { handshaking, status, login, configuration, play };
enum class Bound : std::uint8_t { serverbound, clientbound };

class PacketIds {
public:
    // Ids of the documented protocol versions; nullptr for others.
    static const PacketIds* for_protocol(std::int32_t protocol) noexcept;

    [[nodiscard]] std::int32_t protocol() const noexcept;
    [[nodiscard]] std::int32_t documentation_revision() const noexcept;
    [[nodiscard]] std::optional<std::int32_t> find(State state, Bound bound, std::string_view name) const noexcept;
    // Throws std::out_of_range for an undocumented packet: adapters resolve
    // their ids once at startup, so a missing packet fails early.
    [[nodiscard]] std::int32_t id(State state, Bound bound, std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> name(State state, Bound bound, std::int32_t id) const noexcept;

    struct Table;
    explicit PacketIds(const Table& table) noexcept : table_(table) {}

private:
    const Table& table_;
};

} // namespace circuit::protocol
