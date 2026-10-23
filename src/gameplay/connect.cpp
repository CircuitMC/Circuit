// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <array>
#include <string>
#include <string_view>

// Fences, panes (glass panes and iron bars) and walls, as both official
// servers join them (a report of every state beside and above each kind,
// tests/fixtures/collision/connection-*.tsv; minecraft.wiki "Fence", "Glass
// Pane", "Bars" and "Wall"). A fence or pane joins its neighbor on a side
// when the neighbor is a connector it joins (registry::Connector), a fence
// gate turned across that side (fences and walls), or a block whose face
// toward it is whole (registry::joins_face). A shape update from a side sets
// that side; a wall then takes all its heights and its post again, as it
// does after a shape update from above:
// - a joined side is tall when the bottom of the collision shape above
//   covers the strip from 7 to 9 sixteenths across, from 7 to that edge;
//   low otherwise;
// - its post stays under a wall with a post, and when its joined sides are
//   not exactly two opposite ones or all four; otherwise it goes when two
//   opposite sides are tall, and stays only under blocks that keep it
//   (registry::keeps_wall_post) or whose bottom covers the middle (7 to 9
//   both ways).
// Fences and panes ignore updates from above and below, walls from below. A
// block a piston sets down takes its shape from all its neighbors. Water in
// waterlogged ones is stored only.
namespace circuit::gameplay::detail {
namespace {
using registry::Connector;

// North, east, south, west: the order of `joined` below.
constexpr std::array<Direction, 4> sides{Direction::north, Direction::east, Direction::south, Direction::west};
constexpr std::array<std::string_view, 4> side_names{"north", "east", "south", "west"};

Connector kind_of(BlockState s) {
    return s.is(BlockType::other) ? registry::connector(s.registry_state()) : Connector::none;
}

std::string value_of(BlockState s, std::string_view key) {
    for (const auto& [k, v] : registry::properties(s.registry_state()))
        if (k == key) return v;
    return {};
}

BlockState with_values(BlockState s, const std::vector<registry::Property>& changes) {
    auto props = registry::properties(s.registry_state());
    for (auto& [k, v] : props)
        for (const auto& [ck, cv] : changes)
            if (k == ck) v = cv;
    const auto block = registry::block_of(s.registry_state());
    const auto state = block ? registry::state_of(*block, props) : std::nullopt;
    return state ? BlockState::registered(*state) : s;
}

// Whether the bottom of a collision shape covers a rectangle of the cell's
// floor, in sixteenths.
bool covers(std::span<const registry::Box> boxes, int x0, int z0, int x1, int z1) {
    for (int x = x0; x < x1; ++x)
        for (int z = z0; z < z1; ++z) {
            const double cx = (x + 0.5) / 16.0, cz = (z + 0.5) / 16.0;
            bool in = false;
            for (const auto& b : boxes)
                if (b.min_y <= 0.0 && b.max_y > 0.0 && b.min_x <= cx && cx <= b.max_x && b.min_z <= cz && cz <= b.max_z) in = true;
            if (!in) return false;
        }
    return true;
}

// The strip a wall's side rises under, in sixteenths (x0, z0, x1, z1).
constexpr std::array<std::array<int, 4>, 4> side_strips{{{7, 0, 9, 9}, {7, 7, 16, 9}, {7, 7, 9, 16}, {0, 7, 9, 9}}};

std::array<bool, 4> joined_sides(BlockState wall) {
    std::array<bool, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) out[i] = value_of(wall, side_names[i]) != "none";
    return out;
}

std::array<bool, 4> joined_from(RuleContext& ctx, BlockPos pos, BlockState s) {
    std::array<bool, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) out[i] = connector_joins(s, as_settled(ctx, pos.relative(sides[i])), sides[i]);
    return out;
}

BlockState wall_from(RuleContext& ctx, BlockPos pos, BlockState s, std::array<bool, 4> joined) {
    return wall_under(s, joined, as_settled(ctx, pos.relative(Direction::up)), ctx.bundle().schema.content_release);
}

BlockState fence_with(BlockState s, std::array<bool, 4> joined) {
    std::vector<registry::Property> changes;
    for (std::size_t i = 0; i < 4; ++i) changes.emplace_back(std::string(side_names[i]), joined[i] ? "true" : "false");
    return with_values(s, changes);
}
} // namespace

bool is_connector(BlockState s) { return kind_of(s) != Connector::none; }

BlockState joined_state(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto joined = joined_from(ctx, pos, s);
    return kind_of(s) == Connector::wall ? wall_from(ctx, pos, s, joined) : fence_with(s, joined);
}

void connector_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d) {
    BlockState next = s;
    if (kind_of(s) == Connector::wall) {
        if (d == Direction::down) return;
        auto joined = joined_sides(s);
        for (std::size_t i = 0; i < 4; ++i)
            if (sides[i] == d) joined[i] = connector_joins(s, as_settled(ctx, pos.relative(d)), d);
        next = wall_from(ctx, pos, s, joined);
    } else {
        if (!contracts::is_horizontal(d)) return;
        const bool join = connector_joins(s, as_settled(ctx, pos.relative(d)), d);
        for (std::size_t i = 0; i < 4; ++i)
            if (sides[i] == d) next = with_values(s, {{std::string(side_names[i]), join ? "true" : "false"}});
    }
    if (next != s) set_block_quiet(ctx, pos, next);
}

} // namespace circuit::gameplay::detail

namespace circuit::gameplay {
using namespace detail;

bool connector_joins(BlockState connector, BlockState neighbor, contracts::Direction side) {
    const auto kind = kind_of(connector);
    const auto other = kind_of(neighbor);
    const bool fence = kind == Connector::wooden_fence || kind == Connector::nether_brick_fence;
    switch (kind) {
    case Connector::wooden_fence: case Connector::nether_brick_fence:
        if (other == kind) return true;
        break;
    case Connector::pane: case Connector::wall:
        if (other == Connector::pane || other == Connector::wall) return true;
        break;
    default: return false;
    }
    // A fence gate turned across the side joins fences and walls.
    if ((fence || kind == Connector::wall) && neighbor.is(BlockType::fence_gate)
        && (contracts::clockwise(neighbor.facing()) == side || contracts::counter_clockwise(neighbor.facing()) == side))
        return true;
    const auto state = physics::registry_state_of(neighbor);
    return state && registry::joins_face(*state, contracts::opposite(side));
}

BlockState wall_under(BlockState wall, std::array<bool, 4> joined, BlockState above, std::string_view release) {
    const auto boxes = physics::collision_boxes(above, release);
    std::array<bool, 4> tall{};
    std::vector<registry::Property> changes;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& r = side_strips[i];
        tall[i] = joined[i] && covers(boxes, r[0], r[1], r[2], r[3]);
        changes.emplace_back(std::string(side_names[i]), !joined[i] ? "none" : tall[i] ? "tall" : "low");
    }
    bool post = true;
    const bool wall_post_above = kind_of(above) == Connector::wall && value_of(above, "up") == "true";
    const bool straight_or_cross = (joined[0] || joined[1] || joined[2] || joined[3]) && joined[0] == joined[2] && joined[1] == joined[3];
    if (!wall_post_above && straight_or_cross) {
        if ((tall[0] && tall[2]) || (tall[1] && tall[3])) {
            post = false;
        } else {
            const auto state = physics::registry_state_of(above);
            post = (state && registry::keeps_wall_post(*state)) || covers(boxes, 7, 7, 9, 9);
        }
    }
    changes.emplace_back("up", post ? "true" : "false");
    return with_values(wall, changes);
}

} // namespace circuit::gameplay
