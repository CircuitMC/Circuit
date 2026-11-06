// SPDX-License-Identifier: AGPL-3.0-only
#include "movement.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace circuit::server::movement {
namespace {
double horizontal(Vec3 v) noexcept { return v.x * v.x + v.z * v.z; }
using gameplay::physics::collide_with;
using gameplay::physics::flatten;
} // namespace


Aabb player_box(Vec3 feet, bool crouching) noexcept { return player_box(feet, crouching ? crouching_height : standing_height); }

Aabb player_box(Vec3 feet, double height) noexcept {
    const double half = player_width / 2.0;
    return {feet.x - half, feet.y, feet.z - half, feet.x + half, feet.y + height, feet.z + half};
}

std::vector<Collider> colliders(const Aabb& area, const ShapeLookup& lookup) {
    return gameplay::physics::colliders(area, [&lookup](contracts::BlockPos p) { return lookup(p).boxes; });
}

Vec3 collide(const Aabb& box, Vec3 movement, bool on_ground, const ShapeLookup& lookup) {
    if (movement.x == 0.0 && movement.y == 0.0 && movement.z == 0.0) return movement;
    const auto result = collide_with(movement, box, flatten(colliders(box.expand_towards(movement.x, movement.y, movement.z), lookup)));
    const bool hit_x = movement.x != result.x, hit_z = movement.z != result.z;
    const bool landed = movement.y != result.y && movement.y < 0.0;
    if (!(landed || on_ground) || !(hit_x || hit_z)) return result;
    // Stepping up: from the box after its vertical movement when it landed,
    // try the heights of the blocks ahead, lowest first, up to the step
    // height; the first that gets further horizontally wins.
    const Aabb base = landed ? box.move(0.0, result.y, 0.0) : box;
    Aabb area = base.expand_towards(movement.x, step_height, movement.z);
    if (!landed) area = area.expand_towards(0.0, -1.0e-5, 0.0);
    const auto ahead = colliders(area, lookup);
    const auto skip = static_cast<float>(result.y);
    std::vector<float> heights;
    for (const auto& c : ahead) {
        std::vector<double> ys;
        for (const auto& b : c.boxes) {
            ys.push_back(b.min_y);
            ys.push_back(b.max_y);
        }
        std::ranges::sort(ys);
        for (const auto y : ys) {
            const auto h = static_cast<float>(y - base.min_y);
            if (h < 0.0f || h == skip) continue;
            if (h > static_cast<float>(step_height)) break;
            if (std::ranges::find(heights, h) == heights.end()) heights.push_back(h);
        }
    }
    std::ranges::sort(heights);
    const auto boxes = flatten(ahead);
    for (const auto h : heights) {
        const auto stepped = collide_with({movement.x, static_cast<double>(h), movement.z}, base, boxes);
        if (horizontal(stepped) > horizontal(result)) return {stepped.x, stepped.y + (base.min_y - box.min_y), stepped.z};
    }
    return result;
}

bool collides(const Aabb& box, const ShapeLookup& lookup) { return !colliders(box, lookup).empty(); }

bool collides_with_anything_new(const Aabb& from, const Aabb& to, const ShapeLookup& lookup) {
    const auto old_box = from.deflate(1.0e-5);
    for (const auto& c : colliders(to.deflate(1.0e-5), lookup))
        if (std::ranges::none_of(c.boxes, [&](const Aabb& b) { return b.intersects(old_box); })) return true;
    return false;
}

bool no_blocks_around(const Aabb& box, const ShapeLookup& lookup) {
    const auto area = box.inflate(0.0625, 0.0625, 0.0625).expand_towards(0.0, -0.55, 0.0);
    const auto first = [](double v) { return static_cast<std::int32_t>(std::floor(v)); };
    for (auto x = first(area.min_x); x <= first(area.max_x); ++x)
        for (auto y = first(area.min_y); y <= first(area.max_y); ++y)
            for (auto z = first(area.min_z); z <= first(area.max_z); ++z)
                if (!lookup({x, y, z}).air) return false;
    return true;
}

} // namespace circuit::server::movement
