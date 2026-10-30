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
// average, in integer channels. The dyes' colors are minecraft.wiki's.
namespace circuit::gameplay::detail {
namespace {
constexpr std::array<std::string_view, 16> colors{"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                                  "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};
constexpr std::array<std::uint32_t, 16> dye_rgb{0xF9FFFE, 0xF9801D, 0xC74EBD, 0x3AB3DA, 0xFED83D, 0x80C71F, 0xF38BAA, 0x474F52,
                                                0x9D9D97, 0x169C9C, 0x8932B8, 0x3C44AA, 0x835432, 0x5E7C16, 0xB02E26, 0x1D1D21};

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
            return SpecialMade{special.kind, made, std::nullopt};
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
            return SpecialMade{special.kind, made, std::nullopt};
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
            return SpecialMade{special.kind, made, a_patterned ? used[0] : used[1]};
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
            return SpecialMade{special.kind, made, std::nullopt};
        }
        }
    }
    return std::nullopt;
}

} // namespace circuit::gameplay::detail

namespace circuit::gameplay {
contracts::ItemStack crafted(const GameData& data, const recipes::Grid& grid) {
    if (const auto made = recipes::match(data.crafting, grid)) return made->result;
    const auto special = detail::special_crafting(data, grid.slots);
    return special ? special->result : contracts::ItemStack{};
}
} // namespace circuit::gameplay
