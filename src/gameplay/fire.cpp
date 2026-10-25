// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <string_view>

// Fire (both official servers; fire_basics, fire_stone, fire_netherrack,
// fire_planks, fire_log, fire_tnt, fire_wool; which blocks burn,
// tests/fixtures/collision/fire-*.txt; the odds, minecraft.wiki "Fire"):
// - fire stays on a block whose top is sturdy or beside a flammable block,
//   soul fire on soul sand and soul soil; an update that finds it without
//   that removes it. Fire takes the faces (north, east, south, west, up) of
//   its flammable neighbors when the block below neither bears nor feeds it;
// - every state set for fire, its own changes of age included, draws the
//   delay of its block tick, 30 + nextInt(10), from the level random (a tick
//   already scheduled is kept); fire set where it cannot stay is removed
//   after that draw;
// - in its block tick it draws the next delay, and then, with doFireTick
//   (and from 1.21.5 a player near the chunk, as for random ticks, or
//   allowFireTicksAwayFromPlayer): unsupported, it goes; it ages by
//   nextInt(3) / 2; unless the block below is netherrack or magma, without a
//   flammable neighbor it goes out when the block below is not sturdy or it
//   was older than 3, and at age 15 it goes out one time in four with nothing
//   flammable below; then, by its age before the tick, it tries to burn its
//   six neighbors (east, west, down, up, north, south: nextInt(300, or 250
//   up and down) below their burn odds; then fire takes their place one
//   time in (age + 10) / 5, of its age + nextInt(5) / 4, else they are
//   removed; TNT burned is primed) and to spread to the empty cells from one
//   below to four above it beside flammable blocks: x, then z, then y, with
//   nextInt(100, 100 more for each level above the next) at most (their best
//   ignite odds + 40 + 7 * difficulty) / (age + 30).
// Not modeled: rain and humid biomes, entities in fire, lava and lightning
// setting fire, flint and steel.
namespace circuit::gameplay::detail {
namespace {
struct Odds {
    std::uint8_t ignite = 0, burn = 0;
};

// The ignite and burn odds of a block by its name.
Odds odds_of(std::string_view n) {
    const auto ends = [&](std::string_view suffix) { return n.ends_with(suffix); };
    if (n.starts_with("crimson") || n.starts_with("warped") || n.starts_with("stripped_crimson") || n.starts_with("stripped_warped")
        || n == "nether_brick_fence" || n == "petrified_oak_slab")
        return {};
    if (ends("_log") || ends("_wood") || n == "bamboo_block" || n == "stripped_bamboo_block" || n == "coal_block") return {5, 5};
    if (ends("_planks") || ends("_slab") || ends("_stairs") || ends("_fence") || ends("_fence_gate")) {
        static constexpr std::array<std::string_view, 11> woods{"oak", "spruce", "birch", "jungle", "acacia", "dark_oak",
                                                                "mangrove", "cherry", "pale_oak", "bamboo", "bamboo_mosaic"};
        for (const auto w : woods)
            for (const auto kind : {"_planks", "_slab", "_stairs", "_fence", "_fence_gate"})
                if (n == std::string(w) + kind) return {5, 20};
        return {};
    }
    if (n == "bamboo_mosaic" || n == "composter" || n == "beehive" || n == "mangrove_roots") return {5, 20};
    if (n == "pale_moss_block" || n == "pale_moss_carpet" || n == "pale_hanging_moss") return {5, 100};
    if (n == "cave_vines" || n == "cave_vines_plant") return {15, 60};
    if (n == "tnt" || n == "vine" || n == "glow_lichen") return {15, 100};
    if (n == "target") return {15, 20};
    if (n == "bookshelf" || n == "lectern" || n == "bee_nest") return {30, 20};
    if (ends("_leaves") || ends("_wool") || n == "hanging_roots" || n == "azalea" || n == "flowering_azalea" || n == "dried_kelp_block")
        return {30, 60};
    if ((ends("_carpet") && n != "moss_carpet") || n == "hay_block") return {60, 20};
    if (n == "bamboo" || n == "scaffolding") return {60, 60};
    static constexpr std::array<std::string_view, 39> plants{
        "dandelion", "poppy", "blue_orchid", "allium", "azure_bluet", "red_tulip", "orange_tulip", "white_tulip", "pink_tulip",
        "oxeye_daisy", "cornflower", "lily_of_the_valley", "wither_rose", "torchflower", "open_eyeblossom", "closed_eyeblossom",
        "sweet_berry_bush", "sunflower", "lilac", "rose_bush", "peony", "short_grass", "tall_grass", "fern", "large_fern",
        "short_dry_grass", "tall_dry_grass", "dead_bush", "pitcher_plant", "cactus_flower", "firefly_bush", "bush", "big_dripleaf",
        "big_dripleaf_stem", "small_dripleaf", "spore_blossom", "pink_petals", "wildflowers", "leaf_litter"};
    if (std::ranges::find(plants, n) != plants.end()) return {60, 100};
    return {};
}

const std::vector<Odds>& block_odds() {
    static const std::vector<Odds> out = [] {
        std::vector<Odds> v(registry::block_count());
        for (std::uint32_t b = 0; b < v.size(); ++b) v[b] = odds_of(registry::game_name(b));
        return v;
    }();
    return out;
}

std::optional<std::uint32_t> block_named(BlockState s) {
    const auto state = physics::registry_state_of(s);
    return state ? registry::block_of(*state) : std::nullopt;
}

std::string_view name_of(BlockState s) {
    const auto block = block_named(s);
    return block ? registry::game_name(*block) : std::string_view{};
}

// A waterlogged block does not burn.
Odds odds(BlockState s) {
    const auto state = physics::registry_state_of(s);
    const auto block = state ? registry::block_of(*state) : std::nullopt;
    if (!block || block_odds()[*block].ignite == 0) return {};
    for (const auto& [k, v] : registry::properties(*state))
        if (k == "waterlogged" && v == "true") return {};
    return block_odds()[*block];
}

bool flammable(BlockState s) { return odds(s).ignite > 0; }

bool sturdy_top(BlockState s, std::string_view release) {
    const auto state = physics::registry_state_of(s);
    return state && registry::sturdy_face(*state, Direction::up, release);
}

bool infiniburn(BlockState s) {
    const auto n = name_of(s);
    return n == "netherrack" || n == "magma_block";
}

bool flammable_beside(RuleContext& ctx, BlockPos pos) {
    return std::ranges::any_of(contracts::all_directions, [&](Direction d) { return flammable(ctx.block(pos.relative(d))); });
}

int age_of(BlockState s) {
    for (const auto& [k, v] : registry::properties(s.registry_state()))
        if (k == "age") return std::stoi(v);
    return 0;
}

// Fire of an age at a cell: with the faces of its flammable neighbors when
// the block below neither bears nor feeds it.
BlockState fire_of_age(RuleContext& ctx, BlockPos pos, int age) {
    const auto release = ctx.bundle().schema.content_release;
    const auto below = ctx.block(pos.relative(Direction::down));
    const bool hanging = !flammable(below) && !sturdy_top(below, release);
    const auto face = [&](Direction d) -> std::string { return hanging && flammable(ctx.block(pos.relative(d))) ? "true" : "false"; };
    const std::vector<registry::Property> props{{"age", std::to_string(age)}, {"east", face(Direction::east)},
                                                {"north", face(Direction::north)}, {"south", face(Direction::south)},
                                                {"up", face(Direction::up)}, {"west", face(Direction::west)}};
    return BlockState::registered(registry::state_of(*registry::find_block(release, "fire"), props).value());
}

void schedule_fire(RuleContext& ctx, BlockPos pos) {
    const auto delay = static_cast<std::uint32_t>(30 + ctx.random_int(10));
    schedule(ctx, TickList::block, pos, BlockType::other, delay, 0);
}

bool stays(RuleContext& ctx, BlockPos pos, BlockState s) {
    return fire_supported(s, [&](BlockPos o) { return ctx.block({pos.x + o.x, pos.y + o.y, pos.z + o.z}); },
                          ctx.bundle().schema.content_release);
}

// A neighbor burning: fire takes its place or it is gone; TNT is primed.
void burn(RuleContext& ctx, BlockPos pos, int chance, int age) {
    const auto s = ctx.block(pos);
    if (ctx.random_int(chance) >= odds(s).burn) return;
    if (ctx.random_int(age + 10) < 5) set_block(ctx, pos, fire_of_age(ctx, pos, std::min(age + ctx.random_int(5) / 4, 15)));
    else set_block(ctx, pos, blocks::air());
    if (is_tnt(s)) prime_burned(ctx, pos);
}

// The best ignite odds beside an empty cell; 0 for a cell that is not empty.
int ignite_odds_at(RuleContext& ctx, BlockPos pos) {
    if (!ctx.block(pos).is_air()) return 0;
    int best = 0;
    for (const auto d : contracts::all_directions) best = std::max<int>(best, odds(ctx.block(pos.relative(d))).ignite);
    return best;
}
} // namespace

bool is_fire(BlockState s) { return s.is(BlockType::other) && name_of(s) == "fire"; }
bool is_soul_fire(BlockState s) { return s.is(BlockType::other) && name_of(s) == "soul_fire"; }

bool fire_supported(BlockState s, const Near& near, std::string_view release) {
    const auto below = near({0, -1, 0});
    if (is_soul_fire(s)) {
        const auto n = name_of(below);
        return n == "soul_sand" || n == "soul_soil";
    }
    return sturdy_top(below, release)
        || std::ranges::any_of(contracts::all_directions, [&](Direction d) { return flammable(near(BlockPos{}.relative(d))); });
}

void fire_placed(RuleContext& ctx, BlockPos pos, BlockState s, BlockState previous) {
    if (is_fire(s)) schedule_fire(ctx, pos);
    if (!same_block(previous, s) && !stays(ctx, pos, s)) set_block(ctx, pos, blocks::air());
}

void fire_shape_update(RuleContext& ctx, BlockPos pos, BlockState s) {
    if (!stays(ctx, pos, s)) set_block(ctx, pos, blocks::air());
    else if (is_fire(s)) set_block_quiet(ctx, pos, fire_of_age(ctx, pos, age_of(s)));
}

void tick_fire(RuleContext& ctx, BlockPos pos, BlockState s) {
    schedule_fire(ctx, pos);
    if (ctx.game_rule("doFireTick") == 0) return;
    // From 1.21.5 fire acts only near players, unless the game rule says.
    if (ctx.bundle().schema.game_rule("allowFireTicksAwayFromPlayer") && ctx.game_rule("allowFireTicksAwayFromPlayer") == 0
        && !ctx.player_near(contracts::chunk_of(pos)))
        return;
    if (!stays(ctx, pos, s)) {
        set_block(ctx, pos, blocks::air());
        return;
    }
    const auto release = ctx.bundle().schema.content_release;
    const auto below = ctx.block(pos.relative(Direction::down));
    const int age = age_of(s);
    if (const int older = std::min(15, age + ctx.random_int(3) / 2); older != age) set_block_quiet(ctx, pos, fire_of_age(ctx, pos, older));
    if (!infiniburn(below)) {
        if (!flammable_beside(ctx, pos)) {
            if (!sturdy_top(below, release) || age > 3) set_block(ctx, pos, blocks::air());
            return;
        }
        if (age == 15 && ctx.random_int(4) == 0 && !flammable(below)) {
            set_block(ctx, pos, blocks::air());
            return;
        }
    }
    constexpr std::array<std::pair<Direction, int>, 6> sides{{{Direction::east, 300}, {Direction::west, 300}, {Direction::down, 250},
                                                              {Direction::up, 250}, {Direction::north, 300}, {Direction::south, 300}}};
    for (const auto& [d, chance] : sides) burn(ctx, pos.relative(d), chance, age);
    const int difficulty = ctx.difficulty();
    for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 4; ++dy) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                const BlockPos q{pos.x + dx, pos.y + dy, pos.z + dz};
                const int ignite = ignite_odds_at(ctx, q);
                if (ignite <= 0) continue;
                const int chance = (ignite + 40 + difficulty * 7) / (age + 30);
                const int bound = dy > 1 ? 100 + (dy - 1) * 100 : 100;
                if (chance > 0 && ctx.random_int(bound) <= chance)
                    set_block(ctx, q, fire_of_age(ctx, q, std::min(15, age + ctx.random_int(5) / 4)));
            }
}

} // namespace circuit::gameplay::detail
