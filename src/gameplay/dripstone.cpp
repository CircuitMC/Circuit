// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Pointed dripstone (both official servers; dripstone_shapes,
// dripstone_growth; minecraft.wiki "Pointed Dripstone"):
// - a piece pointing in direction d stands on the block behind it (against
//   d): a face of it that is sturdy toward d, or pointed dripstone pointing d;
// - its thickness, set on every update from above or below, is tip_merge or
//   tip when the piece in front of it points back at it (tip_merge when it
//   was tip_merge or that piece is), tip when nothing pointing d is in front,
//   frustum when the piece in front is a tip, and else base when nothing
//   pointing d is behind it and middle when something is;
// - an update from behind that finds it unsupported gives a stalagmite a
//   block tick a tick later, which breaks it with its drops, and a
//   stalactite (unless it has a tick already) one two ticks later, which
//   turns it and every piece below it into falling blocks, which break into
//   their items where they land.
namespace circuit::gameplay::detail {
namespace {
std::string_view name_of(BlockState s) {
    const auto state = physics::registry_state_of(s);
    const auto block = state ? registry::block_of(*state) : std::nullopt;
    return block ? registry::game_name(*block) : std::string_view{};
}

std::string value_of(BlockState s, std::string_view key) {
    const auto state = physics::registry_state_of(s);
    if (!state) return {};
    for (const auto& [k, v] : registry::properties(*state))
        if (k == key) return v;
    return {};
}

BlockState with(BlockState s, std::string_view key, const std::string& value) {
    auto props = registry::properties(s.registry_state());
    for (auto& [k, v] : props)
        if (k == key) v = value;
    return BlockState::registered(registry::state_of(*registry::block_of(s.registry_state()), props).value());
}

Direction tip_direction(BlockState s) { return value_of(s, "vertical_direction") == "up" ? Direction::up : Direction::down; }

bool pointing(BlockState s, Direction d) { return is_pointed_dripstone(s) && tip_direction(s) == d; }

std::string thickness(RuleContext& ctx, BlockPos pos, Direction d, bool merge_tip) {
    const auto back = contracts::opposite(d);
    const auto front = ctx.block(pos.relative(d));
    if (pointing(front, back)) return !merge_tip && value_of(front, "thickness") != "tip_merge" ? "tip" : "tip_merge";
    if (!pointing(front, d)) return "tip";
    const auto t = value_of(front, "thickness");
    if (t == "tip" || t == "tip_merge") return "frustum";
    return pointing(ctx.block(pos.relative(back)), d) ? "middle" : "base";
}
// A tip that drips and grows: tip, not merged, not waterlogged.
bool drips(BlockState s) { return is_pointed_dripstone(s) && value_of(s, "thickness") == "tip" && value_of(s, "waterlogged") != "true"; }

bool unmerged_tip(BlockState s, Direction d) { return pointing(s, d) && value_of(s, "thickness") == "tip"; }

bool holds_fluid(BlockState s) { return holds_water_fluid(s) || is_lava(s); }

// Whether a drip falls through a cell: air, or a block that is not a full
// solid one, holds no fluid and leaves the middle of the cell open.
bool drips_through(RuleContext& ctx, BlockState s) {
    if (s.is_air()) return true;
    const auto state = physics::registry_state_of(s);
    if (!state || registry::block(*registry::block_of(*state)).opaque || holds_fluid(s)) return false;
    for (const auto& b : registry::collision_boxes(*state, ctx.bundle().schema.content_release))
        if (b.max_x > 0.375 && b.min_x < 0.625 && b.max_z > 0.375 && b.min_z < 0.625 && b.max_y > b.min_y) return false;
    return true;
}

// The tip of a stalactite from its top piece, within `reach` cells.
std::optional<BlockPos> tip_below(RuleContext& ctx, BlockPos top, int reach) {
    auto p = top;
    for (int i = 0; i < reach; ++i) {
        const auto s = ctx.block(p);
        if (!pointing(s, Direction::down)) return std::nullopt;
        if (value_of(s, "thickness") == "tip") return p;
        p = p.relative(Direction::down);
    }
    return std::nullopt;
}

// The fluid above the block a stalactite hangs from: water (also for mud),
// lava, or none; with the block it hangs from.
enum class DripFluid { none, water, lava };
DripFluid fluid_above_root(RuleContext& ctx, BlockPos piece, BlockPos* root_out = nullptr) {
    auto p = piece;
    for (int i = 0; i < 11 && pointing(ctx.block(p), Direction::down); ++i) p = p.relative(Direction::up);
    if (pointing(ctx.block(p), Direction::down)) return DripFluid::none;
    if (root_out) *root_out = p;
    const auto above = ctx.block(p.relative(Direction::up));
    if (name_of(above) == "mud") return DripFluid::water;
    if (above.is(BlockType::water) && above.level() == 0 && !above.flag()) return DripFluid::water;
    if (is_lava_source(above)) return DripFluid::lava;
    return DripFluid::none;
}

bool receives(BlockState cauldron, DripFluid f) {
    const auto n = name_of(cauldron);
    if (n == "cauldron") return f != DripFluid::none;
    if (n == "water_cauldron") return f == DripFluid::water && value_of(cauldron, "level") != "3";
    return false;
}

void place_piece(RuleContext& ctx, BlockPos pos, Direction d, std::string_view t) {
    const auto there = ctx.block(pos);
    const std::vector<registry::Property> props{{"thickness", std::string(t)}, {"vertical_direction", d == Direction::up ? "up" : "down"},
                                                {"waterlogged", there.is(BlockType::water) && there.level() == 0 && !there.flag() ? "true" : "false"}};
    set_block(ctx, pos, BlockState::registered(registry::state_of(*registry::find_block(ctx.bundle().schema.content_release, "pointed_dripstone"), props).value()));
}

// A tip's next cell: air or a tip pointing back, with no fluid.
bool tip_can_grow(RuleContext& ctx, BlockPos tip, BlockState s) {
    const auto d = tip_direction(s);
    const auto next = ctx.block(tip.relative(d));
    if (holds_fluid(next)) return false;
    return next.is_air() || unmerged_tip(next, contracts::opposite(d));
}

void grow_from(RuleContext& ctx, BlockPos pos, Direction d) {
    const auto at = pos.relative(d);
    const auto there = ctx.block(at);
    if (unmerged_tip(there, contracts::opposite(d))) {
        const auto upper = tip_direction(there) == Direction::up ? at.relative(Direction::up) : at;
        place_piece(ctx, upper, Direction::down, "tip_merge");
        place_piece(ctx, upper.relative(Direction::down), Direction::up, "tip_merge");
    } else if (there.is_air() || there.is(BlockType::water)) {
        place_piece(ctx, at, d, "tip");
    }
}

void grow_stalagmite_under(RuleContext& ctx, BlockPos tip) {
    auto p = tip;
    for (int i = 0; i < 10; ++i) {
        p = p.relative(Direction::down);
        const auto s = ctx.block(p);
        if (holds_fluid(s)) return;
        if (unmerged_tip(s, Direction::up) && tip_can_grow(ctx, p, s)) {
            grow_from(ctx, p, Direction::up);
            return;
        }
        const auto below = ctx.block(p.relative(Direction::down));
        const auto state = physics::registry_state_of(below);
        const bool stands = pointing(below, Direction::up)
                         || (state && registry::sturdy_face(*state, Direction::up, ctx.bundle().schema.content_release));
        if (stands && !holds_water_fluid(below)) {
            grow_from(ctx, p.relative(Direction::down), Direction::up);
            return;
        }
        if (!drips_through(ctx, s)) return;
    }
}
} // namespace

bool is_pointed_dripstone(BlockState s) { return s.is(BlockType::other) && name_of(s) == "pointed_dripstone"; }

// Its random tick draws two floats: the first drips, the second grows.
// Dripping (the top piece of a stalactite): with water above the block it
// hangs from (a source, or mud) and the float below 45/256, or lava and
// below 15/256, the drip from its tip turns mud into clay, or gives the first
// cauldron below the tip within 11 cells (through cells drips pass) that
// takes the fluid a block tick 50 + its distance later. Growing (the top
// piece, the float below 64/5625): under a dripstone block with a water
// source above, a tip within 7 cells that can grow draws nextBoolean and
// grows a piece down (merging with a stalagmite's tip) or raises a stalagmite
// on the first floor within 10 cells below.
void dripstone_random_tick(RuleContext& ctx, BlockPos pos, BlockState s) {
    const float drip = ctx.random_float();
    const bool top = pointing(s, Direction::down) && !is_pointed_dripstone(ctx.block(pos.relative(Direction::up)));
    if (drip <= 0.17578125F && top) {
        BlockPos root{};
        const auto f = fluid_above_root(ctx, pos, &root);
        const float chance = f == DripFluid::water ? 0.17578125F : 0.05859375F;
        if (f != DripFluid::none && drip < chance) {
            if (const auto tip = tip_below(ctx, pos, 11)) {
                if (f == DripFluid::water && name_of(ctx.block(root.relative(Direction::up))) == "mud") {
                    set_block(ctx, root.relative(Direction::up), BlockState::registered(registry::block(
                        *registry::find_block(ctx.bundle().schema.content_release, "clay")).default_state));
                } else {
                    auto p = *tip;
                    for (int i = 1; i < 11; ++i) {
                        p = p.relative(Direction::down);
                        if (!ctx.world().contains_y(p.y)) break;
                        const auto c = ctx.block(p);
                        if (receives(c, f)) {
                            schedule(ctx, TickList::block, p, BlockType::other, static_cast<std::uint32_t>(50 + tip->y - p.y), 0);
                            break;
                        }
                        if (!drips_through(ctx, c)) break;
                    }
                }
            }
        }
    }
    if (!(ctx.random_float() < 0.011377778F) || !top) return;
    const auto above = ctx.block(pos.relative(Direction::up));
    const auto above2 = ctx.block({pos.x, pos.y + 2, pos.z});
    if (name_of(above) != "dripstone_block" || !(above2.is(BlockType::water) && above2.level() == 0 && !above2.flag())) return;
    const auto tip = tip_below(ctx, pos, 7);
    if (!tip) return;
    const auto t = ctx.block(*tip);
    if (!drips(t) || !tip_can_grow(ctx, *tip, t)) return;
    if (ctx.random_int(2) == 1) grow_from(ctx, *tip, Direction::down);
    else grow_stalagmite_under(ctx, *tip);
}

bool is_cauldron(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    const auto n = name_of(s);
    return n == "cauldron" || n == "water_cauldron" || n == "lava_cauldron" || n == "powder_snow_cauldron";
}

// A cauldron's block tick: the drip of the stalactite tip above it within 11
// cells fills it, water a level, lava whole.
void tick_cauldron(RuleContext& ctx, BlockPos pos, BlockState s) {
    auto p = pos;
    for (int i = 1; i < 11; ++i) {
        p = p.relative(Direction::up);
        if (!ctx.world().contains_y(p.y)) return;
        const auto c = ctx.block(p);
        if (drips(c)) {
            const auto f = fluid_above_root(ctx, p);
            if (!receives(s, f)) return;
            const auto release = ctx.bundle().schema.content_release;
            if (f == DripFluid::lava) {
                set_block(ctx, pos, BlockState::registered(registry::block(*registry::find_block(release, "lava_cauldron")).default_state));
            } else {
                const int level = name_of(s) == "water_cauldron" ? std::stoi(value_of(s, "level")) + 1 : 1;
                const std::vector<registry::Property> props{{"level", std::to_string(level)}};
                set_block(ctx, pos, BlockState::registered(registry::state_of(*registry::find_block(release, "water_cauldron"), props).value()));
            }
            return;
        }
        if (!drips_through(ctx, c)) return;
    }
}

bool dripstone_supported(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto d = tip_direction(s);
    const auto behind = ctx.block(pos.relative(contracts::opposite(d)));
    if (pointing(behind, d)) return true;
    const auto state = physics::registry_state_of(behind);
    return state && registry::sturdy_face(*state, d, ctx.bundle().schema.content_release);
}

void dripstone_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction from) {
    if (from != Direction::up && from != Direction::down) return;
    const auto d = tip_direction(s);
    if (d == Direction::down && ctx.has_tick(TickList::block, pos, BlockType::other)) return;
    if (from == contracts::opposite(d) && !dripstone_supported(ctx, pos, s)) {
        schedule(ctx, TickList::block, pos, BlockType::other, d == Direction::down ? 2 : 1, 0);
        return;
    }
    const auto t = thickness(ctx, pos, d, value_of(s, "thickness") == "tip_merge");
    if (t != value_of(s, "thickness")) set_block_quiet(ctx, pos, with(s, "thickness", t));
}

void tick_dripstone(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (tip_direction(s) == Direction::up) {
        if (!dripstone_supported(ctx, pos, s)) destroy_block(ctx, pos);
        return;
    }
    // Every piece of the stalactite from here down, top first.
    for (auto p = pos;; p = p.relative(Direction::down)) {
        const auto piece = ctx.block(p);
        if (!pointing(piece, Direction::down)) return;
        spawn_falling_block(ctx, p, piece);
        const auto t = value_of(piece, "thickness");
        if (t == "tip" || t == "tip_merge") return;
    }
}

} // namespace circuit::gameplay::detail
