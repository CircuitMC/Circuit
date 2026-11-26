// SPDX-License-Identifier: AGPL-3.0-only
// The net/view boundary (include/circuit/net/link.hpp) through the in-memory
// network side the game side's tests use (tests/support/net_fakes.hpp): order,
// shared frames, flushes, kicks and inbound arrival order.
#include "../support/check.hpp"
#include "../support/net_fakes.hpp"
#include <circuit/protocol/framing.hpp>
#include <format>
#include <vector>

namespace {
using namespace circuit;
using circuit::test::MemoryNetwork;
using circuit::test::MemoryOutbox;
using circuit::test::require;

// A packet: id, then `size` bytes of `fill`.
net::Bytes packet(std::uint8_t id, std::size_t size, std::uint8_t fill = 7) {
    net::Bytes p{static_cast<std::byte>(id)};
    p.resize(1 + size, static_cast<std::byte>(fill));
    return p;
}
net::Bytes framed(const net::Bytes& p, std::int32_t threshold) { return protocol::encode_frame(p, threshold); }
} // namespace

CIRCUIT_TEST("an outbox writes one connection's items in the order sent, at flush") {
    MemoryNetwork network(256);
    MemoryOutbox out(network);
    const auto peer = network.connect();
    const auto chunk = out.share(packet(0x27, 900));
    out.send(peer->id(), packet(0x01, 10));
    out.send(peer->id(), chunk);
    out.send(peer->id(), packet(0x02, 20));
    require(network.delivered(peer->id()).frames.empty() && out.pending() == 3, "nothing is written before the flush");
    out.flush();
    const auto& d = network.delivered(peer->id()).frames;
    require(d.size() == 3 && d[0] == framed(packet(0x01, 10), 256) && d[1] == *chunk && d[2] == framed(packet(0x02, 20), 256),
            "in the order sent");
    require(out.pending() == 0, "the flush empties the outbox");
}

CIRCUIT_TEST("a shared frame is the same bytes for every recipient, framed at the server's threshold") {
    MemoryNetwork network(256);
    MemoryOutbox out(network);
    std::vector<net::ConnId> viewers;
    for (int i = 0; i < 5; ++i) viewers.push_back(network.connect()->id());
    for (const std::size_t size : {std::size_t{10}, std::size_t{4000}}) {
        const auto p = packet(0x27, size);
        const auto frame = out.share(p);
        require(*frame == framed(p, 256), std::format("{} bytes: framed as encode_frame frames it", size));
        out.send(viewers, frame);
    }
    out.flush();
    for (const auto v : viewers) {
        const auto& d = network.delivered(v).frames;
        require(d.size() == 2 && d[0] == framed(packet(0x27, 10), 256) && d[1] == framed(packet(0x27, 4000), 256), "each viewer got both");
    }
    // Above the threshold the frame is compressed: shorter than the packet.
    require(framed(packet(0x27, 4000), 256).size() < 4000, "compressed at the threshold");
}

CIRCUIT_TEST("a kick is the last item of its connection") {
    MemoryNetwork network;
    MemoryOutbox out(network);
    const auto a = network.connect(), b = network.connect();
    out.send(a->id(), packet(0x01, 4));
    out.kick(a->id(), "Kicked by an operator");
    out.send(a->id(), packet(0x02, 4)); // after the kick: dropped
    out.send(b->id(), packet(0x03, 4)); // another connection: written
    out.flush();
    const auto& da = network.delivered(a->id());
    require(da.frames.size() == 1 && da.kicked == "Kicked by an operator" && a->closed(), "written up to the kick, then closed");
    require(network.delivered(b->id()).frames.size() == 1 && !b->closed(), "the other connection is unaffected");
    out.send(a->id(), packet(0x04, 4));
    out.flush();
    require(network.delivered(a->id()).frames.size() == 1, "nothing reaches a closed connection");
}

CIRCUIT_TEST("items of two outboxes for one connection are ordered by their flushes") {
    MemoryNetwork network;
    MemoryOutbox first(network), second(network);
    const auto peer = network.connect();
    first.send(peer->id(), packet(0x01, 4));
    second.send(peer->id(), packet(0x02, 4));
    second.flush();
    first.flush();
    const auto& d = network.delivered(peer->id()).frames;
    require(d.size() == 2 && d[0] == framed(packet(0x02, 4), 256) && d[1] == framed(packet(0x01, 4), 256), "the second outbox flushed first");
}

CIRCUIT_TEST("inbound packets come in arrival order, numbered over all connections") {
    MemoryNetwork network;
    const auto a = network.connect(), b = network.connect(769);
    require(a->protocol() == 772 && b->protocol() == 769, "each peer speaks its protocol");
    network.receive(*a, packet(0x10, 2));
    network.receive(*b, packet(0x11, 2));
    network.receive(*a, packet(0x12, 2));
    std::vector<net::InboundPacket> in;
    require(a->take_inbound(in) == 2 && b->take_inbound(in) == 1, "two from a, one from b");
    require(in[0].packet == packet(0x10, 2) && in[1].packet == packet(0x12, 2) && in[2].packet == packet(0x11, 2), "each peer's in its order");
    require(in[0].arrival < in[2].arrival && in[2].arrival < in[1].arrival, "arrival numbers merge the peers into one order");
    std::vector<net::InboundPacket> again;
    require(a->take_inbound(again) == 0 && again.empty(), "taken packets are gone");
    network.disconnect(*a);
    network.receive(*a, packet(0x13, 2));
    require(a->closed() && a->take_inbound(again) == 0, "a closed connection receives nothing");
}
