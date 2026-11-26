// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/protocol/codec.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The boundary between the network layer (net lane: sockets, framing,
// encryption, login, on PACE) and the game side (view lane: what players see
// and do). Nothing here names PACE, sockets or threads of the network side.
//
// Threads: the network side runs its own threads (reactors). A Peer is used
// by one simulation thread at a time, the one that owns the player (one
// island; with one simulation thread, that thread), concurrently with the
// network side. An Outbox is used by one producing thread and hands its
// contents to the network side at flush(). PlayHost::admit is called by the
// network side from any of its threads.
namespace circuit::net {

using protocol::Bytes;

// One connection, as long as it lasts; the generation tells a reused slot
// apart.
struct ConnId {
    std::uint16_t reactor{};
    std::uint32_t slot{};
    std::uint32_t generation{};
    bool operator==(const ConnId&) const = default;
    auto operator<=>(const ConnId&) const = default;
};

// One or more complete wire frames (length-prefixed, compressed at the
// server's threshold), before encryption: the same bytes for every
// recipient. Immutable once made.
using Frame = std::shared_ptr<const Bytes>;

// A play packet the client sent: its id and fields (a decrypted, inflated,
// unframed packet), with the order it arrived in. Arrival numbers increase
// over all connections of the process, so packets of several players merge
// into one arrival order.
struct InboundPacket {
    std::uint64_t arrival{};
    Bytes packet; // VarInt id followed by the fields
};

// The game side's handle of one connection in play.
class Peer {
public:
    virtual ~Peer() = default;
    [[nodiscard]] virtual ConnId id() const = 0;
    // The protocol number the client speaks (772: 1.21.8, 769: 1.21.4).
    [[nodiscard]] virtual std::int32_t protocol() const = 0;
    // Appends the play packets received since the last call, in arrival
    // order, and returns how many. The network side stops reading a
    // connection whose packets are not taken, which pushes back through TCP.
    virtual std::size_t take_inbound(std::vector<InboundPacket>& out) = 0;
    // The connection ended (the client left, a limit closed it, or a kick
    // was written); nothing more arrives and sends to it are dropped.
    [[nodiscard]] virtual bool closed() const = 0;
    // Bytes given to the network side for this connection and not yet
    // written: the back-pressure gauge. Above a soft mark the game sends only
    // what the player needs (no new chunks).
    [[nodiscard]] virtual std::size_t queued_bytes() const = 0;
};

// Outgoing packets of one producing thread. For one connection, what one
// outbox sends is written in the order it was sent; packets of several
// outboxes for one connection are ordered by their flushes. A kick is the
// last item of its connection: what is sent to it afterwards is dropped.
class Outbox {
public:
    virtual ~Outbox() = default;
    // One packet (VarInt id followed by its fields) to one connection; the
    // outbox frames and compresses it.
    virtual void send(ConnId to, std::span<const std::byte> packet) = 0;
    // Pre-encoded frames to one connection, or to several.
    virtual void send(ConnId to, const Frame& frames) = 0;
    virtual void send(std::span<const ConnId> to, const Frame& frames) = 0;
    // Frames one packet once, for sending to many connections (a chunk to
    // every viewer).
    [[nodiscard]] virtual Frame share(std::span<const std::byte> packet) = 0;
    // Sends the disconnect packet with `reason` (text) and closes the
    // connection after it is written.
    virtual void kick(ConnId to, std::string_view reason) = 0;
    // Hands everything sent since the last flush to the network side.
    virtual void flush() = 0;
};

// Who a completed login is.
struct Identity {
    std::string uuid;       // canonical form with dashes
    std::string name;
    bool verified{};        // confirmed by the session service (online mode); otherwise an offline identity
    std::vector<std::pair<std::string, std::string>> properties; // name, value (textures), from the session service
};

// A connection that finished login and configuration and enters play.
struct JoinRequest {
    std::shared_ptr<Peer> peer;
    Identity identity;
    std::string locale;          // as the client's information gave it
    std::uint8_t view_distance{}; // the client's own setting, before the server's limit
};

// The game side, as the network side sees it.
class PlayHost {
public:
    virtual ~PlayHost() = default;
    // A player enters the world. Called from any network thread; the game
    // side queues it and places the player at a tick boundary.
    virtual void admit(JoinRequest request) = 0;
};

} // namespace circuit::net
