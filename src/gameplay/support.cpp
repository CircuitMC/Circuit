// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <array>
#include <string>
#include <string_view>
#include <vector>

// Blocks that need support, as both official servers keep them (reports of
// what every state supports and of every state's survival,
// tests/fixtures/collision/support-*.txt and survival-*.txt; minecraft.wiki
// "Torch", "Lantern", "Ladder", "Carpet", "Flower" and "Sapling"):
// - torches and soul torches stand on a top whose middle bears them, wall
//   torches hang on the whole face behind them;
// - lanterns stand as torches do, or hang under a bottom whose middle holds
//   them;
// - ladders hang on the whole face behind them;
// - carpets lie on anything but air;
// - signs and banners stand on a solid block or hang on one behind them
//   (registry::solid), and break when it goes; a bed's halves need each
//   other, and only its head drops the bed;
// - flowers and saplings grow on soil (registry::soil); the lower half of a
//   tall flower (sunflower, lilac, rose bush, peony) too, and goes when the
//   block above is not its upper half, which stands on it.
// A shape update that finds one unsupported (torches, lanterns and ladders:
// from their support's side) breaks it with its drops, and a
// piston that sets one down where it cannot stay breaks it at once. Without
// data packs they drop themselves (their loot tables do the same).
namespace circuit::gameplay::detail {
namespace {
enum class Need : std::uint8_t { none, standing, wall, lantern, ladder, carpet, plant, tall_plant, standing_sign, wall_sign, bed };

const std::vector<Need>& needs() {
    static const std::vector<Need> out = [] {
        constexpr std::array<std::string_view, 23> plants{
            "dandelion", "poppy", "blue_orchid", "allium", "azure_bluet", "red_tulip", "orange_tulip", "white_tulip",
            "pink_tulip", "oxeye_daisy", "cornflower", "lily_of_the_valley", "torchflower", "closed_eyeblossom", "open_eyeblossom",
            "oak_sapling", "spruce_sapling", "birch_sapling", "jungle_sapling", "acacia_sapling", "dark_oak_sapling",
            "cherry_sapling", "pale_oak_sapling"};
        std::vector<Need> v(registry::block_count(), Need::none);
        for (std::uint32_t b = 0; b < v.size(); ++b) {
            const auto name = registry::game_name(b);
            if (name == "torch" || name == "soul_torch") v[b] = Need::standing;
            else if (name == "wall_torch" || name == "soul_wall_torch") v[b] = Need::wall;
            else if (name == "lantern" || name == "soul_lantern") v[b] = Need::lantern;
            else if (name == "ladder") v[b] = Need::ladder;
            else if (name == "moss_carpet" || (name.ends_with("_carpet") && name != "pale_moss_carpet")) v[b] = Need::carpet;
            else if (std::ranges::find(plants, name) != plants.end()) v[b] = Need::plant;
            else if (name == "sunflower" || name == "lilac" || name == "rose_bush" || name == "peony") v[b] = Need::tall_plant;
            else if ((name.ends_with("_wall_sign") && !name.ends_with("_hanging_sign")) || name.ends_with("_wall_banner")) v[b] = Need::wall_sign;
            else if ((name.ends_with("_sign") && !name.ends_with("_hanging_sign")) || name.ends_with("_banner")) v[b] = Need::standing_sign;
            else if (name.ends_with("_bed")) v[b] = Need::bed;
        }
        return v;
    }();
    return out;
}

Need need_of(BlockState s) {
    if (!s.is(BlockType::other)) return Need::none;
    const auto block = registry::block_of(s.registry_state());
    return block ? needs()[*block] : Need::none;
}

std::string value_of(BlockState s, std::string_view key) {
    for (const auto& [k, v] : registry::properties(s.registry_state()))
        if (k == key) return v;
    return {};
}

Direction facing_of(BlockState s) {
    const auto f = value_of(s, "facing");
    return f == "north" ? Direction::north : f == "south" ? Direction::south : f == "west" ? Direction::west : Direction::east;
}

// The other half of a tall flower: the same block's other half beside it.
bool other_half_of(BlockState s, BlockState other, bool upper) {
    return other.is(BlockType::other) && registry::block_of(other.registry_state()) == registry::block_of(s.registry_state())
        && (value_of(other, "half") == "upper") == upper;
}

bool solid_block(BlockState s) {
    const auto state = physics::registry_state_of(s);
    return state && registry::solid(*state);
}

// Toward a bed's other half: its head is ahead of its foot.
Direction toward_other_half(BlockState bed) {
    return value_of(bed, "part") == "foot" ? facing_of(bed) : contracts::opposite(facing_of(bed));
}

bool middle(BlockState s, Direction face, std::string_view release) {
    const auto state = physics::registry_state_of(s);
    return state && registry::supports_middle(*state, face, release);
}
bool sturdy(BlockState s, Direction face, std::string_view release) {
    const auto state = physics::registry_state_of(s);
    return state && registry::sturdy_face(*state, face, release);
}
} // namespace

bool needs_support(BlockState s) { return need_of(s) != Need::none; }

bool supported(BlockState s, const std::function<BlockState(Direction)>& around, std::string_view release) {
    switch (need_of(s)) {
    case Need::standing: return middle(around(Direction::down), Direction::up, release);
    case Need::lantern:
        return value_of(s, "hanging") == "true" ? middle(around(Direction::up), Direction::down, release)
                                                : middle(around(Direction::down), Direction::up, release);
    case Need::wall: case Need::ladder: {
        // A wall torch or ladder facing north hangs on the north face of the
        // block south of it.
        const auto facing = facing_of(s);
        return sturdy(around(contracts::opposite(facing)), facing, release);
    }
    case Need::carpet: return !around(Direction::down).is_air();
    case Need::plant: {
        const auto below = physics::registry_state_of(around(Direction::down));
        return below && registry::soil(*below);
    }
    // Signs and banners stand on a solid block or hang on one behind them.
    case Need::standing_sign: return solid_block(around(Direction::down));
    case Need::wall_sign: return solid_block(around(contracts::opposite(facing_of(s))));
    // A bed half needs its other half.
    case Need::bed: {
        const auto other = around(toward_other_half(s));
        return other.is(BlockType::other) && registry::block_of(other.registry_state()) == registry::block_of(s.registry_state())
            && value_of(other, "part") != value_of(s, "part");
    }
    // A tall flower's lower half grows on soil, its upper half on its lower half.
    case Need::tall_plant: {
        if (value_of(s, "half") == "upper") return other_half_of(s, around(Direction::down), false);
        const auto below = physics::registry_state_of(around(Direction::down));
        return below && registry::soil(*below);
    }
    default: return true;
    }
}

bool supported_at(RuleContext& ctx, BlockPos pos, BlockState s) {
    return supported(s, [&](Direction d) { return ctx.block(pos.relative(d)); }, ctx.bundle().schema.content_release);
}

void support_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d) {
    // Torches, lanterns and ladders look again only when their support's side
    // changes, carpets, flowers and saplings on any update.
    std::optional<Direction> side;
    switch (need_of(s)) {
    case Need::standing: side = Direction::down; break;
    case Need::lantern: side = value_of(s, "hanging") == "true" ? Direction::up : Direction::down; break;
    case Need::wall: case Need::ladder: case Need::wall_sign: side = contracts::opposite(facing_of(s)); break;
    case Need::standing_sign: side = Direction::down; break;
    case Need::bed: side = toward_other_half(s); break;
    default: break;
    }
    if (side && *side != d) return;
    // A tall flower's lower half also goes when the block above is not its upper half.
    const bool lost_top = need_of(s) == Need::tall_plant && d == Direction::up && value_of(s, "half") == "lower"
                       && !other_half_of(s, ctx.block(pos.relative(Direction::up)), true);
    if (lost_top || !supported_at(ctx, pos, s)) destroy_block(ctx, pos);
}

std::optional<ItemStack> supported_drop(BlockState s) {
    if (need_of(s) == Need::none) return std::nullopt;
    // A tall flower drops from its lower half, a bed from its head.
    if (need_of(s) == Need::tall_plant && value_of(s, "half") == "upper") return std::nullopt;
    if (need_of(s) == Need::bed && value_of(s, "part") == "foot") return std::nullopt;
    // A wall torch, sign or banner drops the item of the standing one.
    auto name = std::string("minecraft:") + std::string(registry::game_name(*registry::block_of(s.registry_state())));
    if (const auto wall = name.find("wall_"); wall != std::string::npos) name.erase(wall, 5);
    const auto item = items::by_name(name);
    return item ? std::optional(ItemStack{*item, 1}) : std::nullopt;
}

} // namespace circuit::gameplay::detail

namespace circuit::gameplay {
bool block_supported(BlockState s, const std::function<BlockState(contracts::Direction)>& around, std::string_view release) {
    return detail::supported(s, around, release);
}
} // namespace circuit::gameplay
