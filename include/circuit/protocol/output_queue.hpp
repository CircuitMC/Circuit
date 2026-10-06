// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/protocol/codec.hpp>
#include <deque>
#include <memory>

// Outgoing frames of one connection: frames waiting to be written, and the
// one an asynchronous write is reading. The active frame is shared with the
// write's completion handler, which keeps it alive until the write completes,
// even after the connection discarded its waiting frames. Not thread-safe:
// a connection uses it from its strand.
namespace circuit::protocol {

class OutputQueue {
public:
    void push(Bytes frame) {
        bytes_ += frame.size();
        waiting_.push_back(std::move(frame));
    }
    // Makes the next waiting frame the active one and returns it; null when a
    // write is already active or nothing waits. The caller passes the result
    // to the write's completion handler.
    std::shared_ptr<const Bytes> begin() {
        if (active_ || waiting_.empty()) return nullptr;
        auto frame = std::make_shared<const Bytes>(std::move(waiting_.front()));
        waiting_.pop_front();
        active_ = frame;
        return frame;
    }
    // The active write completed; `frame` is the value `begin` returned for it.
    // A completion of a write begun before `discard` is ignored.
    void finish(const std::shared_ptr<const Bytes>& frame) noexcept {
        if (!frame || frame != active_) return;
        bytes_ -= frame->size();
        active_.reset();
    }
    // Drops the waiting frames. The active frame stays alive in its handler;
    // only the queue's references and counts are released.
    void discard() noexcept {
        waiting_.clear();
        active_.reset();
        bytes_ = 0;
    }
    // Bytes waiting or being written.
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool writing() const noexcept { return active_ != nullptr; }
    [[nodiscard]] std::size_t waiting() const noexcept { return waiting_.size(); }

private:
    std::deque<Bytes> waiting_;
    std::shared_ptr<const Bytes> active_;
    std::size_t bytes_{};
};

} // namespace circuit::protocol
