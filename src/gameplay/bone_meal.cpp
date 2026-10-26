// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Bone meal on plants (the official 1.21.8 server; bonemeal_dispensers;
// minecraft.wiki "Bone Meal"). A plant it acts on grows, drawing from the
// level random where noted, and the bone meal is used; on anything else it is
// not:
// - wheat, carrots, potatoes and stems grow 2 + nextInt(4) stages (to their
//   last), beetroots a third of that (rounded down), torchflower and pitcher
//   crops, cocoa and sweet berry bushes one;
// - a bamboo shoot grows a stalk above; a stalk under 16 grows 1 + nextInt(2)
//   blocks from its top as in its random tick, while there is air above and
//   it has not stopped;
// - a big dripleaf (or its stem) with room above grows a block; a small
//   dripleaf becomes a big one 2 + nextInt(4) tall, as far as there is room;
// - pink petals and wildflowers get one more flower, or with four drop one;
// - cave vines without berries get them;
// - weeping and twisting vines whose head has room grow from it a head for
//   each nextDouble() in a row below 1, 0.826, 0.826^2 and so on, each one
//   older; kelp one, into water.
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

int int_of(BlockState s, std::string_view key) {
    const auto v = value_of(s, key);
    return v.empty() ? 0 : std::stoi(v);
}

BlockState with(BlockState s, std::string_view key, const std::string& value) {
    if (s.is(BlockType::wheat) && key == "age") return s.with_level(static_cast<std::uint8_t>(std::stoi(value)));
    auto props = registry::properties(s.registry_state());
    for (auto& [k, v] : props)
        if (k == key) v = value;
    return BlockState::registered(registry::state_of(*registry::block_of(s.registry_state()), props).value());
}

BlockState named(RuleContext& ctx, std::string_view name, const std::vector<registry::Property>& props = {}) {
    const auto block = registry::find_block(ctx.bundle().schema.content_release, name);
    if (props.empty()) return BlockState::registered(registry::block(*block).default_state);
    return BlockState::registered(registry::state_of(*block, props).value());
}

bool water_source(BlockState s) { return s.is(BlockType::water) && s.level() == 0 && !s.flag(); }

// Where a big dripleaf can go: air or water, or the small dripleaf it grows from.
bool dripleaf_room(RuleContext& ctx, BlockPos p) {
    if (!ctx.world().contains_y(p.y)) return false;
    const auto s = ctx.block(p);
    return s.is_air() || s.is(BlockType::water) || name_of(s) == "small_dripleaf";
}

void place_dripleaf(RuleContext& ctx, BlockPos p, std::string_view block, const std::string& facing) {
    const auto there = ctx.block(p);
    std::vector<registry::Property> props{{"facing", facing}, {"waterlogged", water_source(there) ? "true" : "false"}};
    if (block == "big_dripleaf") props.push_back({"tilt", "none"});
    set_block(ctx, p, named(ctx, block, props));
}

struct Vine {
    std::string_view head, body;
    Direction growth;
};
constexpr std::array<Vine, 3> vines{{{"weeping_vines", "weeping_vines_plant", Direction::down},
                                     {"twisting_vines", "twisting_vines_plant", Direction::up}, {"kelp", "kelp_plant", Direction::up}}};

bool vine_room(BlockState s, const Vine& v) { return v.head == "kelp" ? water_source(s) : s.is_air(); }
} // namespace

bool apply_bone_meal(RuleContext& ctx, BlockPos pos) {
    auto s = ctx.block(pos);
    if (s.is_unloaded()) return false;
    const auto n = s.is(BlockType::wheat) ? std::string_view("wheat") : name_of(s);
    // Crops and stems.
    if (n == "wheat" || n == "carrots" || n == "potatoes" || n == "melon_stem" || n == "pumpkin_stem" || n == "beetroots") {
        const int max = n == "beetroots" ? 3 : 7;
        const int age = s.is(BlockType::wheat) ? s.level() : int_of(s, "age");
        if (age >= max) return false;
        int grow = 2 + ctx.random_int(4);
        if (n == "beetroots") grow /= 3;
        set_block_quiet(ctx, pos, with(s, "age", std::to_string(std::min(age + grow, max))));
        return true;
    }
    if (n == "torchflower_crop") {
        if (int_of(s, "age") >= 1) set_block_quiet(ctx, pos, named(ctx, "torchflower"));
        else set_block_quiet(ctx, pos, with(s, "age", "1"));
        return true;
    }
    if (n == "pitcher_crop") {
        if (value_of(s, "half") == "upper") {
            pos = pos.relative(Direction::down);
            s = ctx.block(pos);
            if (name_of(s) != "pitcher_crop") return false;
        }
        const int age = int_of(s, "age");
        if (age >= 4) return false;
        const auto up = pos.relative(Direction::up);
        if (ctx.light(pos) < 8 && ctx.sky_light(pos) < 15) return false;
        const auto above = ctx.block(up);
        if (age + 1 >= 3 && !above.is_air() && name_of(above) != "pitcher_crop") return false;
        const auto next = with(s, "age", std::to_string(age + 1));
        set_block_quiet(ctx, pos, next);
        if (age + 1 >= 3) set_block(ctx, up, with(next, "half", "upper"));
        return true;
    }
    if (n == "cocoa" || n == "sweet_berry_bush") {
        const int age = int_of(s, "age");
        if (age >= (n == "cocoa" ? 2 : 3)) return false;
        set_block_quiet(ctx, pos, with(s, "age", std::to_string(age + 1)));
        return true;
    }
    // Bamboo.
    if (n == "bamboo_sapling") {
        if (!ctx.block(pos.relative(Direction::up)).is_air()) return false;
        set_block(ctx, pos.relative(Direction::up), named(ctx, "bamboo", {{"age", "0"}, {"leaves", "small"}, {"stage", "0"}}));
        return true;
    }
    if (n == "bamboo") {
        int above = 0, below = 0;
        while (above < 16 && name_of(ctx.block({pos.x, pos.y + above + 1, pos.z})) == "bamboo") ++above;
        while (below < 16 && name_of(ctx.block({pos.x, pos.y - below - 1, pos.z})) == "bamboo") ++below;
        int height = above + below + 1;
        if (height >= 16 || int_of(ctx.block({pos.x, pos.y + above, pos.z}), "stage") == 1) return false;
        const int times = 1 + ctx.random_int(2);
        for (int i = 0; i < times; ++i) {
            const BlockPos top{pos.x, pos.y + above, pos.z};
            const auto t = ctx.block(top);
            if (height >= 16 || int_of(t, "stage") == 1 || !ctx.block(top.relative(Direction::up)).is_air()) break;
            grow_bamboo_stalk(ctx, top, t, height);
            ++above;
            ++height;
        }
        return true;
    }
    // Dripleaves.
    if (n == "big_dripleaf" || n == "big_dripleaf_stem") {
        auto head = pos;
        while (name_of(ctx.block(head)) == "big_dripleaf_stem") head = head.relative(Direction::up);
        if (name_of(ctx.block(head)) != "big_dripleaf") return false;
        const auto up = head.relative(Direction::up);
        if (!dripleaf_room(ctx, up) || name_of(ctx.block(up)) == "small_dripleaf") return false;
        const auto facing = value_of(ctx.block(head), "facing");
        place_dripleaf(ctx, head, "big_dripleaf_stem", facing);
        place_dripleaf(ctx, up, "big_dripleaf", facing);
        return true;
    }
    if (n == "small_dripleaf") {
        auto base = pos;
        if (value_of(s, "half") == "upper") base = pos.relative(Direction::down);
        const auto facing = value_of(s, "facing");
        const auto upper = base.relative(Direction::up);
        // The upper half goes without any updates (its lower half stays).
        if (const auto u = ctx.block(upper); name_of(u) == "small_dripleaf") {
            const auto air = water_source(u) || value_of(u, "waterlogged") == "true" ? blocks::water_source() : blocks::air();
            ctx.write_block(upper, air);
            ctx.record(TraceKind::block_changed, upper, u.raw(), air.raw());
        }
        const int tall = 2 + ctx.random_int(4);
        int room = 0;
        while (room < tall && dripleaf_room(ctx, {base.x, base.y + room, base.z})) ++room;
        const int top = base.y + room - 1;
        for (int y = base.y; y < top; ++y) place_dripleaf(ctx, {base.x, y, base.z}, "big_dripleaf_stem", facing);
        place_dripleaf(ctx, {base.x, top, base.z}, "big_dripleaf", facing);
        return true;
    }
    // Petals.
    if (n == "pink_petals" || n == "wildflowers") {
        const int amount = int_of(s, "flower_amount");
        if (amount >= 4) {
            if (const auto item = items::by_name(std::string("minecraft:") + std::string(n))) pop_resource(ctx, pos, ItemStack{*item, 1});
        } else {
            set_block_quiet(ctx, pos, with(s, "flower_amount", std::to_string(amount + 1)));
        }
        return true;
    }
    // Vines.
    if (n == "cave_vines" || n == "cave_vines_plant") {
        if (value_of(s, "berries") == "true") return false;
        set_block_quiet(ctx, pos, with(s, "berries", "true"));
        return true;
    }
    for (const auto& v : vines) {
        if (n != v.head && n != v.body) continue;
        auto head = pos;
        while (name_of(ctx.block(head)) == v.body) head = head.relative(v.growth);
        const auto h = ctx.block(head);
        if (name_of(h) != v.head) return false;
        auto next = head.relative(v.growth);
        if (!ctx.world().contains_y(next.y) || !vine_room(ctx.block(next), v)) return false;
        int count = 1;
        if (v.head != "kelp") {
            count = 0;
            for (double chance = 1.0; ctx.random_double() < chance; chance *= 0.826) ++count;
        }
        int age = std::min(int_of(h, "age") + 1, 25);
        for (int i = 0; i < count && ctx.world().contains_y(next.y) && vine_room(ctx.block(next), v); ++i) {
            set_block(ctx, next, with(h, "age", std::to_string(age)));
            next = next.relative(v.growth);
            age = std::min(age + 1, 25);
        }
        return true;
    }
    return false;
}

} // namespace circuit::gameplay::detail
