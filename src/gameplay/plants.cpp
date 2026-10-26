// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/sky.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Plants growing in their random ticks (the official 1.21.8 server;
// plants_growth; minecraft.wiki "Sugar Cane", "Cactus", "Nether Wart",
// "Sweet Berry Bush", "Cocoa Beans", "Kelp", "Cave Vines").
namespace circuit::gameplay::detail {
namespace {
std::string_view name_of(BlockState s) {
    const auto state = physics::registry_state_of(s);
    const auto block = state ? registry::block_of(*state) : std::nullopt;
    return block ? registry::game_name(*block) : std::string_view{};
}

int property(BlockState s, std::string_view key) {
    const auto state = physics::registry_state_of(s);
    if (!state) return 0;
    for (const auto& [k, v] : registry::properties(*state))
        if (k == key) return v == "true" ? 1 : v == "false" ? 0 : std::stoi(v);
    return 0;
}

// The state with one property changed, the others kept.
BlockState with(BlockState s, std::string_view key, const std::string& value) {
    auto props = registry::properties(s.registry_state());
    for (auto& [k, v] : props)
        if (k == key) v = value;
    return BlockState::registered(registry::state_of(*registry::block_of(s.registry_state()), props).value());
}

BlockState named(RuleContext& ctx, std::string_view name) {
    const auto block = registry::find_block(ctx.bundle().schema.content_release, name);
    return BlockState::registered(registry::block(*block).default_state);
}

int height_below(RuleContext& ctx, BlockPos pos, std::string_view name) {
    int h = 1;
    while (name_of(ctx.block({pos.x, pos.y - h, pos.z})) == name) ++h;
    return h;
}

struct Growing {
    std::string_view head;
    Direction growth;
    double chance;
};
constexpr std::array<Growing, 4> growing{{{"kelp", Direction::up, 0.14}, {"weeping_vines", Direction::down, 0.1},
                                          {"twisting_vines", Direction::up, 0.1}, {"cave_vines", Direction::down, 0.1}}};

BlockState bamboo(RuleContext& ctx, int age, std::string_view leaves, int stage) {
    const auto block = registry::find_block(ctx.bundle().schema.content_release, "bamboo");
    const std::vector<registry::Property> props{{"age", std::to_string(age)}, {"leaves", std::string(leaves)}, {"stage", std::to_string(stage)}};
    return BlockState::registered(registry::state_of(*block, props).value());
}

std::string leaves_of(BlockState s) {
    for (const auto& [k, v] : registry::properties(s.registry_state()))
        if (k == "leaves") return v;
    return "none";
}

// A stalk grows a block above it, `height` tall itself (bamboo_growth): the new
// block has leaves, large when the stalk's below has them too, which then
// leaves that one small and the one under it bare; it is thick when the stalk
// is or two of its blocks are below; it stops growing when the stalk is 11 or
// more tall and nextFloat() < 0.25, or 15 tall.
} // namespace

void grow_bamboo_stalk(RuleContext& ctx, BlockPos pos, BlockState s, int height) {
    const BlockPos down{pos.x, pos.y - 1, pos.z}, down2{pos.x, pos.y - 2, pos.z};
    const auto b1 = ctx.block(down), b2 = ctx.block(down2);
    std::string_view leaves = "small";
    if (name_of(b1) == "bamboo" && leaves_of(b1) != "none") {
        leaves = "large";
        if (name_of(b2) == "bamboo") {
            set_block(ctx, down, with(b1, "leaves", "small"));
            set_block(ctx, down2, with(b2, "leaves", "none"));
        }
    }
    const int thick = property(s, "age") == 1 || name_of(b2) == "bamboo" ? 1 : 0;
    const int stage = (height >= 11 && ctx.random_float() < 0.25F) || height == 15 ? 1 : 0;
    set_block(ctx, pos.relative(Direction::up), bamboo(ctx, thick, leaves, stage));
}

bool is_bamboo(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    const auto n = name_of(s);
    return n == "bamboo" || n == "bamboo_sapling";
}

// A shoot turns into a stalk when bamboo grows above it; a stalk thickens
// when the one above it is thicker (without neighbor updates).
bool bamboo_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d) {
    if (d != Direction::up) return false;
    const auto above = ctx.block(pos.relative(d));
    if (name_of(above) != "bamboo") return false;
    if (name_of(s) == "bamboo_sapling") {
        set_block_quiet(ctx, pos, bamboo(ctx, 0, "none", 0));
        return true;
    }
    if (property(above, "age") <= property(s, "age")) return false;
    set_block_quiet(ctx, pos, with(s, "age", "1"));
    return true;
}

namespace {
void bamboo_random_tick(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto up = pos.relative(Direction::up);
    if (name_of(s) == "bamboo_sapling") {
        if (ctx.random_int(3) == 0 && ctx.block(up).is_air() && ctx.light(up) >= 9) set_block(ctx, up, bamboo(ctx, 0, "small", 0));
        return;
    }
    if (property(s, "stage") != 0) return;
    if (ctx.random_int(3) != 0 || !ctx.block(up).is_air() || ctx.light(up) < 9) return;
    int below = 0;
    while (below < 16 && name_of(ctx.block({pos.x, pos.y - below - 1, pos.z})) == "bamboo") ++below;
    if (below + 1 < 16) grow_bamboo_stalk(ctx, pos, s, below + 1);
}
} // namespace

// Eyeblossoms (drip_eggs_blossoms; minecraft.wiki "Eyeblossom"): one open by
// day (the sky darkened less than 4) or closed by night turns, in the
// overworld, and gives every eyeblossom of its old state within 3, 2, 3 a
// block tick nextInt between 5 d and 10 d (d its distance) later, cells in
// x, then y, then z order; the tick turns them as the random tick does. A
// potted one turns alone.
bool turn_eyeblossom(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (ctx.dimension() != contracts::DimensionId::overworld) return false;
    const auto n = name_of(s);
    const bool open = n == "open_eyeblossom" || n == "potted_open_eyeblossom";
    const bool day = ctx.sky().darken < 4;
    if (day != open) return false;
    const bool potted = n.starts_with("potted_");
    set_block(ctx, pos, named(ctx, std::string(potted ? "potted_" : "") + (open ? "closed_eyeblossom" : "open_eyeblossom")));
    // The particle trail of the turn draws four doubles (not modeled otherwise).
    for (int i = 0; i < 4; ++i) (void)ctx.random_double();
    if (potted) return true;
    for (int dz = -3; dz <= 3; ++dz)
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -3; dx <= 3; ++dx) {
                const BlockPos q{pos.x + dx, pos.y + dy, pos.z + dz};
                if (!ctx.world().contains_y(q.y) || ctx.block(q) != s) continue;
                const double d = std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz));
                const int least = static_cast<int>(d * 5.0), most = static_cast<int>(d * 10.0);
                const auto delay = static_cast<std::uint32_t>(least + ctx.random_int(most - least + 1));
                schedule(ctx, TickList::block, q, BlockType::other, delay, 0);
            }
    return true;
}

// Turtle eggs (minecraft.wiki "Turtle Egg"): unless the time of day is
// between 0.65 and 0.69, an egg cracks only on nextInt(500) == 0; on sand it
// then draws its sound's pitch and cracks a step, or at its third hatches
// (the turtles are not modeled: the eggs go).
void turtle_egg_random_tick(RuleContext& ctx, BlockPos pos, BlockState s) {
    const float t = time_of_day(ctx.sky().day_time);
    const bool now = (t < 0.69 && t > 0.65) || ctx.random_int(500) == 0;
    if (!now) return;
    const auto below = name_of(ctx.block(pos.relative(Direction::down)));
    if (below != "sand" && below != "red_sand" && below != "suspicious_sand") return;
    // The sound of the crack or the hatching draws its pitch.
    (void)ctx.random_float();
    const int hatch = property(s, "hatch");
    if (hatch < 2) set_block_quiet(ctx, pos, with(s, "hatch", std::to_string(hatch + 1)));
    else set_block(ctx, pos, blocks::air());
}

bool is_growing_plant(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    const auto n = name_of(s);
    static constexpr std::array<std::string_view, 13> plants{"sugar_cane", "cactus", "nether_wart", "sweet_berry_bush", "cocoa", "kelp",
                                                            "weeping_vines", "twisting_vines", "cave_vines", "brown_mushroom", "red_mushroom",
                                                            "bamboo", "bamboo_sapling"};
    if (n == "open_eyeblossom" || n == "closed_eyeblossom" || n == "potted_open_eyeblossom" || n == "potted_closed_eyeblossom"
        || n == "turtle_egg")
        return true;
    return std::ranges::find(plants, n) != plants.end();
}

void plant_random_tick(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto n = name_of(s);
    const auto up = pos.relative(Direction::up);
    if (n == "bamboo" || n == "bamboo_sapling") {
        bamboo_random_tick(ctx, pos, s);
        return;
    }
    if (n == "turtle_egg") {
        turtle_egg_random_tick(ctx, pos, s);
        return;
    }
    if (n.ends_with("eyeblossom")) {
        (void)turn_eyeblossom(ctx, pos, s);
        return;
    }
    if (n == "sugar_cane") {
        if (!ctx.block(up).is_air() || height_below(ctx, pos, n) >= 3) return;
        const int age = property(s, "age");
        if (age == 15) {
            set_block(ctx, up, named(ctx, n));
            set_block_quiet(ctx, pos, with(s, "age", "0"));
        } else {
            set_block_quiet(ctx, pos, with(s, "age", std::to_string(age + 1)));
        }
        return;
    }
    if (n == "cactus") {
        if (!ctx.block(up).is_air()) return;
        const int height = height_below(ctx, pos, n);
        const int age = property(s, "age");
        if (age == 8 && registry::find_block(ctx.bundle().schema.content_release, "cactus_flower")) {
            const bool clear = std::ranges::all_of(std::array{Direction::north, Direction::south, Direction::west, Direction::east},
                                                   [&](Direction d) { return ctx.block(up.relative(d)).is_air(); });
            if (clear && ctx.random_double() <= (height >= 3 ? 0.25 : 0.1)) set_block(ctx, up, named(ctx, "cactus_flower"));
        }
        if (age < 15) {
            set_block_quiet(ctx, pos, with(s, "age", std::to_string(age + 1)));
        } else if (height < 3) {
            set_block(ctx, up, named(ctx, n));
            set_block_quiet(ctx, pos, with(s, "age", "0"));
        }
        return;
    }
    if (n == "nether_wart") {
        const int age = property(s, "age");
        if (age < 3 && ctx.random_int(10) == 0) set_block_quiet(ctx, pos, with(s, "age", std::to_string(age + 1)));
        return;
    }
    if (n == "sweet_berry_bush") {
        const int age = property(s, "age");
        if (age < 3 && ctx.random_int(5) == 0 && ctx.light(up) >= 9) set_block_quiet(ctx, pos, with(s, "age", std::to_string(age + 1)));
        return;
    }
    if (n == "cocoa") {
        const int age = property(s, "age");
        if (ctx.random_int(5) == 0 && age < 2) set_block_quiet(ctx, pos, with(s, "age", std::to_string(age + 1)));
        return;
    }
    if (n == "brown_mushroom" || n == "red_mushroom") {
        if (ctx.random_int(25) != 0) return;
        int near = 0;
        for (int dx = -4; dx <= 4; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -4; dz <= 4; ++dz)
                    if (const BlockPos q{pos.x + dx, pos.y + dy, pos.z + dz}; ctx.world().contains_y(q.y) && name_of(ctx.block(q)) == n) ++near;
        if (near >= 5) return;
        const auto step = [&](BlockPos from) {
            const int dx = ctx.random_int(3) - 1;
            const int dy = ctx.random_int(2) - ctx.random_int(2);
            const int dz = ctx.random_int(3) - 1;
            return BlockPos{from.x + dx, from.y + dy, from.z + dz};
        };
        const auto fits = [&](BlockPos q) { return ctx.world().contains_y(q.y) && ctx.block(q).is_air() && supported_at(ctx, q, s); };
        auto from = pos;
        auto to = step(from);
        for (int i = 0; i < 4; ++i) {
            if (fits(to)) from = to;
            to = step(from);
        }
        if (fits(to)) set_block_quiet(ctx, to, s);
        return;
    }
    const auto g = std::ranges::find(growing, n, &Growing::head);
    if (g == growing.end()) return;
    const int age = property(s, "age");
    if (age >= 25 || ctx.random_double() >= g->chance) return;
    const auto to = pos.relative(g->growth);
    const auto there = ctx.block(to);
    const bool room = n == "kelp" ? there.is(BlockType::water) && there.level() == 0 && !there.flag() : there.is_air();
    if (!room) return;
    auto next = with(s, "age", std::to_string(std::min(age + 1, 25)));
    if (n == "cave_vines") next = with(next, "berries", ctx.random_float() < 0.11F ? "true" : "false");
    set_block(ctx, to, next);
}

} // namespace circuit::gameplay::detail
