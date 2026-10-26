// SPDX-License-Identifier: AGPL-3.0-only
// Enchanting tables: the offers a table makes for an item from the
// bookshelves around it and the player's enchantment seed, and enchanting by
// one of them (minecraft.wiki "Enchanting mechanics", "Enchanting table";
// the seeding of each draw fitted to both official servers' windows through
// a protocol client).
#include "internal.hpp"
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/experience.hpp>
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/level_random.hpp>
#include <circuit/gameplay/physics.hpp>
#include <circuit/gameplay/rng.hpp>
#include <algorithm>
#include <climits>
#include <cmath>

namespace circuit::gameplay::detail {
namespace {

using Levels = std::vector<std::pair<std::uint32_t, std::int32_t>>;

// An item's enchantability (minecraft.wiki "Enchanting mechanics"): by its
// material for armor, tools and weapons; 1 for books, bows, crossbows,
// fishing rods and tridents; 0 for the items a table does not enchant.
std::int32_t enchantability(std::uint16_t item) {
    const auto* info = items::info(item);
    if (!info) return 0;
    std::string_view n = info->name;
    n.remove_prefix(n.find(':') + 1);
    if (n == "book" || n == "bow" || n == "crossbow" || n == "fishing_rod" || n == "trident") return 1;
    if (n == "mace") return 15;
    if (n == "turtle_helmet") return 9;
    const bool armor = n.ends_with("_helmet") || n.ends_with("_chestplate") || n.ends_with("_leggings") || n.ends_with("_boots");
    const bool tool = n.ends_with("_sword") || n.ends_with("_pickaxe") || n.ends_with("_axe") || n.ends_with("_shovel") || n.ends_with("_hoe");
    if (!armor && !tool) return 0;
    struct Material {
        std::string_view prefix;
        std::int32_t armor, tool;
    };
    static constexpr Material materials[]{{"leather_", 15, 0}, {"chainmail_", 12, 0}, {"iron_", 9, 14}, {"golden_", 25, 22},
                                          {"diamond_", 10, 10}, {"netherite_", 15, 15}, {"wooden_", 0, 15}, {"stone_", 0, 5}};
    for (const auto& m : materials)
        if (n.starts_with(m.prefix)) return armor ? m.armor : m.tool;
    return 0;
}

bool in_tag(const std::vector<std::uint32_t>& blocks, BlockState s) {
    const auto state = physics::registry_state_of(s);
    const auto block = state ? registry::block_of(*state) : std::nullopt;
    return block && std::ranges::binary_search(blocks, *block);
}

// The bookshelves that power a table: those two blocks away on one axis
// (and up to two on the other), level with it or one above, with a block
// that lets the power through halfway toward the table (each offset halved
// toward zero).
std::int32_t bookshelves(RuleContext& ctx, const GameData& data, BlockPos table) {
    std::int32_t n = 0;
    for (std::int32_t dy = 0; dy <= 1; ++dy)
        for (std::int32_t dx = -2; dx <= 2; ++dx)
            for (std::int32_t dz = -2; dz <= 2; ++dz) {
                if (std::abs(dx) != 2 && std::abs(dz) != 2) continue;
                if (in_tag(data.power_providers, ctx.block({table.x + dx, table.y + dy, table.z + dz}))
                    && in_tag(data.power_transmitters, ctx.block({table.x + dx / 2, table.y + dy, table.z + dz / 2})))
                    ++n;
            }
    return n;
}

// An offer's level cost: 1 to 8, half the bookshelves (at most 15) and
// up to all of them more; a third of that (at least 1) for the first offer,
// two thirds and one more for the second, and at least twice the
// bookshelves for the third.
std::int32_t offer_cost(LevelRandom& r, int slot, std::int32_t shelves) {
    const auto b = std::min(shelves, 15);
    const auto base = r.next_int(8) + 1 + (b >> 1) + r.next_int(b + 1);
    if (slot == 0) return std::max(base / 3, 1);
    if (slot == 1) return base * 2 / 3 + 1;
    return std::max(base, b * 2);
}

bool excludes(const GameData& data, std::uint32_t a, std::uint32_t b) {
    const auto lists = [&](std::uint32_t x, std::uint32_t y) {
        const auto it = data.enchantment_rules.find(x);
        return it != data.enchantment_rules.end() && std::ranges::binary_search(it->second.exclusive, y);
    };
    return lists(a, b) || lists(b, a);
}

// The enchantments an offer of `cost` gives: the cost raised by up to
// twice a quarter of the enchantability and varied by up to 15% (in
// single precision); of the table's enchantments the item is a primary
// item for (any for a book), each at its highest level whose cost range
// holds it; one picked by weight, then more while a draw below 50 is not
// above the cost, each without those that conflict with the last, the cost
// halving each time.
Levels selected(const GameData& data, LevelRandom& r, const ItemStack& item, std::int32_t ench, std::int32_t cost) {
    Levels out;
    if (ench <= 0) return out;
    cost += 1 + r.next_int(ench / 4 + 1) + r.next_int(ench / 4 + 1);
    const float spread = (r.next_float() + r.next_float() - 1.0f) * 0.15f;
    const float varied = static_cast<float>(cost) + static_cast<float>(cost) * spread;
    cost = static_cast<std::int32_t>(std::clamp(std::floor(varied + 0.5f), 1.0f, 2147483520.0f));
    struct Candidate {
        std::uint32_t id;
        std::int32_t level, weight;
    };
    std::vector<Candidate> available;
    for (const auto& e : data.table_enchantments) {
        if (item.item != items::book && !std::ranges::binary_search(e.primary, item.item)) continue;
        for (auto level = e.max_level; level >= 1; --level)
            if (cost >= e.min_cost.first + e.min_cost.second * (level - 1) && cost <= e.max_cost.first + e.max_cost.second * (level - 1)) {
                available.push_back({e.id, level, e.weight});
                break;
            }
    }
    const auto pick = [&] {
        std::int32_t total = 0;
        for (const auto& c : available) total += c.weight;
        if (total <= 0) return;
        auto at = r.next_int(total);
        for (const auto& c : available)
            if ((at -= c.weight) < 0) {
                out.emplace_back(c.id, c.level);
                return;
            }
    };
    if (available.empty()) return out;
    pick();
    while (r.next_int(50) <= cost) {
        if (!out.empty()) {
            const auto last = out.back().first;
            std::erase_if(available, [&](const Candidate& c) { return c.id == last || excludes(data, c.id, last); });
        }
        if (available.empty()) break;
        pick();
        cost /= 2;
    }
    return out;
}

// What an offer gives an item, drawn from the seed plus the offer's place:
// its enchantments, of which a book keeps all but one drawn at random.
Levels offered(const GameData& data, LevelRandom& r, const ItemStack& item, std::int32_t seed, int slot, std::int32_t cost) {
    r.set_seed(static_cast<std::int32_t>(static_cast<std::uint32_t>(seed) + static_cast<std::uint32_t>(slot)));
    auto list = selected(data, r, item, enchantability(item.item), cost);
    if (item.item == items::book && list.size() > 1) list.erase(list.begin() + r.next_int(static_cast<std::int32_t>(list.size())));
    return list;
}

bool enchantable(const GameData& data, const ItemStack& s) {
    return !s.empty() && enchantability(s.item) > 0 && (!data.enchantments_of || data.enchantments_of(s.components, false).empty());
}

} // namespace

void set_enchant_offers(RuleContext& ctx, PlayerInventory& inv, BlockPos table) {
    const auto* data = ctx.data();
    std::array<std::int32_t, 3> costs{}, clues{-1, -1, -1}, levels{-1, -1, -1};
    if (data && enchantable(*data, inv.enchant[0])) {
        const auto seed = experience::enchantment_seed(inv);
        const auto shelves = bookshelves(ctx, *data, table);
        LevelRandom r(seed);
        for (int k = 0; k < 3; ++k) {
            costs[k] = offer_cost(r, k, shelves);
            if (costs[k] < k + 1) costs[k] = 0;
        }
        // Each offer shows one of its enchantments, drawn after them.
        for (int k = 0; k < 3; ++k) {
            if (costs[k] <= 0) continue;
            const auto list = offered(*data, r, inv.enchant[0], seed, k, costs[k]);
            if (list.empty()) continue;
            const auto& [id, level] = list[static_cast<std::size_t>(r.next_int(static_cast<std::int32_t>(list.size())))];
            clues[k] = static_cast<std::int32_t>(id);
            levels[k] = level;
        }
    }
    inv.enchant_costs = costs;
    inv.enchant_clues = clues;
    inv.enchant_clue_levels = levels;
}

void enchant_by_offer(RuleContext& ctx, PlayerInventory& inv, BlockPos table, std::int32_t offer, bool creative) {
    const auto* data = ctx.data();
    if (!data || offer < 0 || offer > 2 || !data->with_enchantments || !data->repair_cost_of) return;
    const auto price = offer + 1;
    const auto& item = inv.enchant[0];
    const auto& lapis = inv.enchant[1];
    // Its place in lapis and in levels (and at least its cost in levels),
    // outside creative mode.
    if ((lapis.empty() || lapis.count < price) && !creative) return;
    const auto cost = inv.enchant_costs[static_cast<std::size_t>(offer)];
    if (cost <= 0 || item.empty() || ((inv.xp_level < price || inv.xp_level < cost) && !creative)) return;
    LevelRandom r;
    const auto list = offered(*data, r, item, experience::enchantment_seed(inv), offer, cost);
    if (list.empty()) return;
    auto made = item;
    const bool book = item.item == items::book;
    const auto patch = data->with_enchantments(made.components, book, list, false, data->repair_cost_of(made.components));
    if (!patch) return;
    made.components = *patch;
    if (book) made.item = items::enchanted_book;
    inv.enchant[0] = made;
    record_slot(ctx, inv, contracts::enchant_first);
    // The levels it took, and a new seed (one of Circuit's own; the official
    // servers draw it at random).
    const auto before = inv.xp_total;
    experience::give_levels(inv, -price);
    if (inv.xp_total != before)
        ctx.record(TraceKind::player_experience_changed, table, inv.player.hi, inv.player.lo,
                   static_cast<std::uint64_t>(static_cast<std::uint32_t>(inv.xp_total)));
    inv.xp_seed = static_cast<std::int32_t>(static_cast<std::uint32_t>(
        Rng::derive({static_cast<std::uint64_t>(static_cast<std::uint32_t>(experience::enchantment_seed(inv))), ctx.now(), inv.player.hi, inv.player.lo})
            .next()));
    if (!creative) {
        auto& used = inv.enchant[1];
        used.count = static_cast<std::uint8_t>(used.count - price);
        if (used.count == 0) used = {};
        record_slot(ctx, inv, contracts::enchant_first + 1);
    }
    set_enchant_offers(ctx, inv, table);
}

} // namespace circuit::gameplay::detail
