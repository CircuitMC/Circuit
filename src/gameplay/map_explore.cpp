// SPDX-License-Identifier: AGPL-3.0-only
// Held maps exploring the terrain as both official servers draw it
// (client/maps_explore, both versions identical; minecraft.wiki "Map" and
// "Map item format"). The colors of block states are in map_colors.cpp.
#include "internal.hpp"
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/items.hpp>
#include <circuit/gameplay/maps.hpp>
#include <circuit/gameplay/rules.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace circuit::gameplay {
namespace {

// A column as a map sees it from above: the color of its highest block that
// is not NONE, that block's height and the depth of the water from it down
// (the blocks holding water, waterlogged ones too).
struct Column {
    std::uint8_t color{};
    std::int32_t height{};
    std::int32_t depth{};
};

Column column(RuleContext& ctx, const MapLooks& looks, std::int32_t top, std::int32_t x, std::int32_t z) {
    const auto floor = ctx.world().min_y;
    // A column without blocks (the End's void) is drawn as stone.
    if (top < floor) return {map_stone, floor, 0};
    for (auto y = top; y >= floor; --y) {
        const auto look = looks.of(ctx.block({x, y, z}));
        if (look.color == map_none) continue;
        Column c{look.color, y, 0};
        if (look.color == map_water)
            for (auto w = y; w >= floor && looks.of(ctx.block({x, w, z})).water; --w) ++c.depth;
        return c;
    }
    // NONE blocks over the void only: nothing is drawn.
    return {map_none, floor, 0};
}

// A pixel: the most common color of its n by n columns (taken x by x, z by z
// within), ties going to the color met first, with the sums of their heights
// and of their water's depths; nullopt where the columns are not loaded (a
// pixel's columns lie in one chunk).
struct Pixel {
    std::uint8_t color{};
    std::int64_t height{};
    std::int64_t depth{};
};

std::optional<Pixel> pixel(RuleContext& ctx, const MapLooks& looks, std::int32_t x0, std::int32_t z0, std::int32_t n) {
    std::array<std::uint16_t, 64> counts{};
    std::array<std::uint8_t, 64> order{};
    std::size_t kinds = 0;
    Pixel p;
    for (std::int32_t i = 0; i < n; ++i)
        for (std::int32_t j = 0; j < n; ++j) {
            const auto top = ctx.column_top(x0 + i, z0 + j);
            if (!top) return std::nullopt;
            const auto c = column(ctx, looks, *top, x0 + i, z0 + j);
            if (counts[c.color & 63]++ == 0) order[kinds++] = c.color & 63;
            p.height += c.height;
            p.depth += c.depth;
        }
    std::uint16_t best = 0;
    for (std::size_t k = 0; k < kinds; ++k)
        if (counts[order[k]] > best) {
            best = counts[order[k]];
            p.color = order[k];
        }
    return p;
}

// The shade of a pixel (0 dark, 1 normal, 2 bright) at parity `odd` ((x + z)
// & 1): water by the mean depth of its columns (rounded down: 1 to 2 bright,
// 3 to 4 bright on even pixels, 7 to 9 dark on odd ones, from 10 dark),
// other colors by how much higher its columns stand than the pixel north of
// it's, as a fraction of n^2 (n + 4) / 20 that the parity shifts.
std::uint8_t shade(const Pixel& p, std::int64_t north, std::int32_t n, std::int64_t odd) {
    const std::int64_t area = std::int64_t{n} * n;
    if (p.color == map_water) {
        const auto d = p.depth / area;
        if (d + 2 * odd < 5) return 2;
        return (odd ? d >= 7 : d >= 10) ? 0 : 1;
    }
    const auto unit = area * (n + 4);
    const auto rise = 20 * (p.height - north);
    if (rise >= (4 - 2 * odd) * unit) return 2;
    if (rise <= -(2 + 2 * odd) * unit) return 0;
    return 1;
}

std::optional<std::int32_t> held_map(const GameData* data, const ItemStack& s) {
    if (!data || !data->int_component || s.empty() || s.item != items::filled_map) return std::nullopt;
    return data->int_component(s.components, "map_id");
}

bool carries(const GameData* data, const PlayerInventory& inv, std::int32_t id) {
    return std::ranges::any_of(inv.slots, [&](const ItemStack& s) { return held_map(data, s) == id; });
}

} // namespace

// Each explore counts the holder's step; it scans the map's columns x (in
// pixels, within r of the player's) whose x is the step modulo 16, and also
// the next column after one in which a pixel changed, so a new map fills at
// once from there on. In a column it draws the pixels within r of the
// player's, r being 128 blocks (in pixels), those in the outer two pixels
// only on odd pixels (x + z). Maps of levels with a ceiling (the Nether) are
// not explored: their pattern is not described publicly.
bool explore_map(RuleContext& ctx, MapData& map, contracts::PlayerKey player, contracts::Vec3 position) {
    if (ctx.dimension() == contracts::DimensionId::the_nether) return false;
    auto holder = std::ranges::find(map.holders, player, &MapHolder::player);
    if (holder == map.holders.end()) holder = map.holders.insert(map.holders.end(), MapHolder{player, 0});
    const auto step = static_cast<std::int32_t>(++holder->step & 15);
    const auto& looks = map_looks(ctx.bundle().schema.content_release);
    const std::int32_t n = 1 << map.scale;
    const std::int32_t r = 128 / n;
    const auto side = static_cast<std::int32_t>(map_side);
    const auto px = static_cast<std::int32_t>(std::floor((position.x - map.x_center) / n)) + side / 2;
    const auto pz = static_cast<std::int32_t>(std::floor((position.z - map.z_center) / n)) + side / 2;
    bool changed = false, chain = false;
    for (auto x = px - r + 1; x < px + r; ++x) {
        if ((x & 15) != step && !chain) continue;
        chain = false;
        if (x < 0 || x >= side) continue;
        const auto x0 = (map.x_center / n + x - side / 2) * n;
        // The pixel north of the first drawn one is computed too: outside
        // the map, row -1.
        std::int64_t north = 0;
        for (auto z = std::max(pz - r, -1); z < std::min(pz + r, side); ++z) {
            const auto p = pixel(ctx, looks, x0, (map.z_center / n + z - side / 2) * n, n);
            if (!p) continue;
            const auto s = shade(*p, north, n, (x + z) & 1);
            north = p->height;
            const auto dx = x - px, dz = z - pz, d2 = dx * dx + dz * dz;
            if (z < 0 || d2 >= r * r || (d2 > (r - 2) * (r - 2) && ((x + z) & 1) == 0)) continue;
            auto& cell = map.colors[static_cast<std::size_t>(x + z * side)];
            const auto value = static_cast<std::uint8_t>(p->color * 4 + s);
            if (cell == value) continue;
            cell = value;
            changed = chain = true;
        }
    }
    return changed;
}

void forget_map_holders(RuleContext& ctx) {
    for (const auto id : ctx.map_ids()) {
        auto* map = ctx.map(id);
        if (!map || map->holders.empty()) continue;
        std::erase_if(map->holders, [&](const MapHolder& h) {
            const auto* inv = ctx.peek_player(h.player);
            return !inv || !carries(ctx.data(), *inv, id);
        });
    }
}

void explore_held_maps(RuleContext& ctx) {
    for (const auto& presence : ctx.presences()) {
        if (presence.spectator) continue;
        const auto* inv = ctx.peek_player(presence.player);
        if (!inv) continue;
        for (const auto slot : {static_cast<std::size_t>(contracts::hotbar_first + presence.held), std::size_t{contracts::offhand_slot}}) {
            const auto id = held_map(ctx.data(), inv->slots[slot]);
            if (!id) continue;
            auto* map = ctx.map(*id);
            if (map && !map->locked && map->dimension == ctx.dimension()) explore_map(ctx, *map, presence.player, presence.position);
        }
    }
}

} // namespace circuit::gameplay
