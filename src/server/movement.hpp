// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "wire.hpp"
#include <circuit/contracts/ids.hpp>
#include <circuit/gameplay/physics.hpp>
#include <functional>
#include <span>
#include <vector>

// Player movement against block collision shapes, as the target version's
// server checks it: a claimed move is replayed with collision (the rules'
// box collision, gameplay/physics.hpp, stepping up to 0.6 blocks), and the
// result is compared with the claim.
namespace circuit::server::movement {

// Boxes and the collision of a box moving against blocks are the rules'
// (gameplay/physics.hpp): items and players move alike.
using gameplay::physics::Aabb;
using gameplay::physics::Collider;

// A block for movement: whether it is air, and its collision boxes.
struct BlockShape {
    bool air{true};
    std::span<const gameplay::registry::Box> boxes;
};
using ShapeLookup = std::function<BlockShape(contracts::BlockPos)>;

inline constexpr double player_width = 0.6;
inline constexpr double standing_height = 1.8;
inline constexpr double crouching_height = 1.5;
inline constexpr double step_height = 0.6;

[[nodiscard]] Aabb player_box(Vec3 feet, bool crouching) noexcept;

// The collision boxes, in world coordinates, of the blocks whose shapes
// intersect the queried area.
[[nodiscard]] std::vector<Collider> colliders(const Aabb& area, const ShapeLookup& lookup);

// The part of `movement` a box can make before it collides.
[[nodiscard]] Vec3 collide(const Aabb& box, Vec3 movement, bool on_ground, const ShapeLookup& lookup);

// Whether any block's collision shape overlaps the box.
[[nodiscard]] bool collides(const Aabb& box, const ShapeLookup& lookup);

// Whether the box at `to` overlaps a block that the box at `from` did not.
[[nodiscard]] bool collides_with_anything_new(const Aabb& from, const Aabb& to, const ShapeLookup& lookup);

// Whether every block around a player (the box grown by 1/16, and 0.55
// downwards) is air: a player there that does not sink is floating.
[[nodiscard]] bool no_blocks_around(const Aabb& box, const ShapeLookup& lookup);

} // namespace circuit::server::movement
