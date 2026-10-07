// SPDX-License-Identifier: AGPL-3.0-only
#include "movement.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace circuit::server::movement {
namespace {
constexpr double epsilon = 1.0e-7;

enum class Axis : std::uint8_t { x, y, z };

double low(const Aabb& b, Axis a) noexcept { return a == Axis::x ? b.min_x : a == Axis::y ? b.min_y : b.min_z; }
double high(const Aabb& b, Axis a) noexcept { return a == Axis::x ? b.max_x : a == Axis::y ? b.max_y : b.max_z; }
double get(Vec3 v, Axis a) noexcept { return a == Axis::x ? v.x : a == Axis::y ? v.y : v.z; }
void set(Vec3& v, Axis a, double d) noexcept { (a == Axis::x ? v.x : a == Axis::y ? v.y : v.z) = d; }
Aabb shifted(const Aabb& b, Vec3 v) noexcept { return b.move(v.x, v.y, v.z); }

// The cells whose blocks may have a box in `area`: one more on every side,
// for shapes that reach beyond their cell.
template <class F>
void for_each_cell(const Aabb& area, F&& f) {
    const auto first = [](double v) { return static_cast<std::int32_t>(std::floor(v - epsilon)) - 1; };
    const auto last = [](double v) { return static_cast<std::int32_t>(std::floor(v + epsilon)) + 1; };
    for (auto x = first(area.min_x); x <= last(area.max_x); ++x)
        for (auto y = first(area.min_y); y <= last(area.max_y); ++y)
            for (auto z = first(area.min_z); z <= last(area.max_z); ++z) f(contracts::BlockPos{x, y, z});
}

// How far a box can move along an axis, up to `desired`, before one of the
// boxes stops it. Boxes it already overlaps on that axis do not stop it.
double clip(Axis axis, const Aabb& box, const std::vector<Aabb>& boxes, double desired) {
    const auto overlaps = [&](const Aabb& s, Axis a) { return high(s, a) > low(box, a) + epsilon && low(s, a) < high(box, a) - epsilon; };
    for (const auto& s : boxes) {
        if (std::abs(desired) < epsilon) return 0.0;
        bool across = true;
        for (const auto other : {Axis::x, Axis::y, Axis::z})
            if (other != axis && !overlaps(s, other)) across = false;
        if (!across) continue;
        if (desired > 0.0 && low(s, axis) >= high(box, axis) - epsilon) desired = std::min(desired, low(s, axis) - high(box, axis));
        else if (desired < 0.0 && high(s, axis) <= low(box, axis) + epsilon) desired = std::max(desired, high(s, axis) - low(box, axis));
    }
    return std::abs(desired) < epsilon ? 0.0 : desired;
}

// Moves along the vertical axis first, then along the horizontal axis with
// the larger movement, then the other.
Vec3 collide_with(Vec3 movement, const Aabb& box, const std::vector<Aabb>& boxes) {
    if (boxes.empty()) return movement;
    const auto order = std::abs(movement.x) < std::abs(movement.z) ? std::array{Axis::y, Axis::z, Axis::x}
                                                                   : std::array{Axis::y, Axis::x, Axis::z};
    Vec3 done{};
    for (const auto axis : order) {
        const double d = get(movement, axis);
        if (d != 0.0) set(done, axis, clip(axis, shifted(box, done), boxes, d));
    }
    return done;
}

std::vector<Aabb> flatten(const std::vector<Collider>& colliders) {
    std::vector<Aabb> out;
    for (const auto& c : colliders) out.insert(out.end(), c.boxes.begin(), c.boxes.end());
    return out;
}

double horizontal(Vec3 v) noexcept { return v.x * v.x + v.z * v.z; }
} // namespace

Aabb Aabb::expand_towards(double dx, double dy, double dz) const noexcept {
    Aabb b = *this;
    (dx < 0 ? b.min_x : b.max_x) += dx;
    (dy < 0 ? b.min_y : b.max_y) += dy;
    (dz < 0 ? b.min_z : b.max_z) += dz;
    return b;
}

Aabb player_box(Vec3 feet, bool crouching) noexcept {
    const double half = player_width / 2.0;
    return {feet.x - half, feet.y, feet.z - half, feet.x + half, feet.y + (crouching ? crouching_height : standing_height), feet.z + half};
}

std::vector<Collider> colliders(const Aabb& area, const ShapeLookup& lookup) {
    std::vector<Collider> out;
    for_each_cell(area, [&](contracts::BlockPos p) {
        const auto shape = lookup(p);
        if (shape.boxes.empty()) return;
        Collider c{p, {}};
        bool hit = false;
        for (const auto& b : shape.boxes) {
            const Aabb w{p.x + b.min_x, p.y + b.min_y, p.z + b.min_z, p.x + b.max_x, p.y + b.max_y, p.z + b.max_z};
            hit |= w.intersects(area);
            c.boxes.push_back(w);
        }
        if (hit) out.push_back(std::move(c));
    });
    return out;
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
