// SPDX-License-Identifier: AGPL-3.0-only
// Buckets, as both official servers use them (circuit-clienttest/buckets.js
// and the dispenser traces): what a filled bucket pours into a cell, what an
// empty one takes from it, and a player's use of one along their view.
//
// A cell takes poured water when it holds water as a property (a block with
// a waterlogged property but double slabs and barriers); it stays as it is
// when it already has water, as kelp and seagrass do. Otherwise water and
// lava go into air and into the blocks fluids replace: replaceable blocks
// and those that are not solid (registry::solid), but end portals and end
// gateways. What they replace breaks with its drops first, unless it is a
// fluid. Water poured into a block moves on from it five ticks later
// (waterlogged_flow). An empty bucket takes a water or lava source, a bubble
// column or powder snow, leaving air, or the water of a waterlogged block.
//
// A player's empty bucket follows the view through the fluid sources before
// the block it meets: water and lava sources, waterlogged blocks, bubble
// columns, kelp and seagrass, each filling its cell up to 8/9 of a block, or
// all of it under the same fluid. It takes from the first one the view
// enters, or else from that block. A filled bucket goes into the block met
// when that block holds water as a property, or is kelp or seagrass, and the
// bucket holds water; else, or when the block refuses it, beside it on the
// face met. In survival a poured bucket becomes an empty one; a filled one
// replaces a single empty bucket in the hand, or goes into the inventory
// (where it does not fit, it drops at the player). A creative player keeps
// the empty bucket and gets the filled one unless they have one already.
#include "internal.hpp"
#include <cmath>
#include <numbers>
#include <string>

namespace circuit::gameplay::detail {
namespace {
using contracts::ErrorCode;
using contracts::Vec3;

std::string_view name_of(BlockState s) {
    if (!s.is(BlockType::other)) return {};
    const auto block = registry::block_of(s.registry_state());
    return block ? std::string_view(registry::block(*block).name) : std::string_view{};
}

std::string property_of(BlockState s, std::string_view key) {
    if (!s.is(BlockType::other)) return {};
    for (auto& [k, v] : registry::properties(s.registry_state()))
        if (k == key) return std::move(v);
    return {};
}

bool has_water_property(BlockState s) {
    switch (s.type()) {
    case BlockType::coral_fan: case BlockType::coral_wall_fan: case BlockType::chest: case BlockType::ender_chest: case BlockType::trapdoor:
    case BlockType::rail:
        return true;
    case BlockType::other:
        for (const auto& [key, value] : registry::properties(s.registry_state()))
            if (key == "waterlogged") return true;
        return false;
    default: return false;
    }
}
bool water_plant(BlockState s) {
    const auto name = name_of(s);
    return name == "kelp" || name == "kelp_plant" || name == "seagrass" || name == "tall_seagrass";
}
// Blocks a water bucket aims into rather than beside.
bool holds_liquid(BlockState s) { return has_water_property(s) || water_plant(s); }
// Blocks that take poured water as a property.
bool takes_water(BlockState s) {
    if (!has_water_property(s)) return false;
    return name_of(s) != "barrier" && property_of(s, "type") != "double";
}
// Blocks fluids replace.
bool fluid_replaces(BlockState s) {
    if (s.is_air() || s.is(BlockType::water) || is_lava(s)) return true;
    const auto name = name_of(s);
    if (name == "end_portal" || name == "end_gateway") return false;
    const auto state = physics::registry_state_of(s);
    return block_replaceable(s) || (state && !registry::solid(*state));
}

enum class Pour : std::uint8_t { refused, keep, wet, replace };
Pour pour_of(BlockState s, bool lava) {
    if (!lava && takes_water(s)) return waterlogged(s) ? Pour::keep : Pour::wet;
    if (!lava && water_plant(s)) return Pour::keep;
    return fluid_replaces(s) ? Pour::replace : Pour::refused;
}

bool loaded(RuleContext& ctx, BlockPos pos) { return ctx.world().contains_y(pos.y) && !ctx.block(pos).is_unloaded(); }

// What an empty bucket can take from a block: the bucket it fills.
std::optional<std::uint16_t> taken_from(BlockState s) {
    if ((s.is(BlockType::water) && s.level() == 0 && !s.flag()) || name_of(s) == "bubble_column") return items::water_bucket;
    if (is_lava_source(s)) return items::lava_bucket;
    if (name_of(s) == "powder_snow") return items::powder_snow_bucket;
    if (waterlogged(s) && with_water(s, false)) return items::water_bucket;
    return std::nullopt;
}

// A state the view's fluid test meets: a source of water or lava, filling
// its cell up to 8/9 of a block (all of it under the same fluid).
std::optional<double> fluid_height(RuleContext& ctx, BlockPos p, BlockState s) {
    const bool water = (s.is(BlockType::water) && s.level() == 0 && !s.flag()) || waterlogged(s) || water_plant(s)
                    || name_of(s) == "bubble_column";
    if (!water && !is_lava_source(s)) return std::nullopt;
    const auto above = ctx.block(p.relative(Direction::up));
    const bool same = water ? (above.is(BlockType::water) || holds_water_fluid(above)) : is_lava(above);
    return same ? 1.0 : static_cast<double>(8.0f / 9.0f);
}

// Where the segment from `a` to `b` enters the box, as a fraction of it.
std::optional<double> enters(Vec3 a, Vec3 b, Vec3 lo, Vec3 hi) {
    double t0 = 0.0, t1 = 1.0;
    const double from[3]{a.x, a.y, a.z}, d[3]{b.x - a.x, b.y - a.y, b.z - a.z}, low[3]{lo.x, lo.y, lo.z}, high[3]{hi.x, hi.y, hi.z};
    for (int i = 0; i < 3; ++i) {
        if (d[i] == 0.0) {
            if (from[i] < low[i] || from[i] > high[i]) return std::nullopt;
            continue;
        }
        double u = (low[i] - from[i]) / d[i], v = (high[i] - from[i]) / d[i];
        if (u > v) std::swap(u, v);
        t0 = std::max(t0, u);
        t1 = std::min(t1, v);
        if (t0 > t1) return std::nullopt;
    }
    return t0;
}

// The first cell whose fluid source the segment from `a` to `b` meets, by
// the cells it passes through in order.
std::optional<BlockPos> fluid_on(RuleContext& ctx, Vec3 a, Vec3 b) {
    const auto cell = [](double v) { return static_cast<std::int32_t>(std::floor(v)); };
    BlockPos p{cell(a.x), cell(a.y), cell(a.z)};
    const BlockPos last{cell(b.x), cell(b.y), cell(b.z)};
    const double d[3]{b.x - a.x, b.y - a.y, b.z - a.z};
    const double start[3]{a.x, a.y, a.z};
    int step[3];
    double next[3], delta[3];
    for (int i = 0; i < 3; ++i) {
        const std::int32_t c = i == 0 ? p.x : i == 1 ? p.y : p.z;
        step[i] = d[i] > 0 ? 1 : d[i] < 0 ? -1 : 0;
        delta[i] = d[i] != 0 ? std::abs(1.0 / d[i]) : INFINITY;
        next[i] = d[i] > 0 ? (c + 1 - start[i]) / d[i] : d[i] < 0 ? (c - start[i]) / d[i] : INFINITY;
    }
    for (int guard = 0; guard < 256; ++guard) {
        if (!ctx.world().contains_y(p.y)) {
            if ((step[1] >= 0 && p.y >= ctx.world().min_y + ctx.world().height) || (step[1] <= 0 && p.y < ctx.world().min_y)) return std::nullopt;
        } else if (const auto s = ctx.block(p); !s.is_unloaded()) {
            if (const auto h = fluid_height(ctx, p, s))
                if (enters(a, b, {double(p.x), double(p.y), double(p.z)}, {p.x + 1.0, p.y + *h, p.z + 1.0})) return p;
        }
        if (p == last) return std::nullopt;
        const int axis = next[0] <= next[1] && next[0] <= next[2] ? 0 : next[1] <= next[2] ? 1 : 2;
        if (next[axis] > 1.0) return std::nullopt;
        (axis == 0 ? p.x : axis == 1 ? p.y : p.z) += step[axis];
        next[axis] += delta[axis];
    }
    return std::nullopt;
}

// The view's direction from yaw and pitch (degrees).
Vec3 view(float yaw, float pitch) {
    const double y = yaw * std::numbers::pi / 180.0, p = pitch * std::numbers::pi / 180.0;
    return {-std::sin(y) * std::cos(p), -std::sin(p), std::cos(y) * std::cos(p)};
}
} // namespace

std::optional<BlockState> with_water(BlockState s, bool wet) {
    switch (s.type()) {
    case BlockType::coral_fan: case BlockType::coral_wall_fan: case BlockType::chest: case BlockType::ender_chest: return s.with_flag(wet);
    case BlockType::trapdoor: return s.with_level(static_cast<std::uint8_t>((s.level() & 1) | (wet ? 2 : 0)));
    case BlockType::rail: return s.with_extra(wet ? 1 : 0);
    case BlockType::other: {
        auto props = registry::properties(s.registry_state());
        bool found = false;
        for (auto& [key, value] : props)
            if (key == "waterlogged") value = wet ? "true" : "false", found = true;
        if (!found) return std::nullopt;
        const auto block = registry::block_of(s.registry_state());
        const auto state = block ? registry::state_of(*block, props) : std::nullopt;
        if (!state) return std::nullopt;
        return BlockState::registered(*state);
    }
    default: return std::nullopt;
    }
}

bool pour_fluid(RuleContext& ctx, BlockPos pos, bool lava) {
    if (!loaded(ctx, pos)) return false;
    const auto s = ctx.block(pos);
    switch (pour_of(s, lava)) {
    case Pour::refused: return false;
    case Pour::keep: return true;
    case Pour::wet: {
        // The block's new water moves on its next fluid tick (waterlogged_flow).
        const auto wet = *with_water(s, true);
        set_block(ctx, pos, wet);
        schedule(ctx, TickList::fluid, pos, wet.type(), 5, 0);
        return true;
    }
    case Pour::replace:
        if (!s.is_air() && !s.is(BlockType::water) && !is_lava(s)) destroy_block(ctx, pos);
        set_block(ctx, pos, lava ? lava_source() : blocks::water_source());
        return true;
    }
    return false;
}

std::optional<std::uint16_t> take_fluid(RuleContext& ctx, BlockPos pos) {
    if (!loaded(ctx, pos)) return std::nullopt;
    const auto s = ctx.block(pos);
    const auto filled = taken_from(s);
    if (!filled) return std::nullopt;
    set_block(ctx, pos, waterlogged(s) && !s.is(BlockType::water) ? *with_water(s, false) : blocks::air());
    return filled;
}

contracts::Result<std::function<void()>> plan_use_item(RuleContext& ctx, const contracts::PlayerUseItem& c) {
    if (c.slot < contracts::hotbar_first || c.slot >= contracts::player_slots) return std::unexpected(ErrorCode::invalid_slot);
    const auto* inventory = ctx.peek_player(c.player);
    const auto held = inventory ? inventory->slots[c.slot] : ItemStack{};
    const auto end = [&](double t) {
        const auto d = view(c.yaw, c.pitch);
        return Vec3{c.eye.x + d.x * t, c.eye.y + d.y * t, c.eye.z + d.z * t};
    };
    // A knowledge book is used up outside creative mode, whatever recipes it
    // names (the server gives them; client/knowledge_books).
    if (static const auto knowledge = items::by_name("minecraft:knowledge_book"); knowledge && held.item == *knowledge && !held.empty())
        return [&ctx, c] {
            if (!c.survival) return;
            auto& inv = ctx.player(c.player);
            auto& slot = inv.slots[c.slot];
            if (--slot.count == 0) slot = {};
            record_slot(ctx, inv, c.slot);
        };
    if (held.item == items::bucket && !held.empty()) {
        // The view goes as far as the block it meets.
        auto to = end(c.reach);
        if (c.hit) to = {c.hit->pos.x + c.hit->cursor_x, c.hit->pos.y + c.hit->cursor_y, c.hit->pos.z + c.hit->cursor_z};
        auto target = fluid_on(ctx, c.eye, to);
        if (!target && c.hit) target = c.hit->pos;
        if (!target || !loaded(ctx, *target) || !taken_from(ctx.block(*target))) return std::unexpected(ErrorCode::not_usable);
        return [&ctx, c, at = *target] {
            const auto filled = take_fluid(ctx, at);
            auto& inv = ctx.player(c.player);
            auto& slot = inv.slots[c.slot];
            const ItemStack full{*filled, 1, 0};
            if (!c.survival) {
                if (std::ranges::none_of(inv.slots, [&](const ItemStack& s) { return s.item == *filled && !s.empty(); }))
                    add_to_player(ctx, inv, full, c.slot);
                return;
            }
            if (slot.count == 1) {
                slot = full;
                record_slot(ctx, inv, c.slot);
                return;
            }
            --slot.count;
            record_slot(ctx, inv, c.slot);
            if (const auto rest = add_to_player(ctx, inv, full, c.slot); !rest.empty()) {
                const Vec3 at{c.eye.x, c.eye.y - 0.3, c.eye.z};
                spawn_dropped(ctx, {static_cast<std::int32_t>(std::floor(at.x)), static_cast<std::int32_t>(std::floor(at.y)),
                                    static_cast<std::int32_t>(std::floor(at.z))},
                              at, {}, rest, 40);
            }
        };
    }
    if ((held.item == items::water_bucket || held.item == items::lava_bucket) && !held.empty()) {
        if (!c.hit) return std::unexpected(ErrorCode::not_usable);
        const bool lava = held.item == items::lava_bucket;
        const auto at = c.hit->pos;
        const auto beside = at.relative(c.hit->face);
        if (!loaded(ctx, at)) return std::unexpected(ErrorCode::not_usable);
        auto target = !lava && holds_liquid(ctx.block(at)) ? at : beside;
        const auto pours = [&](BlockPos p) { return loaded(ctx, p) && pour_of(ctx.block(p), lava) != Pour::refused; };
        if (!pours(target)) {
            if (target == beside || !pours(beside)) return std::unexpected(ErrorCode::not_usable);
            target = beside;
        }
        return [&ctx, c, target, lava] {
            pour_fluid(ctx, target, lava);
            if (!c.survival) return;
            auto& inv = ctx.player(c.player);
            inv.slots[c.slot] = ItemStack{items::bucket, 1, 0};
            record_slot(ctx, inv, c.slot);
        };
    }
    if (held.item == items::map && !held.empty()) return plan_use_empty_map(ctx, c);
    return std::unexpected(ErrorCode::not_usable);
}

} // namespace circuit::gameplay::detail

namespace circuit::gameplay {
bool takes_poured_water(BlockState s) { return detail::takes_water(s); }
bool fluids_replace(BlockState s) { return detail::fluid_replaces(s); }

std::optional<std::pair<std::string_view, int>> fluid_state(BlockState s) {
    if (s.is(BlockType::water)) {
        if (s.flag()) return std::pair{std::string_view("flowing_water"), 8};
        if (s.level() == 0) return std::pair{std::string_view("water"), 8};
        return std::pair{std::string_view("flowing_water"), 8 - s.level()};
    }
    if (is_lava(s)) {
        const auto level = std::stoi(detail::property_of(s, "level"));
        if (level == 0) return std::pair{std::string_view("lava"), 8};
        return std::pair{std::string_view("flowing_lava"), level >= 8 ? 8 : 8 - level};
    }
    if (detail::holds_water_fluid(s)) return std::pair{std::string_view("water"), 8};
    return std::nullopt;
}
} // namespace circuit::gameplay
