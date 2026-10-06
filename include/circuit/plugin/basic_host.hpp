// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "circuit/plugin/api.hpp"

namespace circuit::plugin {

// A host without a world, for tools and tests: it implements logging and
// metrics in a derived class and answers every world service with
// Status::unavailable.
class HostWithoutWorld : public IHost {
public:
    [[nodiscard]] Permissions permissions() const noexcept override { return 0; }
    [[nodiscard]] Result<std::uint64_t> time() const noexcept override { return std::unexpected(Status::unavailable); }
    [[nodiscard]] Result<std::string> block(Position) const noexcept override { return std::unexpected(Status::unavailable); }
    [[nodiscard]] Result<BlockEntityInfo> block_entity(Position) const noexcept override { return std::unexpected(Status::unavailable); }
    [[nodiscard]] Result<ChunkInfo> chunk(std::int32_t, std::int32_t) const noexcept override { return std::unexpected(Status::unavailable); }
    [[nodiscard]] Result<std::vector<ItemEntity>> items(std::int32_t, std::int32_t) const noexcept override {
        return std::unexpected(Status::unavailable);
    }
    Result<std::uint64_t> set_block(Position, std::string_view) noexcept override { return std::unexpected(Status::unavailable); }
    Result<std::uint64_t> use_block(Position) noexcept override { return std::unexpected(Status::unavailable); }
    Result<void> cancel(std::uint64_t) noexcept override { return std::unexpected(Status::unavailable); }
    Result<void> subscribe() noexcept override { return std::unexpected(Status::unavailable); }
    Result<void> register_command(std::string_view, std::string_view, ICommandHandler&) noexcept override {
        return std::unexpected(Status::unavailable);
    }
    Result<void> tell(std::string_view, std::string_view) noexcept override { return std::unexpected(Status::unavailable); }
    Result<void> freeze(bool) noexcept override { return std::unexpected(Status::unavailable); }
    Result<void> step(std::uint32_t) noexcept override { return std::unexpected(Status::unavailable); }
    [[nodiscard]] TickStatus tick_status() const noexcept override { return {}; }
};

} // namespace circuit::plugin
