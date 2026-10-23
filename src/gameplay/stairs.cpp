// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <array>
#include <string>
#include <string_view>
#include <vector>

// Stairs, as both official servers shape them (a report of every
// combination of stairs on their four sides, tests/fixtures/collision/
// stairs.txt; minecraft.wiki "Stairs"). Stairs of any material join stairs
// of the same half whose facing is turned a quarter from theirs:
// - the stairs on their full side (toward `facing`) make an outer corner,
//   unless the stairs behind that one's half side face as they do;
// - otherwise the stairs on their half side make an inner corner, unless the
//   stairs toward that one's facing face as they do;
// left when the joining stairs face counter-clockwise of theirs, right
// otherwise. A shape update from a side and a piston setting them down give
// them their shape again; updates from above and below do not.
namespace circuit::gameplay::detail {
namespace {
// North, east, south, west.
constexpr std::array<Direction, 4> sides{Direction::north, Direction::east, Direction::south, Direction::west};
constexpr std::array<std::string_view, 4> side_names{"north", "east", "south", "west"};

const std::vector<bool>& stairs_blocks() {
    static const std::vector<bool> out = [] {
        std::vector<bool> v(registry::block_count(), false);
        for (std::uint32_t b = 0; b < v.size(); ++b) v[b] = registry::block(b).name.ends_with("_stairs");
        return v;
    }();
    return out;
}

struct Stair {
    Direction facing;
    bool top;
};

std::optional<Stair> stair_of(BlockState s) {
    if (!is_stairs(s)) return std::nullopt;
    Stair out{Direction::north, false};
    for (const auto& [k, v] : registry::properties(s.registry_state())) {
        if (k == "half") out.top = v == "top";
        if (k == "facing")
            for (std::size_t i = 0; i < 4; ++i)
                if (side_names[i] == v) out.facing = sides[i];
    }
    return out;
}

bool same_axis(Direction a, Direction b) { return a == b || a == contracts::opposite(b); }
} // namespace

bool is_stairs(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    const auto block = registry::block_of(s.registry_state());
    return block && stairs_blocks()[*block];
}

BlockState shaped_stairs(RuleContext& ctx, BlockPos pos, BlockState s) {
    std::array<BlockState, 4> around{};
    for (std::size_t i = 0; i < 4; ++i) around[i] = as_settled(ctx, pos.relative(sides[i]));
    return stairs_with_neighbors(s, around);
}

void stairs_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d) {
    if (!contracts::is_horizontal(d)) return;
    const auto next = shaped_stairs(ctx, pos, s);
    if (next != s) set_block_quiet(ctx, pos, next);
}

} // namespace circuit::gameplay::detail

namespace circuit::gameplay {
using namespace detail;

BlockState stairs_with_neighbors(BlockState stairs, std::array<BlockState, 4> around) {
    const auto self = stair_of(stairs);
    if (!self) return stairs;
    const auto at = [&](Direction d) {
        for (std::size_t i = 0; i < 4; ++i)
            if (sides[i] == d) return around[i];
        return around[0];
    };
    // Whether the block on side `d` is not stairs facing and placed as these are.
    const auto free = [&](Direction d) {
        const auto o = stair_of(at(d));
        return !o || o->facing != self->facing || o->top != self->top;
    };
    std::string shape = "straight";
    const auto front = stair_of(at(self->facing));
    const auto back = stair_of(at(contracts::opposite(self->facing)));
    if (front && front->top == self->top && !same_axis(front->facing, self->facing) && free(contracts::opposite(front->facing))) {
        shape = front->facing == contracts::counter_clockwise(self->facing) ? "outer_left" : "outer_right";
    } else if (back && back->top == self->top && !same_axis(back->facing, self->facing) && free(back->facing)) {
        shape = back->facing == contracts::counter_clockwise(self->facing) ? "inner_left" : "inner_right";
    }
    auto props = registry::properties(stairs.registry_state());
    for (auto& [k, v] : props)
        if (k == "shape") v = shape;
    const auto block = registry::block_of(stairs.registry_state());
    const auto state = block ? registry::state_of(*block, props) : std::nullopt;
    return state ? BlockState::registered(*state) : stairs;
}

} // namespace circuit::gameplay
