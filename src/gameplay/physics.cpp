// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/gameplay/physics.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace circuit::gameplay::physics {
namespace {
constexpr double epsilon = 1.0e-7;

enum class Axis : std::uint8_t { x, y, z };

double low(const Aabb& b, Axis a) noexcept { return a == Axis::x ? b.min_x : a == Axis::y ? b.min_y : b.min_z; }
double high(const Aabb& b, Axis a) noexcept { return a == Axis::x ? b.max_x : a == Axis::y ? b.max_y : b.max_z; }
double get(Vec3 v, Axis a) noexcept { return a == Axis::x ? v.x : a == Axis::y ? v.y : v.z; }
void set(Vec3& v, Axis a, double d) noexcept { (a == Axis::x ? v.x : a == Axis::y ? v.y : v.z) = d; }

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

// Modeled states through the registry states they stand for.
const std::unordered_map<std::uint32_t, std::uint32_t>& modeled_registry_states() {
    static const auto table = [] {
        std::unordered_map<std::uint32_t, std::uint32_t> out;
        for_each_modeled_state([&](BlockState s) {
            if (const auto state = registry_equivalent(s)) out.emplace(s.raw(), *state);
        });
        return out;
    }();
    return table;
}
} // namespace

std::optional<std::uint32_t> registry_state_of(BlockState s) {
    if (s.is(BlockType::other)) return s.registry_state();
    const auto& modeled = modeled_registry_states();
    const auto it = modeled.find(s.raw());
    return it == modeled.end() ? std::nullopt : std::optional(it->second);
}

Aabb Aabb::expand_towards(double dx, double dy, double dz) const noexcept {
    Aabb b = *this;
    (dx < 0 ? b.min_x : b.max_x) += dx;
    (dy < 0 ? b.min_y : b.max_y) += dy;
    (dz < 0 ? b.min_z : b.max_z) += dz;
    return b;
}

Aabb box_at(Vec3 feet, double width, double height) noexcept {
    const double half = width / 2.0;
    return {feet.x - half, feet.y, feet.z - half, feet.x + half, feet.y + height, feet.z + half};
}

std::span<const registry::Box> collision_boxes(BlockState s, std::string_view release) {
    if (s.is_unloaded()) return {};
    if (s.is(BlockType::other)) return registry::collision_boxes(s.registry_state(), release);
    const auto& modeled = modeled_registry_states();
    const auto it = modeled.find(s.raw());
    return it == modeled.end() ? std::span<const registry::Box>{} : registry::collision_boxes(it->second, release);
}

std::vector<Collider> colliders(const Aabb& area, const ShapeAt& shapes) {
    std::vector<Collider> out;
    for_each_cell(area, [&](contracts::BlockPos p) {
        const auto boxes = shapes(p);
        if (boxes.empty()) return;
        Collider c{p, {}};
        bool hit = false;
        for (const auto& b : boxes) {
            const Aabb w{p.x + b.min_x, p.y + b.min_y, p.z + b.min_z, p.x + b.max_x, p.y + b.max_y, p.z + b.max_z};
            hit |= w.intersects(area);
            c.boxes.push_back(w);
        }
        if (hit) out.push_back(std::move(c));
    });
    return out;
}

std::vector<Aabb> flatten(const std::vector<Collider>& colliders) {
    std::vector<Aabb> out;
    for (const auto& c : colliders) out.insert(out.end(), c.boxes.begin(), c.boxes.end());
    return out;
}

Vec3 collide_with(Vec3 movement, const Aabb& box, const std::vector<Aabb>& boxes) {
    if (boxes.empty()) return movement;
    const auto order = std::abs(movement.x) < std::abs(movement.z) ? std::array{Axis::y, Axis::z, Axis::x}
                                                                   : std::array{Axis::y, Axis::x, Axis::z};
    Vec3 done{};
    for (const auto axis : order) {
        const double d = get(movement, axis);
        if (d != 0.0) set(done, axis, clip(axis, box.move(done), boxes, d));
    }
    return done;
}

Vec3 collide(const Aabb& box, Vec3 movement, const ShapeAt& shapes) {
    if (movement.x == 0.0 && movement.y == 0.0 && movement.z == 0.0) return movement;
    return collide_with(movement, box, flatten(colliders(box.expand_towards(movement.x, movement.y, movement.z), shapes)));
}

} // namespace circuit::gameplay::physics
