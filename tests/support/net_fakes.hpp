// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// An in-memory network side behind the net/view boundary
// (include/circuit/net/link.hpp), for tests of the game side: no sockets, no
// threads, no encryption. MemoryNetwork keeps, per connection, the frames
// that were flushed to it, in the order the network side would write them.
#include <circuit/net/link.hpp>
#include <circuit/protocol/framing.hpp>
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace circuit::test {

class MemoryNetwork;

class MemoryPeer final : public net::Peer {
public:
    MemoryPeer(MemoryNetwork& network, net::ConnId id, std::int32_t protocol) : network_(network), id_(id), protocol_(protocol) {}
    net::ConnId id() const override { return id_; }
    std::int32_t protocol() const override { return protocol_; }
    std::size_t take_inbound(std::vector<net::InboundPacket>& out) override {
        const auto n = inbound_.size();
        out.insert(out.end(), std::make_move_iterator(inbound_.begin()), std::make_move_iterator(inbound_.end()));
        inbound_.clear();
        return n;
    }
    bool closed() const override { return closed_; }
    std::size_t queued_bytes() const override { return queued_; }

private:
    friend class MemoryNetwork;
    MemoryNetwork& network_;
    net::ConnId id_;
    std::int32_t protocol_;
    std::vector<net::InboundPacket> inbound_;
    bool closed_{};
    std::size_t queued_{};
};

// What a flush delivered to one connection.
struct Delivered {
    std::vector<net::Bytes> frames;   // each item's frames, in write order
    std::optional<std::string> kicked; // the reason, when a kick was written
};

class MemoryNetwork {
public:
    explicit MemoryNetwork(std::int32_t compression_threshold = 256) : threshold_(compression_threshold) {}
    std::int32_t threshold() const noexcept { return threshold_; }

    std::shared_ptr<MemoryPeer> connect(std::int32_t protocol = 772) {
        const net::ConnId id{0, next_slot_++, 1};
        auto peer = std::make_shared<MemoryPeer>(*this, id, protocol);
        peers_[id] = peer;
        return peer;
    }
    // The client sends a play packet; arrival numbers count over all peers.
    void receive(MemoryPeer& peer, net::Bytes packet) {
        if (peer.closed_) return;
        peer.inbound_.push_back({next_arrival_++, std::move(packet)});
    }
    void disconnect(MemoryPeer& peer) { peer.closed_ = true; }
    const Delivered& delivered(net::ConnId id) { return delivered_[id]; }

    // An item a flush hands over: frames, or a kick.
    struct Item {
        net::ConnId to;
        net::Frame frames;
        std::optional<std::string> kick;
    };
    void deliver(std::vector<Item> items) {
        for (auto& item : items) {
            const auto peer = peers_.find(item.to);
            if (peer == peers_.end()) continue;
            auto& d = delivered_[item.to];
            if (peer->second->closed_ || d.kicked) continue; // after a kick or a close, nothing more
            if (item.kick) {
                d.kicked = *item.kick;
                peer->second->closed_ = true;
                continue;
            }
            d.frames.push_back(*item.frames);
        }
    }

private:
    std::int32_t threshold_;
    std::uint32_t next_slot_{1};
    std::uint64_t next_arrival_{1};
    std::map<net::ConnId, std::shared_ptr<MemoryPeer>> peers_;
    std::map<net::ConnId, Delivered> delivered_;
};

class MemoryOutbox final : public net::Outbox {
public:
    explicit MemoryOutbox(MemoryNetwork& network) : network_(network) {}
    void send(net::ConnId to, std::span<const std::byte> packet) override { send(to, share(packet)); }
    void send(net::ConnId to, const net::Frame& frames) override { pending_.push_back({to, frames, std::nullopt}); }
    void send(std::span<const net::ConnId> to, const net::Frame& frames) override {
        for (const auto id : to) send(id, frames);
    }
    net::Frame share(std::span<const std::byte> packet) override {
        return std::make_shared<const net::Bytes>(protocol::encode_frame(packet, network_.threshold()));
    }
    void kick(net::ConnId to, std::string_view reason) override { pending_.push_back({to, nullptr, std::string(reason)}); }
    void flush() override { network_.deliver(std::exchange(pending_, {})); }
    std::size_t pending() const noexcept { return pending_.size(); }

private:
    MemoryNetwork& network_;
    std::vector<MemoryNetwork::Item> pending_;
};

} // namespace circuit::test
