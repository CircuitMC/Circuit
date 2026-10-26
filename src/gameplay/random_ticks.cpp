// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <vector>

// Which block states take random ticks (both official servers;
// tests/fixtures/collision/random-ticks-*.txt): grass blocks, mycelium and
// nylium, saplings, mushrooms, lava, farmland, snow layers, ice, cacti, sugar
// cane, stems, vines, turtle eggs, budding amethyst, copper that can still
// weather, pointed dripstone, nether portals, eyeblossoms and the other
// listed blocks in every state; leaves only at distance 7 and not persistent;
// crops, nether wart, cocoa, sweet berry bushes, chorus flowers, kelp and the
// growing vines below their last age; a pitcher crop's lower half below age
// 4; bamboo at stage 0; redstone ore while lit.
namespace circuit::gameplay {
namespace {
int property(std::uint32_t state, std::string_view key, int otherwise) {
    for (const auto& [k, v] : registry::properties(state))
        if (k == key) return v == "true" ? 1 : v == "false" ? 0 : std::stoi(v);
    return otherwise;
}

bool copper_that_weathers(std::string_view n) {
    if (!n.contains("copper") || n.contains("oxidized") || n.contains("waxed") || n.contains("ore") || n.starts_with("raw_")) return false;
    return n.starts_with("copper_") || n.starts_with("exposed_") || n.starts_with("weathered_") || n.starts_with("cut_copper")
        || n.starts_with("chiseled_copper");
}

bool ticks(std::uint32_t state) {
    const auto n = registry::game_name(*registry::block_of(state));
    static constexpr std::array<std::string_view, 27> always{
        "grass_block", "mangrove_propagule", "lava", "brown_mushroom", "red_mushroom", "farmland", "snow", "ice", "cactus", "sugar_cane",
        "nether_portal", "pumpkin_stem", "melon_stem", "vine", "mycelium", "torchflower_crop", "turtle_egg", "dried_ghast", "bamboo_sapling",
        "warped_nylium", "crimson_nylium", "budding_amethyst", "pointed_dripstone", "open_eyeblossom", "closed_eyeblossom",
        "potted_open_eyeblossom", "potted_closed_eyeblossom"};
    if (std::ranges::find(always, n) != always.end()) return true;
    if (n.ends_with("_sapling") && !n.starts_with("potted_")) return true;
    if (copper_that_weathers(n)) return true;
    if (n.ends_with("_leaves")) return property(state, "distance", 0) == 7 && property(state, "persistent", 1) == 0;
    if (n == "wheat" || n == "carrots" || n == "potatoes") return property(state, "age", 7) < 7;
    if (n == "beetroots" || n == "nether_wart" || n == "sweet_berry_bush") return property(state, "age", 3) < 3;
    if (n == "cocoa") return property(state, "age", 2) < 2;
    if (n == "chorus_flower") return property(state, "age", 5) < 5;
    if (n == "kelp" || n == "weeping_vines" || n == "twisting_vines" || n == "cave_vines") return property(state, "age", 25) < 25;
    if (n == "pitcher_crop") {
        bool lower = false;
        for (const auto& [k, v] : registry::properties(state))
            if (k == "half") lower = v == "lower";
        return lower && property(state, "age", 4) < 4;
    }
    if (n == "bamboo") return property(state, "stage", 1) == 0;
    if (n == "redstone_ore" || n == "deepslate_redstone_ore") return property(state, "lit", 0) == 1;
    return false;
}

const std::vector<bool>& ticking_states() {
    static const std::vector<bool> out = [] {
        std::vector<bool> v(registry::state_count() + 1, false);
        for (std::uint32_t s = 1; s <= registry::state_count(); ++s) v[s] = ticks(s);
        return v;
    }();
    return out;
}
} // namespace

bool takes_random_ticks(BlockState s) {
    const auto state = physics::registry_state_of(s);
    return state && ticking_states()[*state];
}

bool ticks_section_randomly(BlockState s) {
    if (!s.is(BlockType::other)) return takes_random_ticks(s) || detail::holds_water_fluid(s);
    static const std::vector<bool> out = [] {
        std::vector<bool> v(registry::state_count() + 1, false);
        for (std::uint32_t st = 1; st <= registry::state_count(); ++st) v[st] = ticks(st) || detail::holds_water_fluid(BlockState::registered(st));
        return v;
    }();
    return out[s.registry_state()];
}

// The order (both official servers, fitted to every pick of farmland in
// forced areas of 4 and 16 chunks, random_ticks): the chunks are kept in an
// open hash table of their keys (x in the low 32 bits, z in the high),
// 64 slots while they fill at most three quarters of it: the key 0 (the
// chunk at 0, 0) first, then the slots from the top down, a key at the slot
// of its mix (see below) or, when that is taken, the next free one up. The
// official servers fill the table as chunks start to tick; Circuit fills it
// nearest the chunks' center first (ties by x, then z), which gives the
// order observed for forced areas, not that of a history of loads.
std::vector<ChunkPos> random_tick_order(std::vector<ChunkPos> chunks) {
    if (chunks.empty()) return chunks;
    double cx = 0.0, cz = 0.0;
    for (const auto c : chunks) {
        cx += c.x;
        cz += c.z;
    }
    cx /= static_cast<double>(chunks.size());
    cz /= static_cast<double>(chunks.size());
    std::ranges::sort(chunks, [&](ChunkPos a, ChunkPos b) {
        const double da = (a.x - cx) * (a.x - cx) + (a.z - cz) * (a.z - cz);
        const double db = (b.x - cx) * (b.x - cx) + (b.z - cz) * (b.z - cz);
        return da != db ? da < db : a.x != b.x ? a.x < b.x : a.z < b.z;
    });
    std::size_t size = 64;
    while (chunks.size() * 4 > size * 3) size *= 2;
    const auto mix = [](std::uint64_t k) {
        std::uint64_t h = k * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 32;
        return h ^ (h >> 16);
    };
    std::vector<std::optional<ChunkPos>> table(size);
    bool zero = false;
    for (const auto c : chunks) {
        const auto key = static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x)) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.z)) << 32);
        if (key == 0) {
            zero = true;
            continue;
        }
        auto slot = static_cast<std::size_t>(mix(key)) & (size - 1);
        while (table[slot]) slot = (slot + 1) & (size - 1);
        table[slot] = c;
    }
    std::vector<ChunkPos> out;
    if (zero) out.push_back({0, 0});
    for (auto slot = size; slot-- > 0;)
        if (table[slot]) out.push_back(*table[slot]);
    return out;
}

// One chunk's random ticks (both official servers, 1.21.8; random_ticks): with
// randomTickSpeed n, n precipitation checks, each a nextInt(48) of the level
// random that advances the position generator when it is 0 (snow and ice
// forming, cauldrons filling and lightning are not modeled), then n picks in
// each section that holds blocks taking random ticks or water, from the
// bottom up: the generator's next value v gives x = v >> 2 & 15,
// z = v >> 10 & 15 and y = v >> 18 & 15 in the section, and the block there
// takes its random tick when it is one that takes them (crops_growth,
// stems_growth, grass_spread; crops.cpp, grass.cpp, farmland.cpp).
void run_random_ticks(RuleContext& ctx, ChunkPos chunk, std::span<const std::int32_t> sections) {
    const auto speed = ctx.game_rule("randomTickSpeed");
    if (speed <= 0) return;
    for (std::int32_t k = 0; k < speed; ++k)
        if (ctx.random_int(48) == 0) (void)ctx.random_tick_pick();
    for (const auto section : sections) {
        const auto base_y = ctx.world().min_y + section * 16;
        for (std::int32_t k = 0; k < speed; ++k) {
            const auto v = ctx.random_tick_pick() >> 2;
            const BlockPos pos{chunk.x * 16 + static_cast<std::int32_t>(v & 15), base_y + static_cast<std::int32_t>((v >> 16) & 15),
                               chunk.z * 16 + static_cast<std::int32_t>((v >> 8) & 15)};
            const auto s = ctx.block(pos);
            if (!takes_random_ticks(s)) continue;
            if (detail::is_crop(s)) detail::crop_random_tick(ctx, pos, s);
            else if (detail::is_stem(s)) detail::stem_random_tick(ctx, pos, s);
            else if (detail::is_spreading_dirt(s)) detail::spreading_dirt_random_tick(ctx, pos, s);
            else if (detail::is_farmland(s)) detail::farmland_random_tick(ctx, pos, s);
            else if (detail::is_growing_plant(s)) detail::plant_random_tick(ctx, pos, s);
        }
    }
}

} // namespace circuit::gameplay
