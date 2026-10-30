// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/game_data.hpp>
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

// The special crafting recipes (see recipes.hpp; both official servers,
// client/special_a). Armor dye mixes colors as minecraft.wiki "Dye" gives
// it: the sums of the red, green and blue of the item's color (if any) and
// of each dye, and of the largest channel of each, all divided by how many;
// the averages then scaled by the average largest channel over the largest
// average, in integer channels. The dyes' colors are minecraft.wiki's; their
// firework colors are the ones both official servers give stars
// (client/fireworks), also minecraft.wiki's.
namespace circuit::gameplay::detail {
namespace {
constexpr std::array<std::string_view, 16> colors{"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                                  "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};
constexpr std::array<std::uint32_t, 16> dye_rgb{0xF9FFFE, 0xF9801D, 0xC74EBD, 0x3AB3DA, 0xFED83D, 0x80C71F, 0xF38BAA, 0x474F52,
                                                0x9D9D97, 0x169C9C, 0x8932B8, 0x3C44AA, 0x835432, 0x5E7C16, 0xB02E26, 0x1D1D21};
constexpr std::array<std::int32_t, 16> firework_rgb{0xF0F0F0, 0xEB8844, 0xC354CD, 0x6689D3, 0xDECF2A, 0x41CD34, 0xD88198, 0x434343,
                                                    0xABABAB, 0x287697, 0x7B2FBE, 0x253192, 0x51301A, 0x3B511A, 0xB3312C, 0x1E1B1B};
// A star's shape item: a fire charge a large ball (1), a gold nugget a star
// (2), a mob head a creeper (3), a feather a burst (4).
std::optional<std::int32_t> shape_of(std::uint16_t item) {
    switch (item) {
    case items::fire_charge: return 1;
    case items::gold_nugget: return 2;
    case items::skeleton_skull:
    case items::wither_skeleton_skull:
    case items::zombie_head:
    case items::player_head:
    case items::creeper_head:
    case items::dragon_head:
    case items::piglin_head: return 3;
    case items::feather: return 4;
    default: return std::nullopt;
    }
}

std::string_view name_of(std::uint16_t item) {
    const auto* info = items::info(item);
    if (!info) return {};
    std::string_view n = info->name;
    if (n.starts_with("minecraft:")) n.remove_prefix(10);
    return n;
}
// The color a name with `suffix` starts with (red_dye, red_banner), if any.
std::optional<std::uint32_t> color_of(std::uint16_t item, std::string_view suffix) {
    const auto n = name_of(item);
    for (std::uint32_t k = 0; k < colors.size(); ++k)
        if (n.size() == colors[k].size() + suffix.size() && n.starts_with(colors[k]) && n.ends_with(suffix)) return k;
    return std::nullopt;
}

std::int32_t mixed(std::optional<std::int32_t> base, std::span<const std::uint32_t> dyes) {
    int r = 0, g = 0, b = 0, most = 0, n = 0;
    const auto add = [&](std::uint32_t c) {
        const int cr = static_cast<int>(c >> 16 & 0xFF), cg = static_cast<int>(c >> 8 & 0xFF), cb = static_cast<int>(c & 0xFF);
        r += cr;
        g += cg;
        b += cb;
        most += std::max({cr, cg, cb});
        ++n;
    };
    if (base) add(static_cast<std::uint32_t>(*base));
    for (const auto d : dyes) add(dye_rgb[d]);
    const int ar = r / n, ag = g / n, ab = b / n;
    const float gain = static_cast<float>(most / n) / static_cast<float>(std::max({ar, ag, ab}));
    const auto scaled = [&](int v) { return static_cast<std::int32_t>(static_cast<float>(v) * gain); };
    return scaled(ar) << 16 | scaled(ag) << 8 | scaled(ab);
}
} // namespace

std::optional<SpecialMade> special_crafting(const GameData& data, std::span<const ItemStack> grid) {
    std::vector<std::size_t> used;
    for (std::size_t k = 0; k < grid.size(); ++k)
        if (!grid[k].empty()) used.push_back(k);
    if (used.empty()) return std::nullopt;
    using Kind = recipes::Special::Kind;
    for (const auto& special : data.special) {
        switch (special.kind) {
        case Kind::repair_item: {
            if (used.size() != 2) break;
            const auto& a = grid[used[0]];
            const auto& b = grid[used[1]];
            const int most = items::max_damage(a);
            if (a.item != b.item || most <= 0 || items::max_stack(a) > 1) break;
            ItemStack made{a.item, 1};
            made.damage = static_cast<std::uint16_t>(std::max(most - ((most - a.damage) + (most - b.damage) + most * 5 / 100), 0));
            // The curses of both, each at its higher level.
            if (data.enchantments_of && data.with_enchantments) {
                std::vector<std::pair<std::uint32_t, std::int32_t>> curses;
                for (const auto* input : {&a, &b})
                    for (const auto& [id, level] : data.enchantments_of(input->components, false)) {
                        if (!std::ranges::binary_search(data.curses, id)) continue;
                        const auto it = std::ranges::find(curses, id, &std::pair<std::uint32_t, std::int32_t>::first);
                        if (it == curses.end()) curses.emplace_back(id, level);
                        else it->second = std::max(it->second, level);
                    }
                if (!curses.empty()) {
                    const auto patch = data.with_enchantments(contracts::ComponentPatch{}, false, curses, false, 0);
                    if (!patch) break;
                    made.components = *patch;
                }
            }
            return SpecialMade{special.kind, special.id, made, std::nullopt};
        }
        case Kind::armor_dye: {
            if (!data.dyed_color_of || !data.with_dyed_color) break;
            const ItemStack* item = nullptr;
            std::vector<std::uint32_t> dyes;
            bool other = false;
            for (const auto k : used) {
                if (const auto dye = color_of(grid[k].item, "_dye")) dyes.push_back(*dye);
                else if (std::ranges::binary_search(data.dyeable, grid[k].item) && !item) item = &grid[k];
                else other = true;
            }
            if (!item || dyes.empty() || other) break;
            const auto patch = data.with_dyed_color(item->components, mixed(data.dyed_color_of(item->components), dyes));
            if (!patch) break;
            auto made = *item;
            made.count = 1;
            made.components = *patch;
            return SpecialMade{special.kind, special.id, made, std::nullopt};
        }
        case Kind::banner_duplicate: {
            if (used.size() != 2 || !data.banner_layers) break;
            const auto& a = grid[used[0]];
            const auto& b = grid[used[1]];
            if (a.item != b.item || !color_of(a.item, "_banner")) break;
            const bool a_patterned = data.banner_layers(a.components) > 0, b_patterned = data.banner_layers(b.components) > 0;
            if (a_patterned == b_patterned) break;
            auto made = a_patterned ? a : b;
            made.count = 1;
            return SpecialMade{special.kind, special.id, made, a_patterned ? used[0] : used[1]};
        }
        case Kind::shield_decoration: {
            if (used.size() != 2 || !data.banner_layers || !data.with_component_from || !data.with_int_component) break;
            const ItemStack* shield = nullptr;
            const ItemStack* banner = nullptr;
            for (const auto k : used) {
                if (grid[k].item == items::shield) shield = &grid[k];
                else if (color_of(grid[k].item, "_banner")) banner = &grid[k];
            }
            // A shield with patterns already is refused; one with a base color alone takes the banner's.
            if (!shield || !banner || data.banner_layers(shield->components) > 0) break;
            auto patch = data.with_component_from(shield->components, banner->components, "banner_patterns");
            if (patch) patch = data.with_int_component(*patch, "base_color", static_cast<std::int32_t>(*color_of(banner->item, "_banner")));
            if (!patch) break;
            auto made = *shield;
            made.count = 1;
            made.components = *patch;
            return SpecialMade{special.kind, special.id, made, std::nullopt};
        }
        case Kind::firework_star: {
            if (!data.firework_star) break;
            int gunpowder = 0, shapes = 0, diamonds = 0, glowstone = 0;
            std::int32_t shape = 0;
            std::vector<std::int32_t> dyes;
            bool other = false;
            for (const auto k : used) {
                const auto item = grid[k].item;
                if (const auto dye = color_of(item, "_dye")) dyes.push_back(firework_rgb[*dye]);
                else if (const auto s = shape_of(item)) shape = *s, ++shapes;
                else if (item == items::gunpowder) ++gunpowder;
                else if (item == items::diamond) ++diamonds;
                else if (item == items::glowstone_dust) ++glowstone;
                else other = true;
            }
            if (other || gunpowder != 1 || dyes.empty() || shapes > 1 || diamonds > 1 || glowstone > 1) break;
            const auto patch = data.firework_star(shape, dyes, diamonds == 1, glowstone == 1);
            if (!patch) break;
            return SpecialMade{special.kind, special.id, ItemStack{items::firework_star, 1, 0, *patch}, std::nullopt};
        }
        case Kind::firework_rocket: {
            if (!data.firework_rocket) break;
            int paper = 0, gunpowder = 0;
            std::vector<contracts::ComponentPatch> stars;
            bool other = false;
            for (const auto k : used) {
                const auto item = grid[k].item;
                if (item == items::paper) ++paper;
                else if (item == items::gunpowder) ++gunpowder;
                else if (item == items::firework_star) stars.push_back(grid[k].components);
                else other = true;
            }
            if (other || paper != 1 || gunpowder < 1 || gunpowder > 3) break;
            const auto patch = data.firework_rocket(gunpowder, stars);
            if (!patch) break;
            ItemStack made{items::firework_rocket, 3};
            // One gunpowder and no explosions is the rocket's default: no components.
            if (gunpowder != 1 || patch != data.firework_rocket(1, {})) made.components = *patch;
            return SpecialMade{special.kind, special.id, made, std::nullopt};
        }
        case Kind::firework_star_fade: {
            if (!data.with_fade_colors) break;
            const ItemStack* star = nullptr;
            std::vector<std::int32_t> dyes;
            bool other = false;
            for (const auto k : used) {
                if (const auto dye = color_of(grid[k].item, "_dye")) dyes.push_back(firework_rgb[*dye]);
                else if (grid[k].item == items::firework_star && !star) star = &grid[k];
                else other = true;
            }
            if (other || !star || dyes.empty()) break;
            const auto patch = data.with_fade_colors(star->components, dyes);
            if (!patch) break;
            auto made = *star;
            made.count = 1;
            made.components = *patch;
            return SpecialMade{special.kind, special.id, made, std::nullopt};
        }
        }
    }
    return std::nullopt;
}

} // namespace circuit::gameplay::detail

namespace circuit::gameplay {
namespace detail {
GridRecipe grid_recipe(const GameData& data, const recipes::Grid& grid, std::int32_t placed) {
    if (placed >= 0 && static_cast<std::size_t>(placed) < data.crafting.size())
        if (auto made = recipes::match(std::span(&data.crafting[static_cast<std::size_t>(placed)], 1), grid)) return {std::move(made), std::nullopt};
    auto made = recipes::match(data.crafting, grid);
    auto special = special_crafting(data, grid.slots);
    if (made && special) {
        // Both lists are in id order; the earlier id makes it.
        if (recipes::id_before(special->id, made->recipe->id)) made.reset();
        else special.reset();
    }
    return {std::move(made), std::move(special)};
}
} // namespace detail

contracts::ItemStack crafted(const GameData& data, const recipes::Grid& grid) {
    const auto r = detail::grid_recipe(data, grid);
    return r.made ? r.made->result : r.special ? r.special->result : contracts::ItemStack{};
}
} // namespace circuit::gameplay
