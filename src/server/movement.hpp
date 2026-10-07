// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "collision_shapes.hpp"
#include "wire.hpp"
#include <circuit/contracts/ids.hpp>
#include <functional>
#include <span>
#include <vector>

// Player movement against block collision shapes, as the target version's
// server checks it: a claimed move is replayed with collision (vertical axis
// first, then the larger horizontal one, stepping up to 0.6 blocks), and the
// result is compared with the claim.
namespace circuit::server::movement {

struct Aabb {
    double min_x{}, min_y{}, min_z{}, max_x{}, max_y{}, max_z{};

    [[nodiscard]] Aabb move(double dx, double dy, double dz) const noexcept {
        return {min_x + dx, min_y + dy, min_z + dz, max_x + dx, max_y + dy, max_z + dz};
    }
    // Grows the box in the direction of a movement.
    [[nodiscard]] Aabb expand_towards(double dx, double dy, double dz) const noexcept;
    [[nodiscard]] Aabb inflate(double x, double y, double z) const noexcept {
        return {min_x - x, min_y - y, min_z - z, max_x + x, max_y + y, max_z + z};
    }
    [[nodiscard]] Aabb deflate(double d) const noexcept { return inflate(-d, -d, -d); }
    // Overlap with a positive volume (touching faces do not intersect).
    [[nodiscard]] bool intersects(const Aabb& o) const noexcept {
        return min_x < o.max_x && max_x > o.min_x && min_y < o.max_y && max_y > o.min_y && min_z < o.max_z && max_z > o.min_z;
    }
};

// A block for movement: whether it is air, and its collision boxes.
struct BlockShape {
    bool air{true};
    std::span<const CollisionBox> boxes;
};
using ShapeLookup = std::function<BlockShape(contracts::BlockPos)>;

inline constexpr double player_width = 0.6;
inline constexpr double standing_height = 1.8;
inline constexpr double crouching_height = 1.5;
inline constexpr double step_height = 0.6;

[[nodiscard]] Aabb player_box(Vec3 feet, bool crouching) noexcept;

// The collision boxes, in world coordinates, of one block whose shape
// intersects the queried area.
struct Collider {
    contracts::BlockPos pos;
    std::vector<Aabb> boxes;
};
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
