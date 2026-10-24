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
//   block above is not its upper half, which stands on it;
// - other plants grow on the ground of their kind (tests/fixtures/collision/
//   plants-*.txt): grasses, ferns, bushes, petals, wildflowers and sweet
//   berry bushes on soil (tall grass and large ferns as tall flowers); dead
//   bushes and dry grasses also on sand and terracotta (on 1.21.4 not on
//   farmland); fungi, roots and nether sprouts also on nylium and soul soil;
//   wither roses also on netherrack, soul sand and soul soil; azaleas and
//   mangrove propagules (not hanging ones) also on clay; crops on farmland;
//   nether wart on soul sand; bamboo on soil but farmland, on sand, gravel and
//   bamboo.
// A shape update that finds one unsupported (torches, lanterns and ladders:
// from their support's side) breaks it with its drops, and a
// piston that sets one down where it cannot stay breaks it at once. Without
// data packs they drop themselves (their loot tables do the same), young
// crops and bamboo shoots what their loot tables give, and plants whose loot
// is random (grasses, ferns, dead bushes, stems) nothing.
namespace circuit::gameplay::detail {
namespace {
enum class Need : std::uint8_t {
    none, standing, wall, lantern, ladder, carpet, plant, tall_plant, standing_sign, wall_sign, bed, cane, cactus, lily_pad, sea_pickle
};
// The ground a plant grows on: one bit each in a block's grounds.
enum class Ground : std::uint8_t { soil, dry, nether, wither, clay, farmland, soul_sand, bamboo, cane, cactus };
constexpr std::uint16_t bit(Ground g) { return static_cast<std::uint16_t>(1u << static_cast<unsigned>(g)); }

struct Kind {
    Need need = Need::none;
    Ground ground = Ground::soil;
};

const std::vector<Kind>& kinds() {
    static const std::vector<Kind> out = [] {
        constexpr std::array<std::string_view, 30> soil_plants{
            "dandelion", "poppy", "blue_orchid", "allium", "azure_bluet", "red_tulip", "orange_tulip", "white_tulip",
            "pink_tulip", "oxeye_daisy", "cornflower", "lily_of_the_valley", "torchflower", "closed_eyeblossom", "open_eyeblossom",
            "oak_sapling", "spruce_sapling", "birch_sapling", "jungle_sapling", "acacia_sapling", "dark_oak_sapling",
            "cherry_sapling", "pale_oak_sapling", "fern", "short_grass", "bush", "firefly_bush", "pink_petals", "wildflowers",
            "sweet_berry_bush"};
        constexpr std::array<std::pair<std::string_view, Ground>, 19> other_plants{{
            {"dead_bush", Ground::dry}, {"short_dry_grass", Ground::dry}, {"tall_dry_grass", Ground::dry},
            {"crimson_fungus", Ground::nether}, {"warped_fungus", Ground::nether}, {"crimson_roots", Ground::nether},
            {"warped_roots", Ground::nether}, {"nether_sprouts", Ground::nether}, {"wither_rose", Ground::wither},
            {"azalea", Ground::clay}, {"flowering_azalea", Ground::clay}, {"mangrove_propagule", Ground::clay},
            {"carrots", Ground::farmland}, {"potatoes", Ground::farmland}, {"beetroots", Ground::farmland},
            {"melon_stem", Ground::farmland}, {"pumpkin_stem", Ground::farmland}, {"torchflower_crop", Ground::farmland},
            {"nether_wart", Ground::soul_sand}}};
        std::vector<Kind> v(registry::block_count());
        for (std::uint32_t b = 0; b < v.size(); ++b) {
            const auto name = registry::game_name(b);
            if (name == "torch" || name == "soul_torch") v[b].need = Need::standing;
            else if (name == "wall_torch" || name == "soul_wall_torch") v[b].need = Need::wall;
            else if (name == "lantern" || name == "soul_lantern") v[b].need = Need::lantern;
            else if (name == "ladder") v[b].need = Need::ladder;
            else if (name == "moss_carpet" || (name.ends_with("_carpet") && name != "pale_moss_carpet")) v[b].need = Need::carpet;
            else if (std::ranges::find(soil_plants, name) != soil_plants.end()) v[b].need = Need::plant;
            else if (const auto it = std::ranges::find(other_plants, name, &std::pair<std::string_view, Ground>::first); it != other_plants.end())
                v[b] = {Need::plant, it->second};
            else if (name == "bamboo_sapling" || name == "bamboo") v[b] = {Need::plant, Ground::bamboo};
            else if (name == "sugar_cane") v[b] = {Need::cane, Ground::cane};
            else if (name == "cactus") v[b] = {Need::cactus, Ground::cactus};
            else if (name == "lily_pad") v[b].need = Need::lily_pad;
            else if (name == "sea_pickle") v[b].need = Need::sea_pickle;
            else if (name == "sunflower" || name == "lilac" || name == "rose_bush" || name == "peony" || name == "tall_grass"
                     || name == "large_fern")
                v[b].need = Need::tall_plant;
            else if ((name.ends_with("_wall_sign") && !name.ends_with("_hanging_sign")) || name.ends_with("_wall_banner")) v[b].need = Need::wall_sign;
            else if ((name.ends_with("_sign") && !name.ends_with("_hanging_sign")) || name.ends_with("_banner")) v[b].need = Need::standing_sign;
            else if (name.ends_with("_bed")) v[b].need = Need::bed;
        }
        return v;
    }();
    return out;
}

// The grounds each block is (plants of those kinds grow on it).
const std::vector<std::uint16_t>& grounds() {
    static const std::vector<std::uint16_t> out = [] {
        std::vector<std::uint16_t> v(registry::block_count());
        for (std::uint32_t b = 0; b < v.size(); ++b) {
            const auto name = registry::game_name(b);
            const bool soil = name == "dirt" || name == "coarse_dirt" || name == "grass_block" || name == "podzol" || name == "mycelium"
                           || name == "rooted_dirt" || name == "moss_block" || name == "pale_moss_block" || name == "mud"
                           || name == "muddy_mangrove_roots" || name == "farmland";
            const bool sand = name == "sand" || name == "red_sand" || name == "suspicious_sand";
            const bool terracotta = name == "terracotta" || (name.ends_with("_terracotta") && !name.ends_with("glazed_terracotta"));
            std::uint16_t g = 0;
            if (soil) g |= bit(Ground::soil) | bit(Ground::dry) | bit(Ground::nether) | bit(Ground::wither) | bit(Ground::clay);
            if (soil && name != "farmland") g |= bit(Ground::bamboo);
            if (sand || terracotta) g |= bit(Ground::dry);
            if (sand || name == "gravel" || name == "suspicious_gravel" || name == "bamboo" || name == "bamboo_sapling") g |= bit(Ground::bamboo);
            if (name == "crimson_nylium" || name == "warped_nylium" || name == "soul_soil") g |= bit(Ground::nether);
            if (name == "netherrack" || name == "soul_sand" || name == "soul_soil") g |= bit(Ground::wither);
            if (name == "clay") g |= bit(Ground::clay);
            if (name == "farmland") g |= bit(Ground::farmland);
            if (name == "soul_sand") g |= bit(Ground::soul_sand);
            // Sugar cane: soil but farmland, and sand (and itself, by its rule);
            // cacti: sand (and themselves).
            if ((soil && name != "farmland") || sand || name == "sugar_cane") g |= bit(Ground::cane);
            if (sand || name == "cactus") g |= bit(Ground::cactus);
            v[b] = g;
        }
        return v;
    }();
    return out;
}

Kind kind_of(BlockState s) {
    if (!s.is(BlockType::other)) return {};
    const auto block = registry::block_of(s.registry_state());
    return block ? kinds()[*block] : Kind{};
}
Need need_of(BlockState s) { return kind_of(s).need; }

// Whether a plant of the ground `g` grows on `below` in `release`.
bool grows_on(Ground g, BlockState below, std::string_view release) {
    if (!below.is(BlockType::other)) return false;
    const auto block = registry::block_of(below.registry_state());
    if (!block || (grounds()[*block] & bit(g)) == 0) return false;
    // Dead bushes and dry grass grow on farmland from 1.21.5.
    return !(g == Ground::dry && release == "1.21.4" && registry::game_name(*block) == "farmland");
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

bool named(BlockState s, std::string_view name) {
    const auto block = s.is(BlockType::other) ? registry::block_of(s.registry_state()) : std::nullopt;
    return block && registry::game_name(*block) == name;
}
// Water or frosted ice, beside sugar cane's ground: any water, a waterlogged
// block's among them.
bool waters_cane(BlockState s) {
    const auto fluid = fluid_state(s);
    return (fluid && (fluid->first == "water" || fluid->first == "flowing_water")) || named(s, "frosted_ice");
}
// Water, lava and bubble columns (not waterlogged blocks).
bool liquid(BlockState s) { return s.is(BlockType::water) || is_lava(s) || named(s, "bubble_column"); }
// Whether the state's collision shape reaches the top of its cell.
bool top_reached(BlockState s, std::string_view release) {
    return std::ranges::any_of(physics::collision_boxes(s, release), [](const registry::Box& b) {
        return b.min_y < 1.0 && b.max_y >= 1.0 && b.max_x > b.min_x && b.max_z > b.min_z;
    });
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

bool supported(BlockState s, const Near& near, std::string_view release) {
    const auto around = [&](Direction d) { return near(BlockPos{}.relative(d)); };
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
        // A hanging mangrove propagule hangs under leaves, which the rules do
        // not check yet.
        if (value_of(s, "hanging") == "true") return true;
        return grows_on(kind_of(s).ground, around(Direction::down), release);
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
    // Sugar cane stands on sugar cane, or on its ground with water (or
    // frosted ice) beside the ground.
    case Need::cane: {
        const auto below = around(Direction::down);
        if (same_block(below, s)) return true;
        if (!grows_on(Ground::cane, below, release)) return false;
        return std::ranges::any_of(contracts::horizontal_directions, [&](Direction d) {
            return waters_cane(near(BlockPos{0, -1, 0}.relative(d)));
        });
    }
    // A cactus stands on a cactus or sand, with nothing solid or lava beside
    // it and no liquid above it.
    case Need::cactus:
        for (const auto d : contracts::horizontal_directions) {
            const auto side = around(d);
            if (solid_block(side) || is_lava(side)) return false;
        }
        return !liquid(around(Direction::up)) && grows_on(Ground::cactus, around(Direction::down), release);
    // A lily pad lies on a water source or ice.
    case Need::lily_pad: {
        const auto below = around(Direction::down);
        const auto fluid = fluid_state(below);
        return (fluid && fluid->first == "water") || named(below, "ice") || named(below, "frosted_ice");
    }
    // A sea pickle sits on a top its collision shape reaches, or a sturdy one.
    case Need::sea_pickle: return top_reached(around(Direction::down), release) || sturdy(around(Direction::down), Direction::up, release);
    default: return true;
    }
}

bool supported_at(RuleContext& ctx, BlockPos pos, BlockState s) {
    return supported(s, [&](BlockPos o) { return ctx.block({pos.x + o.x, pos.y + o.y, pos.z + o.z}); }, ctx.bundle().schema.content_release);
}

bool checks_later(BlockState s) { return need_of(s) == Need::cane || need_of(s) == Need::cactus; }

void check_support_later(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (checks_later(s) && !supported_at(ctx, pos, s)) schedule(ctx, TickList::block, pos, BlockType::other, 1, 0);
}

void support_tick(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (checks_later(s) && !supported_at(ctx, pos, s)) destroy_block(ctx, pos);
}

void support_shape_update(RuleContext& ctx, BlockPos pos, BlockState s, Direction d) {
    // Sugar cane and cacti look again a tick after any update.
    if (checks_later(s)) {
        check_support_later(ctx, pos, s);
        return;
    }
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
    // A wall torch, sign or banner drops the item of the standing one; young
    // crops and a bamboo shoot drop what their loot tables give them.
    auto name = std::string("minecraft:") + std::string(registry::game_name(*registry::block_of(s.registry_state())));
    if (const auto wall = name.find("wall_"); wall != std::string::npos) name.erase(wall, 5);
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 5> crops{{{"minecraft:carrots", "minecraft:carrot"},
        {"minecraft:potatoes", "minecraft:potato"}, {"minecraft:beetroots", "minecraft:beetroot_seeds"},
        {"minecraft:torchflower_crop", "minecraft:torchflower_seeds"}, {"minecraft:bamboo_sapling", "minecraft:bamboo"}}};
    if (const auto it = std::ranges::find(crops, name, &std::pair<std::string_view, std::string_view>::first); it != crops.end()) {
        // Ripe ones drop more, which the rules do not give without data packs.
        if (value_of(s, "age") != "0" && name != "minecraft:bamboo_sapling") return std::nullopt;
        name = it->second;
    }
    // Grasses, ferns, bushes and sprouts drop nothing without shears (but a
    // seed or stick a random roll may give), nor do stems and berry bushes
    // without random rolls; petals drop one for each flower.
    static constexpr std::array<std::string_view, 12> nothing{
        "minecraft:fern", "minecraft:tall_grass", "minecraft:large_fern", "minecraft:short_grass", "minecraft:dead_bush", "minecraft:bush",
        "minecraft:short_dry_grass", "minecraft:tall_dry_grass", "minecraft:nether_sprouts", "minecraft:melon_stem", "minecraft:pumpkin_stem",
        "minecraft:sweet_berry_bush"};
    if (std::ranges::find(nothing, name) != nothing.end()) return std::nullopt;
    if (name == "minecraft:pink_petals" || name == "minecraft:wildflowers" || name == "minecraft:sea_pickle") {
        const auto item = items::by_name(name);
        const auto count = std::stoi(value_of(s, name == "minecraft:sea_pickle" ? "pickles" : "flower_amount"));
        return item ? std::optional(ItemStack{*item, static_cast<std::uint8_t>(count)}) : std::nullopt;
    }
    const auto item = items::by_name(name);
    return item ? std::optional(ItemStack{*item, 1}) : std::nullopt;
}

} // namespace circuit::gameplay::detail

namespace circuit::gameplay {
bool block_supported(BlockState s, const std::function<BlockState(contracts::Direction)>& around, std::string_view release) {
    return detail::supported(
        s,
        [&](contracts::BlockPos o) {
            for (const auto d : contracts::all_directions)
                if (contracts::BlockPos{}.relative(d) == o) return around(d);
            return blocks::air();
        },
        release);
}
bool block_ticks_scheduled(BlockState s) { return detail::falls(s) || detail::checks_later(s); }
bool block_supported_near(BlockState s, const std::function<BlockState(contracts::BlockPos offset)>& near, std::string_view release) {
    return detail::supported(s, near, release);
}
} // namespace circuit::gameplay
