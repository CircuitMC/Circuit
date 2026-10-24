// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <string>

namespace circuit::gameplay::detail {
namespace {
// Fluids as the target version describes them in amounts: a source or falling
// fluid has 8, flowing fluid at level l has 8 - l. Water loses 1 a block,
// looks 4 blocks ahead for a drop and moves every 5 ticks (water_channel,
// water_drop, water_washes, water_torch_button); lava in the overworld loses
// 2, looks 2 blocks ahead and moves every 30 (lava_flow). Each forms sources
// between two of its sources when its game rule says so (fluid_sources).
struct Fluid {
    bool lava;
    int drop;
    int slope_find_distance;
    std::uint32_t delay;
    std::string_view sources_rule;
};
constexpr Fluid water{false, 1, 4, 5, "waterSourceConversion"};
constexpr Fluid lava{true, 2, 2, 30, "lavaSourceConversion"};
constexpr std::array<Direction, 4> plane{Direction::north, Direction::east, Direction::south, Direction::west};

// Lava is a registry block: its states by level (0 source, 1-7 flowing, 8-15
// falling), and the blocks it hardens into.
struct LavaStates {
    std::uint32_t block{};
    std::array<std::uint32_t, 16> by_level{};
    BlockState cobblestone, basalt;
    std::uint32_t soul_soil{}, blue_ice{};
};
const LavaStates& lava_states() {
    static const LavaStates out = [] {
        LavaStates t;
        const auto release = registry::releases().front();
        const auto find = [&](std::string_view name) { return *registry::find_block(release, name); };
        t.block = find("lava");
        for (std::uint32_t l = 0; l < 16; ++l)
            t.by_level[l] = *registry::state_of(t.block, std::array{registry::Property{"level", std::to_string(l)}});
        t.cobblestone = BlockState::registered(registry::block(find("cobblestone")).default_state);
        t.basalt = BlockState::registered(*registry::state_of(find("basalt"), std::array{registry::Property{"axis", "y"}}));
        t.soul_soil = find("soul_soil");
        t.blue_ice = find("blue_ice");
        return t;
    }();
    return out;
}
bool is_block(BlockState s, std::uint32_t block) {
    return s.is(BlockType::other) && registry::block_of(s.registry_state()) == block;
}
// A lava state's level, or -1 for any other state.
int lava_level(BlockState s) {
    if (!is_block(s, lava_states().block)) return -1;
    const auto& by_level = lava_states().by_level;
    return static_cast<int>(std::ranges::find(by_level, s.registry_state()) - by_level.begin());
}
BlockState lava_state(int level) { return BlockState::registered(lava_states().by_level[static_cast<std::size_t>(level)]); }

bool is_of(const Fluid& f, BlockState s) { return f.lava ? lava_level(s) >= 0 : s.is(BlockType::water); }
bool is_source(const Fluid& f, BlockState s) { return f.lava ? lava_level(s) == 0 : s.is(BlockType::water) && s.level() == 0 && !s.flag(); }
bool is_falling(const Fluid& f, BlockState s) { return f.lava ? lava_level(s) >= 8 : s.is(BlockType::water) && s.flag(); }
int amount(const Fluid& f, BlockState s) {
    if (!is_of(f, s)) return 0;
    const int level = f.lava ? lava_level(s) : (s.flag() ? 8 : s.level());
    return level == 0 || level >= 8 ? 8 : 8 - level; // sources and falling fluid: 8
}
BlockState source(const Fluid& f) { return f.lava ? lava_state(0) : blocks::water_source(); }
BlockState falling(const Fluid& f) { return f.lava ? lava_state(8) : blocks::water_falling(); }
BlockState flowing(const Fluid& f, int k) {
    return f.lava ? lava_state(8 - k) : blocks::water_flowing(static_cast<std::uint8_t>(8 - k));
}

// Registry blocks fluids flow into, washing them away, as both official
// servers do for every state (tests/fixtures/collision/flows-*.txt): those
// that do not block motion (solid ones but cobwebs and bamboo saplings) and
// hold no water (a waterlogged property, kelp and seagrass), but portals and
// structure voids.
bool washed(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    static const std::vector<bool> table = [] {
        std::vector<bool> out(registry::state_count() + 1);
        for (std::uint32_t b = 0; b < registry::block_count(); ++b) {
            const auto info = registry::block(b);
            const auto name = info.name;
            if (name == "nether_portal" || name == "end_portal" || name == "end_gateway" || name == "structure_void" || name == "kelp"
                || name == "kelp_plant" || name == "seagrass" || name == "tall_seagrass" || name == "lava" || name == "water")
                continue;
            for (std::uint32_t state = info.first_state; state < info.first_state + info.state_count; ++state) {
                if (registry::solid(state) && name != "cobweb" && name != "bamboo_sapling") continue;
                const auto props = registry::properties(state);
                out[state] = std::ranges::none_of(props, [](const auto& p) { return p.first == "waterlogged"; });
            }
        }
        return out;
    }();
    const auto state = s.registry_state();
    return state < table.size() && table[state];
}

// The fluid can stand in the block: air, the fluid itself and the blocks it
// washes away (lava destroys them without drops: lava_water).
bool holds(const Fluid& f, BlockState s) { return s.is_air() || is_of(f, s) || washed_away(s.type()) || washed(s); }
// The fluid may flow into the block: it holds the fluid and is no source of
// it. Lava also flows down into water, which becomes stone.
bool passable(const Fluid& f, BlockState s, bool down = false) {
    if (f.lava && down && s.is(BlockType::water)) return true;
    return holds(f, s) && !is_source(f, s);
}
// Only cells without the fluid take new flowing fluid from a side; flowing
// fluid changes through its own ticks.
bool replaceable_by_flow(const Fluid& f, BlockState s) { return !is_of(f, s); }

// Fluid a cell would hold from its neighbors: a source between two sources on
// solid ground or above a source (where the game rule allows it), falling
// fluid below the fluid, else its strongest neighbor's less the drop.
BlockState new_liquid(RuleContext& ctx, const Fluid& f, BlockPos pos) {
    int best = 0, sources = 0;
    for (const auto d : plane) {
        const auto n = ctx.block(pos.relative(d));
        if (!is_of(f, n)) continue;
        if (is_source(f, n)) ++sources;
        best = std::max(best, amount(f, n));
    }
    // Versions without the game rule: water forms sources, lava none.
    const bool forms = ctx.bundle().schema.game_rule(f.sources_rule) ? ctx.game_rule(f.sources_rule) != 0 : !f.lava;
    if (sources >= 2 && forms) {
        const auto below = ctx.block(pos.relative(Direction::down));
        if (is_solid(below) || is_source(f, below)) return source(f);
    }
    if (is_of(f, ctx.block(pos.relative(Direction::up)))) return falling(f);
    const int k = best - f.drop;
    if (k <= 0) return blocks::air();
    return flowing(f, k);
}

// The fluid could flow down from `pos`.
bool is_hole(RuleContext& ctx, const Fluid& f, BlockPos pos) {
    const auto below = ctx.block(pos.relative(Direction::down));
    return holds(f, below) || (f.lava && below.is(BlockType::water));
}

// Steps from `pos` to the nearest hole within reach, not going back the way it came.
int slope_distance(RuleContext& ctx, const Fluid& f, BlockPos pos, int depth, Direction from) {
    int best = 1000;
    for (const auto d : plane) {
        if (d == from) continue;
        const auto next = pos.relative(d);
        if (!passable(f, ctx.block(next))) continue;
        if (is_hole(ctx, f, next)) return depth;
        if (depth >= f.slope_find_distance) continue;
        best = std::min(best, slope_distance(ctx, f, next, depth + 1, contracts::opposite(d)));
    }
    return best;
}

void spread_to(RuleContext& ctx, const Fluid& f, BlockPos pos, BlockState fluid) {
    const auto there = ctx.block(pos);
    // Lava flowing down into water makes stone (lava_water).
    if (f.lava && there.is(BlockType::water)) {
        set_block(ctx, pos, blocks::stone());
        return;
    }
    if (!f.lava && !there.is_air() && !is_of(f, there)) drop_items(ctx, pos, there);
    set_block(ctx, pos, fluid);
}

// Sides the fluid spreads to: toward the nearest holes if any is in reach,
// else all. Spreads in the order north, south, west, east.
void spread_to_sides(RuleContext& ctx, const Fluid& f, BlockPos pos, BlockState s) {
    if (!is_falling(f, s) && amount(f, s) - f.drop <= 0) return;
    int best = 1000;
    std::map<Direction, BlockState> out; // Direction order: north, south, west, east
    for (const auto d : plane) {
        const auto p = pos.relative(d);
        const auto there = ctx.block(p);
        if (!passable(f, there)) continue;
        const auto fresh = new_liquid(ctx, f, p);
        const int j = is_hole(ctx, f, p) ? 0 : slope_distance(ctx, f, p, 1, contracts::opposite(d));
        if (j < best) out.clear();
        if (j <= best) {
            if (replaceable_by_flow(f, there)) out[d] = fresh;
            best = j;
        }
    }
    for (const auto& [d, fluid] : out) spread_to(ctx, f, pos.relative(d), fluid);
}

void spread(RuleContext& ctx, const Fluid& f, BlockPos pos, BlockState s) {
    const auto below = pos.relative(Direction::down);
    const auto under = ctx.block(below);
    if (passable(f, under, true) && replaceable_by_flow(f, under)) {
        spread_to(ctx, f, below, new_liquid(ctx, f, below));
        int sources = 0;
        for (const auto d : plane) sources += is_source(f, ctx.block(pos.relative(d)));
        if (sources >= 3) spread_to_sides(ctx, f, pos, s);
        return;
    }
    if (is_source(f, s) || !is_hole(ctx, f, pos)) spread_to_sides(ctx, f, pos, s);
}

void tick_fluid(RuleContext& ctx, const Fluid& f, BlockPos pos, BlockState s) {
    auto current = s;
    if (!is_source(f, s)) {
        const auto next = new_liquid(ctx, f, pos);
        if (next.is_air()) {
            set_block(ctx, pos, next);
            return;
        }
        if (next != s) {
            set_block(ctx, pos, next); // schedules the next fluid tick (on_placed)
            current = next;
        }
    }
    spread(ctx, f, pos, current);
}
} // namespace

bool washed_away(BlockType t) noexcept {
    return t == BlockType::redstone_wire || t == BlockType::lever || t == BlockType::repeater || t == BlockType::comparator
        || t == BlockType::wheat || t == BlockType::redstone_torch || t == BlockType::redstone_wall_torch || t == BlockType::button;
}

bool holds_water(BlockState s) { return holds(water, s); }

void tick_water(RuleContext& ctx, BlockPos pos, BlockState s) { tick_fluid(ctx, water, pos, s); }
void tick_lava(RuleContext& ctx, BlockPos pos, BlockState s) { tick_fluid(ctx, lava, pos, s); }

// Lava touching water above or beside it hardens, a source into obsidian and
// flowing lava into cobblestone; lava on soul soil beside blue ice becomes
// basalt. Else it moves on its next fluid tick (lava_water).
void lava_changed(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto& t = lava_states();
    const bool on_soul_soil = is_block(ctx.block(pos.relative(Direction::down)), t.soul_soil);
    for (const auto d : {Direction::up, Direction::north, Direction::south, Direction::west, Direction::east}) {
        const auto n = ctx.block(pos.relative(d));
        if (holds_water_fluid(n)) {
            set_block(ctx, pos, is_source(lava, s) ? blocks::obsidian() : t.cobblestone);
            return;
        }
        if (on_soul_soil && is_block(n, t.blue_ice)) {
            set_block(ctx, pos, t.basalt);
            return;
        }
    }
    schedule(ctx, TickList::fluid, pos, BlockType::other, lava.delay, 0);
}

void wheat_random_tick(RuleContext& ctx, BlockPos pos, BlockState s, Rng& stream) {
    // Crops grow only with light 9 or more at their own cell (minecraft.wiki
    // "Wheat Crops"), checked before the growth draw.
    if (s.level() >= 7 || ctx.light(pos) < 9) return;
    if (stream.bounded(3) == 0) set_block(ctx, pos, s.with_level(static_cast<std::uint8_t>(s.level() + 1)));
}

} // namespace circuit::gameplay::detail

namespace circuit::gameplay {
bool fluid_flows_into(BlockState s, bool lava) { return detail::holds(lava ? detail::lava : detail::water, s) && !detail::is_of(lava ? detail::lava : detail::water, s); }
bool is_lava(BlockState s) { return detail::lava_level(s) >= 0; }
bool is_lava_source(BlockState s) { return detail::lava_level(s) == 0; }
BlockState lava_source() { return detail::lava_state(0); }
} // namespace circuit::gameplay
