// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>
#include <span>

namespace circuit::server {

// A box of a block's collision shape, in block coordinates (0-1; piston
// heads reach a quarter block beyond their cell).
struct CollisionBox {
    double min_x, min_y, min_z, max_x, max_y, max_z;
};

// The collision shape of a target-version block state, for the blocks the
// reference rules model (generated from public data); nullopt for other
// states or protocols.
std::optional<std::span<const CollisionBox>> collision_boxes(std::int32_t protocol, std::int32_t state_id) noexcept;

} // namespace circuit::server
