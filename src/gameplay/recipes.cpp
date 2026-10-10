// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/gameplay/items.hpp>
#include <circuit/gameplay/recipes.hpp>
#include <algorithm>
#include <array>

namespace circuit::gameplay::recipes {
namespace {
using contracts::ItemStack;

// The non-empty part of a grid.
struct Box {
    std::uint8_t x{}, y{}, width{}, height{};
    std::size_t count{}; // non-empty slots
};
Box bounds(const Grid& g) {
    int x0 = g.width, y0 = g.height, x1 = -1, y1 = -1;
    Box b;
    for (int y = 0; y < g.height; ++y)
        for (int x = 0; x < g.width; ++x) {
            if (g.slots[static_cast<std::size_t>(y * g.width + x)].empty()) continue;
            ++b.count;
            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        }
    if (b.count == 0) return b;
    b.x = static_cast<std::uint8_t>(x0), b.y = static_cast<std::uint8_t>(y0);
    b.width = static_cast<std::uint8_t>(x1 - x0 + 1), b.height = static_cast<std::uint8_t>(y1 - y0 + 1);
    return b;
}

bool shaped(const Recipe& r, const Grid& g, const Box& b, bool mirrored) {
    if (b.width != r.width || b.height != r.height) return false;
    for (int y = 0; y < r.height; ++y)
        for (int x = 0; x < r.width; ++x) {
            const auto& ing = r.ingredients[static_cast<std::size_t>(y * r.width + (mirrored ? r.width - 1 - x : x))];
            const auto& s = g.slots[static_cast<std::size_t>((b.y + y) * g.width + b.x + x)];
            if (ing.empty() ? !s.empty() : s.empty() || !ing.test(s.item)) return false;
        }
    return true;
}

// Whether every item takes a different ingredient (a perfect matching, by
// augmenting paths; at most nine of each).
bool shapeless(const Recipe& r, const std::vector<std::uint16_t>& items) {
    if (items.size() != r.ingredients.size()) return false;
    const auto n = items.size();
    std::array<int, 9> owner{};
    owner.fill(-1);
    std::array<bool, 9> seen{};
    const auto assign = [&](auto&& self, std::size_t item) -> bool {
        for (std::size_t k = 0; k < n; ++k) {
            if (seen[k] || !r.ingredients[k].test(items[item])) continue;
            seen[k] = true;
            if (owner[k] < 0 || self(self, static_cast<std::size_t>(owner[k]))) {
                owner[k] = static_cast<int>(item);
                return true;
            }
        }
        return false;
    };
    for (std::size_t i = 0; i < n; ++i) {
        seen.fill(false);
        if (!assign(assign, i)) return false;
    }
    return true;
}
} // namespace

bool Ingredient::test(std::uint16_t item) const noexcept { return std::ranges::binary_search(items, item); }

std::optional<Match> match(std::span<const Recipe> recipes, const Grid& grid) {
    const auto box = bounds(grid);
    if (box.count == 0 || box.count > 9) return std::nullopt;
    std::vector<const ItemStack*> present;
    std::vector<std::uint16_t> items;
    for (const auto& s : grid.slots)
        if (!s.empty()) present.push_back(&s), items.push_back(s.item);
    for (const auto& r : recipes) {
        switch (r.kind) {
        case Recipe::Kind::shaped:
            if (shaped(r, grid, box, false) || shaped(r, grid, box, true)) return Match{&r, r.result};
            break;
        case Recipe::Kind::shapeless:
            if (shapeless(r, items)) return Match{&r, r.result};
            break;
        case Recipe::Kind::transmute: {
            // Two items: the first that is an input, and a material. An
            // input that already is the result does not match (as the
            // official servers show: a red shulker box and red dye make nothing).
            if (present.size() != 2 || r.ingredients.size() != 2) break;
            const bool first = r.ingredients[0].test(present[0]->item);
            const auto* input = first ? present[0] : present[1];
            const auto* material = first ? present[1] : present[0];
            if (!r.ingredients[0].test(input->item) || !r.ingredients[1].test(material->item)) break;
            if (input->item == r.result.item) break;
            auto result = *input;
            result.item = r.result.item;
            result.count = r.result.count;
            return Match{&r, result};
        }
        }
    }
    return std::nullopt;
}

ItemStack remainder(std::uint16_t item) {
    static const auto bucket = items::by_name("minecraft:bucket").value_or(0);
    static const auto bottle = items::by_name("minecraft:glass_bottle").value_or(0);
    static const std::array<std::pair<std::uint16_t, std::uint16_t>, 5> table{{
        {items::by_name("minecraft:milk_bucket").value_or(0), bucket},
        {items::by_name("minecraft:water_bucket").value_or(0), bucket},
        {items::by_name("minecraft:lava_bucket").value_or(0), bucket},
        {items::by_name("minecraft:honey_bottle").value_or(0), bottle},
        {items::by_name("minecraft:dragon_breath").value_or(0), bottle},
    }};
    for (const auto& [from, to] : table)
        if (from != 0 && from == item) return {to, 1};
    return {};
}

} // namespace circuit::gameplay::recipes
