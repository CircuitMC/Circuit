// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <array>
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
} // namespace

bool is_growing_plant(BlockState s) {
    if (!s.is(BlockType::other)) return false;
    const auto n = name_of(s);
    static constexpr std::array<std::string_view, 11> plants{"sugar_cane", "cactus", "nether_wart", "sweet_berry_bush", "cocoa", "kelp",
                                                            "weeping_vines", "twisting_vines", "cave_vines", "brown_mushroom", "red_mushroom"};
    return std::ranges::find(plants, n) != plants.end();
}

void plant_random_tick(RuleContext& ctx, BlockPos pos, BlockState s) {
    const auto n = name_of(s);
    const auto up = pos.relative(Direction::up);
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
