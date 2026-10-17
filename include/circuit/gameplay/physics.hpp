// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/gameplay/block.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Boxes moving against block collision shapes, as both target versions move
// entities and check players: a box moves along the vertical axis first,
// then along the horizontal axis with the larger movement, then the other;
// along each axis it stops at the first collision box ahead, and offsets
// below 1e-7 count as none. Checked against official traces of item
// entities and primed TNT.
namespace circuit::gameplay::physics {

using contracts::Vec3;

struct Aabb {
    double min_x{}, min_y{}, min_z{}, max_x{}, max_y{}, max_z{};

    [[nodiscard]] Aabb move(double dx, double dy, double dz) const noexcept {
        return {min_x + dx, min_y + dy, min_z + dz, max_x + dx, max_y + dy, max_z + dz};
    }
    [[nodiscard]] Aabb move(Vec3 v) const noexcept { return move(v.x, v.y, v.z); }
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

// The box of an entity `width` wide and `height` tall standing at `feet`.
[[nodiscard]] Aabb box_at(Vec3 feet, double width, double height) noexcept;

// The registry state a state stands for (a table built once); nullopt for
// the unloaded sentinel.
[[nodiscard]] std::optional<std::uint32_t> registry_state_of(BlockState s);
// A state's collision boxes in a release, in block coordinates (modeled
// states through the registry states they stand for).
[[nodiscard]] std::span<const registry::Box> collision_boxes(BlockState s, std::string_view release);

// A cell's collision boxes, in block coordinates.
using ShapeAt = std::function<std::span<const registry::Box>(contracts::BlockPos)>;

// The collision boxes, in world coordinates, of one block whose shape
// intersects the queried area.
struct Collider {
    contracts::BlockPos pos;
    std::vector<Aabb> boxes;
};
[[nodiscard]] std::vector<Collider> colliders(const Aabb& area, const ShapeAt& shapes);
[[nodiscard]] std::vector<Aabb> flatten(const std::vector<Collider>& colliders);

// The part of `movement` a box makes before it collides with `boxes`.
[[nodiscard]] Vec3 collide_with(Vec3 movement, const Aabb& box, const std::vector<Aabb>& boxes);
// The same against the blocks around it.
[[nodiscard]] Vec3 collide(const Aabb& box, Vec3 movement, const ShapeAt& shapes);

} // namespace circuit::gameplay::physics
