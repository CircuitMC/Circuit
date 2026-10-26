// SPDX-License-Identifier: AGPL-3.0-only
// Container menus: a player's own window and the windows of chests, double
// chests, hoppers, furnaces, crafters, crafting tables, stonecutters,
// smithing tables, grindstones and looms, with every click mode of the protocol,
// applied as the target version's menus apply them (documented in the
// protocol pages and compared with the official server through a protocol
// client).
#include "internal.hpp"
#include <circuit/gameplay/game_data.hpp>
#include <algorithm>
#include <array>

namespace circuit::gameplay::detail {
namespace {
using contracts::player_slots;
constexpr std::int16_t outside = -999;

// The armor slot of the own window an item may be worn in, or 0.
std::uint8_t armor_slot(std::uint16_t item) {
    const auto* info = items::info(item);
    if (!info) return 0;
    const std::string_view n = info->name;
    if (n.ends_with("_helmet") || n == "minecraft:carved_pumpkin" || n.ends_with("_head") || n.ends_with("_skull")) return 5;
    if (n.ends_with("_chestplate") || n == "minecraft:elytra") return 6;
    if (n.ends_with("_leggings")) return 7;
    if (n.ends_with("_boots")) return 8;
    return 0;
}
bool offhand_item(std::uint16_t item) { return item == items::shield; }

// A player slot by its slot-record number: the inventory, or the open
// crafting table's result and grid.
ItemStack& player_slot(PlayerInventory& inv, std::uint8_t index) {
    if (index >= contracts::loom_first) return inv.loom[index - contracts::loom_first];
    if (index >= contracts::grind_first) return inv.grind[index - contracts::grind_first];
    if (index >= contracts::smith_first) return inv.smith[index - contracts::smith_first];
    if (index >= contracts::cutter_first) return inv.cutter[index - contracts::cutter_first];
    return index >= contracts::table_first ? inv.table[index - contracts::table_first] : inv.slots[index];
}

// A stonecutter's result: the chosen recipe's, among those that cut its
// input, while there is input.
void set_cutter_result(RuleContext& ctx, PlayerInventory& inv) {
    ItemStack result;
    const auto* data = ctx.data();
    if (data && inv.cutter_recipe >= 0 && !inv.cutter[0].empty()) {
        const auto list = recipes::cuttings(data->stonecutting, inv.cutter[0].item);
        if (static_cast<std::size_t>(inv.cutter_recipe) < list.size()) result = list[static_cast<std::size_t>(inv.cutter_recipe)]->result;
    }
    if (inv.cutter[1] == result) return;
    inv.cutter[1] = result;
    record_slot(ctx, inv, contracts::cutter_first + 1);
}
// The stonecutter's input changed from `before`: another item, or none,
// clears the chosen recipe (a new count of the same item keeps it).
void cutter_input_changed(RuleContext& ctx, PlayerInventory& inv, const ItemStack& before) {
    if (inv.cutter[0].empty() || inv.cutter[0].item != before.item) inv.cutter_recipe = -1;
    set_cutter_result(ctx, inv);
}
// What a smithing table makes of a template, a base and an addition: the
// first recipe (in id order) that takes them decides. A transform gives
// its result the base's damage and components; a trim gives the base the
// addition's material in the recipe's pattern (the template item's before
// 1.21.5), and nothing when it already has that trim.
ItemStack smithed(const GameData& data, const ItemStack& template_item, const ItemStack& base, const ItemStack& addition) {
    const auto fits = [](const std::optional<recipes::Ingredient>& i, const ItemStack& s) {
        return i ? !s.empty() && i->test(s.item) : s.empty();
    };
    for (const auto& r : data.smithing) {
        if (!fits(r.template_item, template_item) || base.empty() || !r.base.test(base.item) || !fits(r.addition, addition)) continue;
        auto made = base;
        if (r.kind == recipes::Smithing::Kind::transform) {
            made.item = r.result.item;
            made.count = r.result.count;
            return made;
        }
        const auto material = data.trim_materials.find(addition.item);
        auto pattern = r.pattern;
        if (const auto it = data.trim_patterns.find(template_item.item); pattern.empty() && it != data.trim_patterns.end()) pattern = it->second;
        if (material == data.trim_materials.end() || pattern.empty() || !data.trimmed) return {};
        const auto patch = data.trimmed(base.components, material->second, pattern);
        if (!patch || *patch == base.components) return {};
        made.count = 1;
        made.components = *patch;
        return made;
    }
    return {};
}
void set_smith_result(RuleContext& ctx, PlayerInventory& inv) {
    const auto* data = ctx.data();
    const auto result = data ? smithed(*data, inv.smith[0], inv.smith[1], inv.smith[2]) : ItemStack{};
    if (inv.smith[3] == result) return;
    inv.smith[3] = result;
    record_slot(ctx, inv, contracts::smith_first + 3);
}
// Whether a smithing table's template (0), base (1) or addition (2) slot
// takes an item: those some recipe takes there.
bool smith_takes(const RuleContext& ctx, std::size_t slot, std::uint16_t item) {
    const auto* data = ctx.data();
    if (!data || slot > 2) return false;
    const auto& items = slot == 0 ? data->smithing_template : slot == 1 ? data->smithing_base : data->smithing_addition;
    return std::ranges::binary_search(items, item);
}

// Whether a grindstone's input takes an item: one that wears, or holds
// enchantments.
bool grinds(const RuleContext& ctx, const ItemStack& s) {
    if (items::max_damage(s) > 0) return true;
    const auto* data = ctx.data();
    return data && data->enchantments_of
        && (!data->enchantments_of(s.components, false).empty() || !data->enchantments_of(s.components, true).empty());
}
// What a grindstone makes (both official servers): one input with
// enchantments loses all but its curses (an enchanted book left with none
// becomes a book); two inputs of one item that wears make one with both
// durabilities and 5% of the most, and the curses of both. The repair cost
// counts the curses kept, doubled and one more for each.
ItemStack ground(const GameData& data, const ItemStack& a, const ItemStack& b) {
    if (!data.enchantments_of || !data.with_enchantments) return {};
    const auto any = [&](const ItemStack& s) {
        return !data.enchantments_of(s.components, false).empty() || !data.enchantments_of(s.components, true).empty();
    };
    ItemStack made;
    std::vector<const ItemStack*> inputs;
    if (!a.empty() && !b.empty()) {
        const int most = items::max_damage(a);
        if (a.item != b.item || most <= 0 || items::max_stack(a) > 1) return {};
        made = a;
        made.count = 1;
        made.damage = static_cast<std::uint16_t>(std::max(most - ((most - a.damage) + (most - b.damage) + most * 5 / 100), 0));
        inputs = {&a, &b};
    } else {
        const auto& only = a.empty() ? b : a;
        if (only.empty() || !any(only)) return {};
        made = only;
        inputs = {&only};
    }
    std::array<std::vector<std::pair<std::uint32_t, std::int32_t>>, 2> curses; // enchantments, stored ones
    for (const auto* input : inputs)
        for (const bool stored : {false, true})
            for (const auto& [id, level] : data.enchantments_of(input->components, stored)) {
                if (!std::ranges::binary_search(data.curses, id)) continue;
                auto& kept = curses[stored ? 1 : 0];
                const auto it = std::ranges::find(kept, id, &std::pair<std::uint32_t, std::int32_t>::first);
                if (it == kept.end()) kept.emplace_back(id, level);
                else it->second = std::max(it->second, level);
            }
    std::int32_t cost = 0;
    for (std::size_t k = 0; k < curses[0].size() + curses[1].size(); ++k) cost = cost * 2 + 1;
    const bool book = made.item == items::enchanted_book && curses[1].empty();
    auto patch = data.with_enchantments(made.components, false, curses[0], false, cost);
    if (patch) patch = data.with_enchantments(*patch, true, curses[1], book && data.keeps_empty_book_enchantments, cost);
    if (!patch) return {};
    made.components = *patch;
    if (book) made.item = items::book;
    return made;
}
void set_grind_result(RuleContext& ctx, PlayerInventory& inv) {
    const auto* data = ctx.data();
    const auto result = data ? ground(*data, inv.grind[0], inv.grind[1]) : ItemStack{};
    if (inv.grind[2] == result) return;
    inv.grind[2] = result;
    record_slot(ctx, inv, contracts::grind_first + 2);
}

// A loom's slots: banners (0), dyes (1) and banner pattern items (2).
bool loom_takes(std::size_t slot, std::uint16_t item) {
    const auto* info = items::info(item);
    if (!info || slot > 2) return false;
    const std::string_view n = info->name;
    return slot == 0 ? n.ends_with("_banner") : slot == 1 ? n.ends_with("_dye") : n.ends_with("_banner_pattern");
}
// A dye's color, as the protocol numbers them.
std::optional<std::uint32_t> dye_color(std::uint16_t item) {
    static constexpr std::array<std::string_view, 16> colors{"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                                             "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};
    const auto* info = items::info(item);
    if (!info) return std::nullopt;
    for (std::uint32_t k = 0; k < colors.size(); ++k)
        if (info->name == std::string("minecraft:") + std::string(colors[k]) + "_dye") return k;
    return std::nullopt;
}
// The patterns a loom offers for a pattern item: its own, or those that
// need none.
const std::vector<std::string>& pattern_choices(const GameData& data, const ItemStack& pattern_item) {
    static const std::vector<std::string> none;
    if (pattern_item.empty()) return data.loom_patterns;
    const auto it = data.pattern_item_patterns.find(pattern_item.item);
    return it == data.pattern_item_patterns.end() ? none : it->second;
}
// The patterns a loom offers (its buttons): none without a banner and a dye.
const std::vector<std::string>& loom_choices(const GameData& data, const PlayerInventory& inv) {
    static const std::vector<std::string> none;
    return inv.loom[0].empty() || inv.loom[1].empty() ? none : pattern_choices(data, inv.loom[2]);
}
// A loom's result: the banner with a layer of the chosen pattern in the
// dye's color on top, while a banner, a dye and a choice are there.
void set_loom_result(RuleContext& ctx, PlayerInventory& inv) {
    ItemStack result;
    const auto* data = ctx.data();
    if (data && data->with_banner_layer && !inv.loom[0].empty() && !inv.loom[1].empty() && inv.loom_pattern >= 0) {
        const auto& choices = loom_choices(*data, inv);
        const auto color = dye_color(inv.loom[1].item);
        if (static_cast<std::size_t>(inv.loom_pattern) < choices.size() && color)
            if (const auto patch = data->with_banner_layer(inv.loom[0].components, choices[static_cast<std::size_t>(inv.loom_pattern)], *color)) {
                result = inv.loom[0];
                result.count = 1;
                result.components = *patch;
            }
    }
    if (inv.loom[3] == result) return;
    inv.loom[3] = result;
    record_slot(ctx, inv, contracts::loom_first + 3);
}
// A loom's slots changed (its pattern item from `pattern_before`), as both
// official servers follow them: without a banner and a dye nothing is
// chosen; a single pattern on offer is chosen; otherwise the chosen pattern
// stays chosen where the patterns on offer still hold it.
void loom_changed(RuleContext& ctx, PlayerInventory& inv, bool had_both, const ItemStack& pattern_before) {
    const auto* data = ctx.data();
    if (!data) return;
    const auto& now = loom_choices(*data, inv);
    if (now.empty()) {
        inv.loom_pattern = -1;
    } else if (now.size() == 1) {
        inv.loom_pattern = 0;
    } else {
        const auto& before = had_both ? pattern_choices(*data, pattern_before) : std::vector<std::string>{};
        const auto chosen = inv.loom_pattern;
        inv.loom_pattern = -1;
        if (chosen >= 0 && static_cast<std::size_t>(chosen) < before.size())
            if (const auto it = std::ranges::find(now, before[static_cast<std::size_t>(chosen)]); it != now.end())
                inv.loom_pattern = static_cast<std::int16_t>(it - now.begin());
    }
    set_loom_result(ctx, inv);
}

bool cuts(const RuleContext& ctx, std::uint16_t item) {
    const auto* data = ctx.data();
    return data && std::ranges::any_of(data->stonecutting, [&](const auto& r) { return r.ingredient.test(item); });
}

// A window's crafting grid: the own window's 2x2 (slots 0-4) or a crafting
// table's 3x3 (slot records 47-56); the result first.
struct CraftingGrid {
    std::uint8_t first{};
    std::uint8_t width{};
    [[nodiscard]] std::uint8_t record(std::size_t k) const { return static_cast<std::uint8_t>(first + k); }
    [[nodiscard]] std::size_t size() const { return std::size_t{width} * width; }
};
constexpr CraftingGrid own_grid{0, 2};
constexpr CraftingGrid table_grid{contracts::table_first, 3};

// One menu: a list of slots, each a container slot or a player slot.
class Menu {
public:
    Menu(RuleContext& ctx, PlayerInventory& inv, std::vector<BlockPos> parts) : ctx_(ctx), inv_(inv) {
        table_ = parts.size() == 1 && is_crafting_table(ctx.block(parts[0]));
        cutter_ = parts.size() == 1 && is_stonecutter(ctx.block(parts[0]));
        smith_ = parts.size() == 1 && is_smithing_table(ctx.block(parts[0]));
        grind_ = parts.size() == 1 && is_grindstone(ctx.block(parts[0]));
        loom_ = parts.size() == 1 && is_loom(ctx.block(parts[0]));
        if (loom_) {
            for (std::uint8_t i = 0; i < contracts::loom_slots; ++i)
                slots_.push_back({{}, static_cast<std::uint8_t>(contracts::loom_first + i), false});
        } else if (grind_) {
            for (std::uint8_t i = 0; i < contracts::grind_slots; ++i)
                slots_.push_back({{}, static_cast<std::uint8_t>(contracts::grind_first + i), false});
        } else if (smith_) {
            for (std::uint8_t i = 0; i < contracts::smith_slots; ++i)
                slots_.push_back({{}, static_cast<std::uint8_t>(contracts::smith_first + i), false});
        } else if (table_) {
            for (std::uint8_t i = 0; i < contracts::table_slots; ++i)
                slots_.push_back({{}, static_cast<std::uint8_t>(contracts::table_first + i), false});
        } else if (cutter_) {
            for (std::uint8_t i = 0; i < contracts::cutter_slots; ++i)
                slots_.push_back({{}, static_cast<std::uint8_t>(contracts::cutter_first + i), false});
        } else {
            for (const auto p : parts) {
                const auto* be = ctx.peek_block_entity(p);
                for (std::size_t i = 0; be && i < be->slots.size(); ++i) slots_.push_back({p, static_cast<std::uint8_t>(i), true});
            }
        }
        container_size_ = slots_.size();
        own_ = parts.empty();
        furnace_ = parts.size() == 1 && ctx.block(parts[0]).is(BlockType::furnace);
        crafter_ = parts.size() == 1 && ctx.block(parts[0]).is(BlockType::crafter);
        brewing_ = parts.size() == 1 && ctx.block(parts[0]).is(BlockType::brewing_stand);
        if (own_) {
            for (std::uint8_t i = 0; i < player_slots; ++i) slots_.push_back({{}, i, false});
        } else {
            for (std::uint8_t i = 9; i < 36; ++i) slots_.push_back({{}, i, false});
            for (std::uint8_t i = 36; i < 45; ++i) slots_.push_back({{}, i, false});
        }
    }

    [[nodiscard]] std::size_t size() const { return slots_.size(); }
    [[nodiscard]] bool valid(std::int16_t i) const { return i >= 0 && static_cast<std::size_t>(i) < slots_.size(); }
    ItemStack& cursor() { return inv_.cursor; }

    [[nodiscard]] ItemStack get(std::size_t i) const {
        const auto& s = slots_[i];
        if (!s.container) return player_slot(inv_, s.index);
        const auto* be = ctx_.peek_block_entity(s.pos);
        return be ? be->slots[s.index] : ItemStack{};
    }
    void set(std::size_t i, ItemStack stack) {
        if (stack.count == 0) stack = {};
        const auto& s = slots_[i];
        if (!s.container) {
            auto& slot = player_slot(inv_, s.index);
            if (slot == stack) return;
            const auto before = slot;
            slot = stack;
            record_slot(ctx_, inv_, s.index);
            if (s.index == contracts::cutter_first) cutter_input_changed(ctx_, inv_, before);
            if (s.index >= contracts::smith_first && s.index < contracts::smith_first + 3) set_smith_result(ctx_, inv_);
            if (s.index >= contracts::grind_first && s.index < contracts::grind_first + 2) set_grind_result(ctx_, inv_);
            if (s.index >= contracts::loom_first && s.index < contracts::loom_first + 3) {
                const auto banner = s.index == contracts::loom_first ? before : inv_.loom[0];
                const auto dye = s.index == contracts::loom_first + 1 ? before : inv_.loom[1];
                loom_changed(ctx_, inv_, !banner.empty() && !dye.empty(), s.index == contracts::loom_first + 2 ? before : inv_.loom[2]);
            }
            return;
        }
        auto* be = ctx_.block_entity(s.pos);
        if (!be || be->slots[s.index] == stack) return;
        const auto before = be->slots[s.index];
        be->slots[s.index] = stack;
        ctx_.record(TraceKind::inventory_changed, s.pos, s.index, contracts::pack_stack(before), contracts::pack_stack(stack));
        if (furnace_ && s.index == 0) furnace_input_set(ctx_, s.pos, before);
        for (const auto part : container_parts(ctx_, s.pos)) container_changed(ctx_, part);
    }
    void set_cursor(ItemStack stack) {
        if (stack.count == 0) stack = {};
        if (inv_.cursor == stack) return;
        inv_.cursor = stack;
        record_slot(ctx_, inv_, player_slots);
    }

    // The crafting result of the own window or a crafting table: taking
    // from it crafts (see click_result); nothing is placed in it.
    [[nodiscard]] bool result(std::size_t i) const {
        return ((own_ || table_) && i == 0) || (cutter_ && i == 1) || ((smith_ || loom_) && i == 3) || (grind_ && i == 2);
    }
    [[nodiscard]] std::size_t result_slot() const { return smith_ || loom_ ? 3 : grind_ ? 2 : cutter_ ? 1 : 0; }
    [[nodiscard]] bool may_place(std::size_t i, ItemStack stack) const {
        if (result(i)) return false;
        if (own_ && i >= 5 && i <= 8) return armor_slot(stack.item) == i;
        // A furnace's result slot takes nothing; its fuel slot fuels and buckets.
        if (furnace_ && i == 2) return false;
        if (furnace_ && i == 1) return burn_time(stack.item) > 0 || stack.item == items::bucket;
        // A brewing stand's bottle slots take bottles, its ingredient slot
        // ingredients, its fuel slot fuel.
        if (brewing_ && i < 5) return brewing_slot_takes(ctx_.data(), i, stack.item);
        if (smith_ && i < 3) return smith_takes(ctx_, i, stack.item);
        if (grind_ && i < 2) return grinds(ctx_, stack);
        if (loom_ && i < 3) return loom_takes(i, stack.item);
        // A crafter's disabled slots take nothing.
        if (crafter_ && i < container_size_) {
            const auto* be = ctx_.peek_block_entity(slots_[i].pos);
            return !be || (be->disabled >> slots_[i].index & 1) == 0;
        }
        return true;
    }
    [[nodiscard]] bool may_pickup(std::size_t i) const { return !result(i); }
    [[nodiscard]] int max_stack(std::size_t i, ItemStack stack) const {
        if (own_ && i >= 5 && i <= 8) return 1;
        if (furnace_ && i == 1 && stack.item == items::bucket) return 1;
        if (brewing_ && i < 3) return 1;
        return std::min<int>(contracts::max_stack_size, items::max_stack(stack));
    }

    // Moves `stack` into slots [from, to): first onto stacks of the same kind,
    // then into the first empty slot, walking backward when `reverse`.
    bool move_into(ItemStack& stack, std::size_t from, std::size_t to, bool reverse) {
        bool moved = false;
        const auto order = [&](std::size_t k) { return reverse ? to - 1 - k : from + k; };
        if (items::max_stack(stack) > 1) {
            for (std::size_t k = 0; k < to - from && !stack.empty(); ++k) {
                const auto i = order(k);
                auto existing = get(i);
                if (existing.empty() || !existing.same_kind(stack)) continue;
                const int limit = max_stack(i, existing);
                const int total = existing.count + stack.count;
                if (total <= limit) {
                    existing.count = static_cast<std::uint8_t>(total);
                    stack = {};
                    set(i, existing);
                    moved = true;
                } else if (existing.count < limit) {
                    stack.count = static_cast<std::uint8_t>(stack.count - (limit - existing.count));
                    existing.count = static_cast<std::uint8_t>(limit);
                    set(i, existing);
                    moved = true;
                }
            }
        }
        if (!stack.empty()) {
            for (std::size_t k = 0; k < to - from; ++k) {
                const auto i = order(k);
                if (!get(i).empty() || !may_place(i, stack)) continue;
                const int n = std::min<int>(stack.count, max_stack(i, stack));
                auto placed = stack;
                placed.count = static_cast<std::uint8_t>(n);
                stack.count = static_cast<std::uint8_t>(stack.count - n);
                if (stack.count == 0) stack = {};
                set(i, placed);
                moved = true;
                break;
            }
        }
        return moved;
    }

    // Shift-click: moves a slot's stack to the other part of the window.
    // Returns the stack before the move when something moved, else empty.
    ItemStack quick_move(std::size_t i) {
        auto stack = get(i);
        if (stack.empty()) return {};
        const auto original = stack;
        bool moved = false;
        if (furnace_) {
            // The result goes to the inventory from the hotbar's end; the
            // input and fuel to the inventory from its start; from the
            // inventory what the furnace cooks goes into its input, a fuel
            // into its fuel slot, the rest between the main inventory and
            // the hotbar.
            const auto furnace = ctx_.block(slots_[0].pos);
            if (i == 2) moved = move_into(stack, 3, 39, true);
            else if (i < 2) moved = move_into(stack, 3, 39, false);
            else if (cooking_time(ctx_, furnace, stack, true) > 0) moved = move_into(stack, 0, 1, false);
            else if (burn_time(stack.item) > 0) moved = move_into(stack, 1, 2, false);
            else if (i < 30) moved = move_into(stack, 30, 39, false);
            else moved = move_into(stack, 3, 30, false);
        } else if (brewing_) {
            // The stand's slots go to the inventory from the hotbar's end;
            // from the inventory fuel goes into the fuel slot (else, as an
            // ingredient, into the ingredient slot), an ingredient into the
            // ingredient slot and a bottle into the first empty bottle slot,
            // or nowhere when they do not take it; the rest between the main
            // inventory and the hotbar.
            const auto* data = ctx_.data();
            if (i < 5) {
                moved = move_into(stack, 5, 41, true);
            } else if (brewing_slot_takes(data, 4, stack.item)) {
                moved = move_into(stack, 4, 5, false);
                if (!moved && brewing_slot_takes(data, 3, stack.item)) moved = move_into(stack, 3, 4, false);
            } else if (brewing_slot_takes(data, 3, stack.item)) {
                moved = move_into(stack, 3, 4, false);
            } else if (brewing_slot_takes(data, 0, stack.item)) {
                moved = move_into(stack, 0, 3, false);
            } else if (i < 32) {
                moved = move_into(stack, 32, 41, false);
            } else {
                moved = move_into(stack, 5, 32, false);
            }
        } else if (loom_) {
            // The banner, dye and pattern item go to the inventory from its
            // start; from the inventory each to its slot, or nowhere when
            // that slot does not take it; the rest between the main
            // inventory and the hotbar.
            const auto to = [&]() -> std::optional<std::size_t> {
                for (std::size_t k = 0; k < 3; ++k)
                    if (loom_takes(k, stack.item)) return k;
                return std::nullopt;
            }();
            if (i < 3) moved = move_into(stack, 4, 40, false);
            else if (to) moved = move_into(stack, *to, *to + 1, false);
            else if (i < 31) moved = move_into(stack, 31, 40, false);
            else moved = move_into(stack, 4, 31, false);
        } else if (grind_) {
            // The inputs go to the inventory from its start; from the
            // inventory an item the grindstone takes goes into the upper
            // input, else the lower, while empty; the rest between the main
            // inventory and the hotbar.
            if (i < 2) moved = move_into(stack, 3, 39, false);
            else if (may_place(0, stack) && get(0).empty()) moved = move_into(stack, 0, 1, false);
            else if (may_place(1, stack) && get(1).empty()) moved = move_into(stack, 1, 2, false);
            else if (i < 30) moved = move_into(stack, 30, 39, false);
            else moved = move_into(stack, 3, 30, false);
        } else if (smith_) {
            // The template, base and addition go to the inventory from its
            // start; from the inventory an item goes into the first of them
            // that takes it while empty or holding its kind with room, the
            // rest between the main inventory and the hotbar.
            std::optional<std::size_t> to;
            for (std::size_t k = 0; k < 3 && i >= 4 && !to; ++k) {
                const auto there = get(k);
                if (may_place(k, stack) && (there.empty() || (there.same_kind(stack) && there.count < max_stack(k, there)))) to = k;
            }
            if (i < 3) moved = move_into(stack, 4, 40, false);
            else if (to) moved = move_into(stack, *to, *to + 1, false);
            else if (i < 31) moved = move_into(stack, 31, 40, false);
            else moved = move_into(stack, 4, 31, false);
        } else if (cutter_) {
            // The input goes to the inventory from its start; from the
            // inventory an item a recipe cuts goes into the input, the rest
            // between the main inventory and the hotbar.
            if (i == 0) moved = move_into(stack, 2, 38, false);
            else if (cuts(ctx_, stack.item)) moved = move_into(stack, 0, 1, false);
            else if (i < 29) moved = move_into(stack, 29, 38, false);
            else moved = move_into(stack, 2, 29, false);
        } else if (table_) {
            // The grid goes to the inventory from its start; from the
            // inventory items go into the grid, else between the main
            // inventory and the hotbar.
            if (i < 10) moved = move_into(stack, 10, 46, false);
            else if (!(moved = move_into(stack, 1, 10, false))) moved = i < 37 ? move_into(stack, 37, 46, false) : move_into(stack, 10, 37, false);
        } else if (!own_) {
            moved = i < container_size_ ? move_into(stack, container_size_, size(), true) : move_into(stack, 0, container_size_, false);
        } else {
            const auto armor = armor_slot(stack.item);
            if (i >= 1 && i <= 8) moved = move_into(stack, 9, 45, false);
            else if (armor != 0 && get(armor).empty()) moved = move_into(stack, armor, armor + 1u, false);
            else if (offhand_item(stack.item) && get(45).empty()) moved = move_into(stack, 45, 46, false);
            else if (i >= 9 && i < 36) moved = move_into(stack, 36, 45, false);
            else if (i >= 36 && i < 45) moved = move_into(stack, 9, 36, false);
            else moved = move_into(stack, 9, 45, false);
        }
        if (!moved) return {};
        set(i, stack);
        if (stack.count == original.count) return {};
        return original;
    }

    // Puts up to `count` of `stack` into slot i; returns what is left.
    ItemStack insert(std::size_t i, ItemStack stack, int count) {
        if (stack.empty() || !may_place(i, stack)) return stack;
        auto existing = get(i);
        const int room = max_stack(i, stack) - (existing.empty() ? 0 : existing.count);
        const int n = std::min({count, static_cast<int>(stack.count), room});
        if (n <= 0) return stack;
        if (existing.empty()) {
            existing = stack;
            existing.count = static_cast<std::uint8_t>(n);
        } else if (existing.same_kind(stack)) {
            existing.count = static_cast<std::uint8_t>(existing.count + n);
        } else {
            return stack;
        }
        set(i, existing);
        stack.count = static_cast<std::uint8_t>(stack.count - n);
        if (stack.count == 0) stack = {};
        return stack;
    }
    // Takes up to `count` from slot i.
    ItemStack take(std::size_t i, int count) {
        if (!may_pickup(i)) return {};
        auto existing = get(i);
        const int n = std::min<int>(count, existing.count);
        if (n <= 0) return {};
        auto taken = existing;
        taken.count = static_cast<std::uint8_t>(n);
        existing.count = static_cast<std::uint8_t>(existing.count - n);
        set(i, existing);
        return taken;
    }

    // Window slot of an own-inventory slot (hotbar 0-8, off hand 40) in this menu.
    [[nodiscard]] std::optional<std::size_t> menu_index_of_player_slot(std::uint8_t player_slot) const {
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (!slots_[i].container && slots_[i].index == player_slot) return i;
        return std::nullopt;
    }
    [[nodiscard]] bool own() const { return own_; }
    [[nodiscard]] bool table() const { return table_; }
    [[nodiscard]] bool cutter() const { return cutter_; }
    [[nodiscard]] bool smith() const { return smith_; }
    [[nodiscard]] bool grind() const { return grind_; }
    [[nodiscard]] bool loom() const { return loom_; }
    [[nodiscard]] CraftingGrid grid() const { return table_ ? table_grid : own_grid; }
    // The window slots of the main inventory and the hotbar.
    [[nodiscard]] std::size_t inventory_first() const { return smith_ || loom_ ? 4 : grind_ ? 3 : cutter_ ? 2 : table_ ? 10 : 9; }
    [[nodiscard]] std::size_t inventory_end() const { return smith_ || loom_ ? 40 : grind_ ? 39 : cutter_ ? 38 : table_ ? 46 : 45; }
    [[nodiscard]] PlayerInventory& inventory() const { return inv_; }

private:
    struct Ref {
        BlockPos pos;
        std::uint8_t index{};
        bool container{};
    };
    RuleContext& ctx_;
    PlayerInventory& inv_;
    std::vector<Ref> slots_;
    std::size_t container_size_{};
    bool own_{};
    bool furnace_{};
    bool crafter_{};
    bool table_{};
    bool cutter_{};
    bool smith_{};
    bool grind_{};
    bool loom_{};
    bool brewing_{};
};

void throw_item(RuleContext& ctx, const contracts::Vec3& position, const contracts::Vec3& velocity, ItemStack stack) {
    if (stack.empty()) return;
    spawn_dropped(ctx, contracts::block_of(position), position, velocity, stack, 40);
}

bool same_kind_fits(ItemStack slot, ItemStack stack, bool size_matters) {
    if (slot.empty()) return true;
    if (!slot.same_kind(stack)) return false;
    return slot.count + (size_matters ? 0 : stack.count) <= items::max_stack(stack);
}

void reset_drag(PlayerInventory& inv) {
    inv.drag_stage = 0;
    inv.drag_type = 0;
    inv.drag_slots.clear();
}

void pickup(RuleContext& ctx, Menu& menu, const contracts::ContainerClick& c, std::int16_t slot, std::uint8_t button);

// The result was taken: one of each grid item is used up. An item with a
// remainder leaves it in its slot; when the slot still holds other items,
// the remainder goes into the inventory, or drops when it does not fit.
void use_grid(RuleContext& ctx, PlayerInventory& inv, CraftingGrid grid, const contracts::ContainerClick& c) {
    for (std::size_t k = 1; k <= grid.size(); ++k) {
        const auto slot = grid.record(k);
        auto s = player_slot(inv, slot);
        if (s.empty()) continue;
        const auto left = recipes::remainder(s.item);
        s.count = static_cast<std::uint8_t>(s.count - 1);
        if (s.count == 0) s = {};
        if (!left.empty()) {
            if (s.empty()) {
                s = left;
            } else if (s.same_kind(left)) {
                s.count = static_cast<std::uint8_t>(s.count + left.count);
            } else {
                player_slot(inv, slot) = s;
                record_slot(ctx, inv, slot);
                throw_item(ctx, c.drop_position, c.drop_velocity, add_to_player(ctx, inv, left, contracts::hotbar_first));
                continue;
            }
        }
        player_slot(inv, slot) = s;
        record_slot(ctx, inv, slot);
    }
    update_crafting(ctx, inv);
}

// A stonecutter's result was taken: the input gives one item, and the
// result is made again while input of the same item is left.
void use_cutter(RuleContext& ctx, PlayerInventory& inv) {
    const auto before = inv.cutter[0];
    auto input = before;
    input.count = static_cast<std::uint8_t>(input.count - 1);
    if (input.count == 0) input = {};
    inv.cutter[0] = input;
    record_slot(ctx, inv, contracts::cutter_first);
    if (inv.cutter[1].count != 0) {
        inv.cutter[1] = {};
        record_slot(ctx, inv, contracts::cutter_first + 1);
    }
    cutter_input_changed(ctx, inv, before);
}

// A smithing table's result was taken: the template, base and addition
// each give one item, and the result follows what is left.
void use_smith(RuleContext& ctx, PlayerInventory& inv) {
    for (std::size_t k = 0; k < 3; ++k) {
        auto& slot = inv.smith[k];
        if (slot.empty()) continue;
        slot.count = static_cast<std::uint8_t>(slot.count - 1);
        if (slot.count == 0) slot = {};
        record_slot(ctx, inv, static_cast<std::uint8_t>(contracts::smith_first + k));
    }
    set_smith_result(ctx, inv);
}

// A grindstone's result was taken: both inputs are used up (the
// experience it gives is not modeled).
void use_grind(RuleContext& ctx, PlayerInventory& inv) {
    for (std::size_t k = 0; k < 2; ++k) {
        if (inv.grind[k].empty()) continue;
        inv.grind[k] = {};
        record_slot(ctx, inv, static_cast<std::uint8_t>(contracts::grind_first + k));
    }
    set_grind_result(ctx, inv);
}

// A loom's result was taken: the banner and the dye each give one item
// (the pattern item stays), and the result follows what is left.
void use_loom(RuleContext& ctx, PlayerInventory& inv) {
    for (std::size_t k = 0; k < 2; ++k) {
        auto& slot = inv.loom[k];
        if (slot.empty()) continue;
        slot.count = static_cast<std::uint8_t>(slot.count - 1);
        if (slot.count == 0) slot = {};
        record_slot(ctx, inv, static_cast<std::uint8_t>(contracts::loom_first + k));
    }
    loom_changed(ctx, inv, true, inv.loom[2]);
}

// Clicks on a crafting, stonecutting or smithing result. Each take crafts
// once: the whole result leaves the slot and the grid or the inputs are used.
void click_result(RuleContext& ctx, Menu& menu, const contracts::ContainerClick& c) {
    auto& inv = menu.inventory();
    const auto made_now = [&] { return menu.get(menu.result_slot()); };
    const auto use = [&] {
        if (menu.smith()) use_smith(ctx, inv);
        else if (menu.grind()) use_grind(ctx, inv);
        else if (menu.loom()) use_loom(ctx, inv);
        else if (menu.cutter()) use_cutter(ctx, inv);
        else use_grid(ctx, inv, menu.grid(), c);
    };
    const auto result = made_now();
    if (result.empty()) return;
    switch (c.mode) {
    case 0: { // onto the cursor, when all of it fits there
        if (c.button > 1) return;
        auto carried = menu.cursor();
        if (carried.empty()) {
            carried = result;
        } else if (carried.same_kind(result) && carried.count + result.count <= items::max_stack(carried)) {
            carried.count = static_cast<std::uint8_t>(carried.count + result.count);
        } else {
            return;
        }
        menu.set_cursor(carried);
        use();
        return;
    }
    case 1: { // into the inventory, hotbar first from its end, as long as the result stays the same item
        if (c.button > 1) return;
        for (auto made = made_now(); !made.empty() && made.item == result.item; made = made_now()) {
            auto stack = made;
            if (!menu.move_into(stack, menu.inventory_first(), menu.inventory_end(), true)) return;
            use();
            // What did not fit drops.
            throw_item(ctx, c.drop_position, c.drop_velocity, stack);
        }
        return;
    }
    case 2: { // into an empty hotbar slot or off hand
        if (!(c.button < 9 || c.button == 40)) return;
        const std::uint8_t slot = c.button == 40 ? contracts::offhand_slot : static_cast<std::uint8_t>(contracts::hotbar_first + c.button);
        if (!inv.slots[slot].empty()) return;
        inv.slots[slot] = result;
        record_slot(ctx, inv, slot);
        use();
        return;
    }
    case 4: { // thrown; with button 1, again while the result stays the same item
        if (!menu.cursor().empty()) return;
        for (auto made = made_now(); !made.empty() && made.item == result.item; made = made_now()) {
            throw_item(ctx, c.drop_position, c.drop_velocity, made);
            use();
            if (c.button != 1) return;
        }
        return;
    }
    default: return;
    }
}
} // namespace

void update_crafting(RuleContext& ctx, PlayerInventory& inv) {
    const auto* data = ctx.data();
    if (!data) return;
    const auto craft = [&](std::span<ItemStack> slots, std::uint8_t width, std::uint8_t first) {
        const auto grid = slots.subspan(1);
        const bool empty = std::ranges::all_of(grid, [](const ItemStack& s) { return s.empty(); });
        const auto made = empty ? std::nullopt : recipes::match(data->crafting, {width, width, grid});
        const auto result = made ? made->result : ItemStack{};
        if (slots[0] == result) return;
        slots[0] = result;
        record_slot(ctx, inv, first);
    };
    craft(std::span(inv.slots).first(5), 2, 0);
    craft(std::span(inv.table), 3, contracts::table_first);
}

contracts::Result<std::vector<BlockPos>> menu_parts(RuleContext& ctx, std::optional<BlockPos> container) {
    if (!container) return std::vector<BlockPos>{};
    const auto s = ctx.block(*container);
    if (!s.is_unloaded() && (is_crafting_table(s) || is_stonecutter(s) || is_smithing_table(s) || is_grindstone(s) || is_loom(s)))
        return std::vector<BlockPos>{*container};
    if (s.is_unloaded() || !is_container(s.type())) return std::unexpected(contracts::ErrorCode::not_a_container);
    return container_parts(ctx, *container);
}

namespace {
void pickup(RuleContext& ctx, Menu& menu, const contracts::ContainerClick& c, std::int16_t slot, std::uint8_t button) {
    const bool primary = button == 0;
    if (slot == outside) {
        auto carried = menu.cursor();
        if (carried.empty()) return;
        if (primary) {
            menu.set_cursor({});
            throw_item(ctx, c.drop_position, c.drop_velocity, carried);
        } else {
            auto one = carried;
            one.count = 1;
            carried.count = static_cast<std::uint8_t>(carried.count - 1);
            menu.set_cursor(carried);
            throw_item(ctx, c.drop_position, c.drop_velocity, one);
        }
        return;
    }
    if (!menu.valid(slot)) return;
    const auto i = static_cast<std::size_t>(slot);
    const auto in_slot = menu.get(i);
    auto carried = menu.cursor();
    if (in_slot.empty()) {
        if (!carried.empty()) menu.set_cursor(menu.insert(i, carried, primary ? carried.count : 1));
        return;
    }
    if (!menu.may_pickup(i)) return;
    if (carried.empty()) {
        menu.set_cursor(menu.take(i, primary ? in_slot.count : (in_slot.count + 1) / 2));
    } else if (menu.may_place(i, carried)) {
        if (in_slot.same_kind(carried)) {
            menu.set_cursor(menu.insert(i, carried, primary ? carried.count : 1));
        } else if (carried.count <= menu.max_stack(i, carried)) {
            menu.set(i, carried);
            menu.set_cursor(in_slot);
        }
    } else if (in_slot.same_kind(carried)) {
        const auto taken = menu.take(i, std::min<int>(in_slot.count, items::max_stack(carried) - carried.count));
        carried.count = static_cast<std::uint8_t>(carried.count + taken.count);
        menu.set_cursor(carried);
    }
}
} // namespace

contracts::Result<std::function<void()>> plan_click(RuleContext& ctx, const contracts::ContainerClick& c) {
    auto parts = menu_parts(ctx, c.container);
    if (!parts) return std::unexpected(parts.error());
    if (c.mode > 6) return std::unexpected(contracts::ErrorCode::invalid_argument);
    return [&ctx, c, parts = std::move(*parts)] {
        auto& inv = ctx.player(c.player);
        Menu menu(ctx, inv, parts);
        if (c.slot != outside && !menu.valid(c.slot) && !(c.mode == 5 && (c.button & 3) != 1)) return;
        // The grid's result follows whatever the click changed.
        struct Recraft {
            RuleContext& ctx;
            PlayerInventory& inv;
            bool crafts;
            ~Recraft() {
                if (crafts) update_crafting(ctx, inv);
            }
        } recraft{ctx, inv, menu.own() || menu.table()};
        // Drags collect slots over several clicks; any other click ends one.
        if (c.mode == 5) {
            const auto previous = inv.drag_stage;
            const std::uint8_t stage = c.button & 3;
            const auto next_stage = stage;
            if (!((previous == 1 && next_stage == 2) || previous == next_stage) || menu.cursor().empty()) {
                reset_drag(inv);
                return;
            }
            if (next_stage == 0) {
                const std::uint8_t type = (c.button >> 2) & 3;
                if (type == 0 || type == 1 || (type == 2 && c.creative)) {
                    inv.drag_stage = 1;
                    inv.drag_type = type;
                    inv.drag_slots.clear();
                } else {
                    reset_drag(inv);
                }
                return;
            }
            if (next_stage == 1) {
                if (!menu.valid(c.slot)) return;
                const auto i = static_cast<std::size_t>(c.slot);
                const auto carried = menu.cursor();
                if (same_kind_fits(menu.get(i), carried, true) && menu.may_place(i, carried)
                    && (inv.drag_type == 2 || carried.count > inv.drag_slots.size()) && !menu.result(i)
                    && std::ranges::find(inv.drag_slots, c.slot) == inv.drag_slots.end())
                    inv.drag_slots.push_back(c.slot);
                return;
            }
            // Stage 2: distribute.
            if (inv.drag_slots.empty()) {
                reset_drag(inv);
                return;
            }
            if (inv.drag_slots.size() == 1) {
                const auto slot = inv.drag_slots.front();
                const auto type = inv.drag_type;
                reset_drag(inv);
                pickup(ctx, menu, c, slot, type);
                return;
            }
            auto stack = menu.cursor();
            int left = stack.count;
            const auto slots = inv.drag_slots;
            for (const auto slot : slots) {
                const auto i = static_cast<std::size_t>(slot);
                const auto carried = menu.cursor();
                if (!same_kind_fits(menu.get(i), carried, true) || !menu.may_place(i, carried)
                    || (inv.drag_type != 2 && carried.count < slots.size()) || menu.result(i))
                    continue;
                const auto existing = menu.get(i);
                const int had = existing.empty() ? 0 : existing.count;
                const int limit = std::min<int>(items::max_stack(stack), menu.max_stack(i, stack));
                const int per = inv.drag_type == 0 ? stack.count / static_cast<int>(slots.size())
                              : inv.drag_type == 1 ? 1
                                                   : items::max_stack(stack);
                const int n = std::min(per + had, limit);
                left -= n - had;
                auto placed = stack;
                placed.count = static_cast<std::uint8_t>(n);
                menu.set(i, placed);
            }
            stack.count = static_cast<std::uint8_t>(std::max(left, 0));
            menu.set_cursor(stack);
            reset_drag(inv);
            return;
        }
        if (inv.drag_stage != 0) {
            reset_drag(inv);
            return;
        }
        if (c.slot >= 0 && menu.result(static_cast<std::size_t>(c.slot)) && c.mode != 3 && c.mode != 6) {
            click_result(ctx, menu, c);
            return;
        }
        switch (c.mode) {
        case 0: // pickup
            if (c.button <= 1) pickup(ctx, menu, c, c.slot, c.button);
            break;
        case 1: { // shift-click
            if (c.button > 1 || c.slot < 0) break;
            const auto i = static_cast<std::size_t>(c.slot);
            if (!menu.may_pickup(i)) break;
            for (auto moved = menu.quick_move(i); !moved.empty() && menu.get(i).same_kind(moved);) moved = menu.quick_move(i);
            break;
        }
        case 2: { // swap with a hotbar slot or the off hand
            if (!(c.button < 9 || c.button == 40) || c.slot < 0) break;
            const auto i = static_cast<std::size_t>(c.slot);
            const std::uint8_t player_slot = c.button == 40 ? contracts::offhand_slot : static_cast<std::uint8_t>(contracts::hotbar_first + c.button);
            const auto hand = inv.slots[player_slot];
            const auto in_slot = menu.get(i);
            const auto set_hand = [&](ItemStack s) {
                if (s.count == 0) s = {};
                if (inv.slots[player_slot] == s) return;
                inv.slots[player_slot] = s;
                record_slot(ctx, inv, player_slot);
            };
            if (hand.empty() && in_slot.empty()) break;
            if (hand.empty()) {
                if (menu.may_pickup(i)) {
                    menu.set(i, {});
                    set_hand(in_slot);
                }
            } else if (in_slot.empty()) {
                if (menu.may_place(i, hand)) {
                    const int limit = menu.max_stack(i, hand);
                    if (hand.count > limit) {
                        auto part = hand;
                        part.count = static_cast<std::uint8_t>(limit);
                        auto rest = hand;
                        rest.count = static_cast<std::uint8_t>(hand.count - limit);
                        set_hand(rest);
                        menu.set(i, part);
                    } else {
                        set_hand({});
                        menu.set(i, hand);
                    }
                }
            } else if (menu.may_pickup(i) && menu.may_place(i, hand)) {
                const int limit = menu.max_stack(i, hand);
                if (hand.count > limit) {
                    auto part = hand;
                    part.count = static_cast<std::uint8_t>(limit);
                    auto rest = hand;
                    rest.count = static_cast<std::uint8_t>(hand.count - limit);
                    set_hand(rest);
                    menu.set(i, part);
                    const auto left = add_to_player(ctx, inv, in_slot, contracts::hotbar_first);
                    throw_item(ctx, c.drop_position, c.drop_velocity, left);
                } else {
                    set_hand(in_slot);
                    menu.set(i, hand);
                }
            }
            break;
        }
        case 3: { // clone (creative)
            if (!c.creative || !menu.cursor().empty() || c.slot < 0) break;
            auto s = menu.get(static_cast<std::size_t>(c.slot));
            if (s.empty()) break;
            s.count = items::max_stack(s);
            menu.set_cursor(s);
            break;
        }
        case 4: { // throw from a slot
            if (!menu.cursor().empty() || c.slot < 0) break;
            const auto i = static_cast<std::size_t>(c.slot);
            const int count = c.button == 0 ? 1 : menu.get(i).count;
            auto thrown = menu.take(i, count);
            throw_item(ctx, c.drop_position, c.drop_velocity, thrown);
            if (c.button == 1)
                while (!thrown.empty() && menu.get(i).same_kind(thrown)) {
                    thrown = menu.take(i, count);
                    throw_item(ctx, c.drop_position, c.drop_velocity, thrown);
                }
            break;
        }
        case 6: { // collect matching items onto the cursor
            if (c.slot < 0) break;
            auto carried = menu.cursor();
            const auto i = static_cast<std::size_t>(c.slot);
            if (carried.empty() || (!menu.get(i).empty() && menu.may_pickup(i))) break;
            const bool forward = c.button == 0;
            for (int pass = 0; pass < 2; ++pass)
                for (std::size_t k = 0; k < menu.size() && carried.count < items::max_stack(carried); ++k) {
                    const auto j = forward ? k : menu.size() - 1 - k;
                    const auto s = menu.get(j);
                    if (s.empty() || !same_kind_fits(s, carried, true) || !menu.may_pickup(j) || menu.result(j)) continue;
                    if (pass == 0 && s.count == items::max_stack(s)) continue;
                    const auto taken = menu.take(j, std::min<int>(s.count, items::max_stack(carried) - carried.count));
                    carried.count = static_cast<std::uint8_t>(carried.count + taken.count);
                }
            menu.set_cursor(carried);
            break;
        }
        default: break;
        }
    };
}

// A stonecutter's recipe button: one of the input's recipes, by its place
// in their list; another number does nothing.
contracts::Result<std::function<void()>> plan_button(RuleContext& ctx, const contracts::ContainerButton& c) {
    auto parts = menu_parts(ctx, c.container);
    if (!parts) return std::unexpected(parts.error());
    const bool cutter = parts->size() == 1 && is_stonecutter(ctx.block(parts->front()));
    const bool loom = parts->size() == 1 && is_loom(ctx.block(parts->front()));
    return [&ctx, c, cutter, loom] {
        const auto* data = ctx.data();
        if (!data) return;
        // A loom's pattern button: one of the patterns it offers (with or
        // without a banner there), another number does nothing.
        if (loom) {
            auto& inv = ctx.player(c.player);
            if (c.button < 0 || static_cast<std::size_t>(c.button) >= loom_choices(*data, inv).size()) return;
            inv.loom_pattern = static_cast<std::int16_t>(c.button);
            set_loom_result(ctx, inv);
            return;
        }
        if (!cutter) return;
        auto& inv = ctx.player(c.player);
        if (inv.cutter[0].empty() || c.button < 0) return;
        if (static_cast<std::size_t>(c.button) >= recipes::cuttings(data->stonecutting, inv.cutter[0].item).size()) return;
        inv.cutter_recipe = static_cast<std::int16_t>(c.button);
        set_cutter_result(ctx, inv);
    };
}

// A crafter's slot toggle: an empty grid slot may be disabled, a disabled
// one enabled; comparators read the change.
contracts::Result<std::function<void()>> plan_slot_enabled(RuleContext& ctx, const contracts::SetSlotEnabled& c) {
    auto parts = menu_parts(ctx, c.container);
    if (!parts) return std::unexpected(parts.error());
    if (parts->size() != 1 || !ctx.block(parts->front()).is(BlockType::crafter) || c.slot >= 9)
        return std::unexpected(contracts::ErrorCode::invalid_argument);
    return [&ctx, c, pos = parts->front()] {
        auto* be = ctx.block_entity(pos);
        if (!be || be->slots.size() != 9) return;
        const auto bit = static_cast<std::uint16_t>(1u << c.slot);
        if (c.enabled == ((be->disabled & bit) == 0)) return;
        if (!c.enabled && !be->slots[c.slot].empty()) return;
        be->disabled = static_cast<std::uint16_t>(c.enabled ? be->disabled & ~bit : be->disabled | bit);
        container_changed(ctx, pos);
    };
}

contracts::Result<std::function<void()>> plan_close(RuleContext& ctx, const contracts::CloseContainer& c) {
    (void)ctx;
    return [&ctx, c] {
        auto& inv = ctx.player(c.player);
        reset_drag(inv);
        // The crafting grids (the own window's, an open crafting table's)
        // and the carried stack go back into the inventory, or drop when the
        // player left.
        const auto put_back = [&](ItemStack s) { return c.drop ? s : add_to_player(ctx, inv, s, contracts::hotbar_first); };
        for (const auto grid : {own_grid, table_grid})
            for (std::size_t k = 1; k <= grid.size(); ++k) {
                auto& slot = player_slot(inv, grid.record(k));
                const auto s = slot;
                if (s.empty()) continue;
                slot = {};
                record_slot(ctx, inv, grid.record(k));
                throw_item(ctx, c.drop_position, c.drop_velocity, put_back(s));
            }
        update_crafting(ctx, inv);
        // An open stonecutter's input and smithing table's inputs too; their
        // results and the stonecutter's recipe clear.
        for (const auto slot : {contracts::cutter_first, contracts::smith_first, static_cast<std::uint8_t>(contracts::smith_first + 1),
                                static_cast<std::uint8_t>(contracts::smith_first + 2), contracts::grind_first,
                                static_cast<std::uint8_t>(contracts::grind_first + 1), contracts::loom_first,
                                static_cast<std::uint8_t>(contracts::loom_first + 1), static_cast<std::uint8_t>(contracts::loom_first + 2)}) {
            auto& held = player_slot(inv, slot);
            const auto input = held;
            if (input.empty()) continue;
            held = {};
            record_slot(ctx, inv, slot);
            throw_item(ctx, c.drop_position, c.drop_velocity, put_back(input));
        }
        inv.cutter_recipe = -1;
        set_cutter_result(ctx, inv);
        set_smith_result(ctx, inv);
        set_grind_result(ctx, inv);
        inv.loom_pattern = -1;
        set_loom_result(ctx, inv);
        if (!inv.cursor.empty()) {
            const auto carried = inv.cursor;
            inv.cursor = {};
            record_slot(ctx, inv, player_slots);
            throw_item(ctx, c.drop_position, c.drop_velocity, put_back(carried));
        }
    };
}

// A dead player's items spread around the position, in a fixed pattern (the
// target version picks each direction and speed at random): eight directions
// at 0.25 and then 0.125 blocks per tick, and 0.2 upwards.
namespace {
contracts::Vec3 death_drop_velocity(std::size_t n) {
    constexpr double diagonal = 724.0 / 4096.0; // about 0.25 / sqrt(2)
    constexpr std::array<std::pair<double, double>, 8> directions{
        {{0.0, 0.25}, {diagonal, diagonal}, {0.25, 0.0}, {diagonal, -diagonal}, {0.0, -0.25}, {-diagonal, -diagonal}, {-0.25, 0.0},
         {-diagonal, diagonal}}};
    const auto [x, z] = directions[n % 8];
    const double scale = (n / 8) % 2 == 0 ? 1.0 : 0.5;
    return {x * scale, 819.0 / 4096.0, z * scale};
}
} // namespace

contracts::Result<std::function<void()>> plan_death(RuleContext& ctx, const contracts::PlayerDeath& c) {
    const auto cell = contracts::block_of(c.position);
    const bool in_world = contracts::within_border(c.position) && ctx.world().contains_position_y(c.position.y)
                          && ctx.chunk_generation(contracts::chunk_of(cell)).has_value();
    return [&ctx, c, cell, in_world] {
        if (!ctx.peek_player(c.player)) return; // nothing to drop
        auto& inv = ctx.player(c.player);
        reset_drag(inv);
        // The target version's order: hotbar, main inventory, armor from the
        // feet up, off hand; then the crafting grids (an open crafting
        // table's after the own one) and the carried stack.
        std::vector<std::uint8_t> order;
        for (std::uint8_t s = contracts::hotbar_first; s < contracts::offhand_slot; ++s) order.push_back(s);
        for (std::uint8_t s = 9; s < contracts::hotbar_first; ++s) order.push_back(s);
        for (std::uint8_t s = 8; s >= 5; --s) order.push_back(s);
        order.push_back(contracts::offhand_slot);
        for (std::uint8_t s = 1; s <= 4; ++s) order.push_back(s);
        for (std::size_t k = 1; k <= table_grid.size(); ++k) order.push_back(table_grid.record(k));
        order.push_back(contracts::cutter_first);
        for (std::uint8_t k = 0; k < 3; ++k) order.push_back(static_cast<std::uint8_t>(contracts::smith_first + k));
        for (std::uint8_t k = 0; k < 2; ++k) order.push_back(static_cast<std::uint8_t>(contracts::grind_first + k));
        for (std::uint8_t k = 0; k < 3; ++k) order.push_back(static_cast<std::uint8_t>(contracts::loom_first + k));
        std::size_t n = 0;
        const auto drop = [&](ItemStack stack) {
            if (in_world) spawn_dropped(ctx, cell, c.position, death_drop_velocity(n++), stack, 40);
        };
        for (const auto s : order) {
            auto& slot = player_slot(inv, s);
            if (slot.empty()) continue;
            const auto stack = slot;
            slot = {};
            record_slot(ctx, inv, s);
            drop(stack);
        }
        update_crafting(ctx, inv);
        inv.cutter_recipe = -1;
        set_cutter_result(ctx, inv);
        set_smith_result(ctx, inv);
        set_grind_result(ctx, inv);
        inv.loom_pattern = -1;
        set_loom_result(ctx, inv);
        if (!inv.cursor.empty()) {
            const auto stack = inv.cursor;
            inv.cursor = {};
            record_slot(ctx, inv, contracts::player_slots);
            drop(stack);
        }
    };
}

} // namespace circuit::gameplay::detail

namespace circuit::gameplay {
namespace {
// Whether a state is of a registry block without rules of its own.
bool of_block(BlockState s, const std::optional<std::uint32_t>& block) {
    if (!block || !s.is(BlockType::other)) return false;
    const auto state = physics::registry_state_of(s);
    return state && registry::block_of(*state) == block;
}
std::optional<std::uint32_t> find_registry_block(std::string_view name) {
    for (std::uint32_t b = 0; b < registry::block_count(); ++b)
        if (registry::game_name(b) == name) return b;
    return std::nullopt;
}
} // namespace

bool is_crafting_table(BlockState s) {
    static const auto block = find_registry_block("crafting_table");
    return of_block(s, block);
}
bool is_stonecutter(BlockState s) {
    static const auto block = find_registry_block("stonecutter");
    return of_block(s, block);
}
bool is_smithing_table(BlockState s) {
    static const auto block = find_registry_block("smithing_table");
    return of_block(s, block);
}
bool is_grindstone(BlockState s) {
    static const auto block = find_registry_block("grindstone");
    return of_block(s, block);
}
bool is_loom(BlockState s) {
    static const auto block = find_registry_block("loom");
    return of_block(s, block);
}
} // namespace circuit::gameplay
