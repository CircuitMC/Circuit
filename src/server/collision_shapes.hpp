// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>
#include <span>

namespace circuit::server {

// A box of a block's collision shape, in block coordinates (0-1; piston
// heads, fences and walls reach beyond their cell).
struct CollisionBox {
    double min_x, min_y, min_z, max_x, max_y, max_z;
};

// The collision shape of a target-version block state (generated from public
// data for every state); nullopt for unknown states or protocols.
std::optional<std::span<const CollisionBox>> collision_boxes(std::int32_t protocol, std::int32_t state_id) noexcept;

} // namespace circuit::server
