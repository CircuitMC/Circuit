// SPDX-License-Identifier: AGPL-3.0-only
// Container menus: a player's own window and the windows of chests, double
// chests, hoppers, furnaces, crafters, crafting tables, stonecutters,
// smithing tables, grindstones, looms, anvils and enchanting tables, with every click mode of the protocol,
// applied as the target version's menus apply them (documented in the
// protocol pages and compared with the official server through a protocol
// client).
#include "internal.hpp"
#include <circuit/gameplay/equipment.hpp>
#include <circuit/gameplay/game_data.hpp>
#include <algorithm>
#include <array>

namespace circuit::gameplay::detail {
namespace {
using contracts::player_slots;
constexpr std::int16_t outside = -999;

// The armor slot of the own window an item may be worn in, or 0.
std::uint8_t armor_slot(std::uint16_t item) {
    const auto* worn = equipment::wearable(item);
    return worn ? equipment::inventory_slot(worn->slot, 0) : std::uint8_t{0};
}
bool offhand_item(std::uint16_t item) { return item == items::shield; }

// A player slot by its slot-record number: the inventory, the slots of an
// open workstation's window, or the ender inventory.
ItemStack& player_slot(PlayerInventory& inv, std::uint8_t index) {
    if (index >= contracts::beacon_first) return inv.beacon[index - contracts::beacon_first];
    if (index >= contracts::cartography_first) return inv.cartography[index - contracts::cartography_first];
    if (index >= contracts::ender_first) return inv.ender[index - contracts::ender_first];
    if (index >= contracts::enchant_first) return inv.enchant[index - contracts::enchant_first];
    if (index >= contracts::anvil_first) return inv.anvil[index - contracts::anvil_first];
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

// A cartography table (both official servers; client/maps): its map slot
// takes filled maps, the slot beside it paper, empty maps and glass panes.
bool cartography_takes(std::size_t slot, std::uint16_t item) {
    return slot == 0 ? item == items::filled_map : item == items::paper || item == items::map || item == items::glass_pane;
}

// What a cartography table makes of a filled map whose data exists: with an
// empty map two of it; with paper, while it is unlocked and below the
// largest scale, one of it marked to be zoomed; with a glass pane, while
// unlocked, one marked to be locked (map_post_processing 1 and 0). The
// marked map becomes a new map as it is taken.
void set_cartography_result(RuleContext& ctx, PlayerInventory& inv) {
    const auto* data = ctx.data();
    const auto& map = inv.cartography[0];
    const auto& extra = inv.cartography[1];
    ItemStack result;
    if (data && data->int_component && data->with_int_component && map.item == items::filled_map && !map.empty() && !extra.empty()) {
        const auto id = data->int_component(map.components, "map_id");
        const auto* found = id ? ctx.peek_map(*id) : nullptr;
        const auto marked = [&](std::int32_t how) {
            auto out = map;
            out.count = 1;
            if (const auto components = data->with_int_component(map.components, "map_post_processing", how)) out.components = *components;
            else out = {};
            return out;
        };
        if (found && extra.item == items::map) {
            result = map;
            result.count = 2;
        } else if (found && extra.item == items::paper && !found->locked && found->scale < max_map_scale) {
            result = marked(map_zooms);
        } else if (found && extra.item == items::glass_pane && !found->locked) {
            result = marked(map_locks);
        }
    }
    if (inv.cartography[2] == result) return;
    inv.cartography[2] = result;
    record_slot(ctx, inv, contracts::cartography_first + 2);
}

// One menu: a list of slots, each a container slot or a player slot.
class Menu {
public:
    Menu(RuleContext& ctx, PlayerInventory& inv, std::vector<BlockPos> parts, bool creative) : ctx_(ctx), inv_(inv), creative_(creative) {
        table_ = parts.size() == 1 && is_crafting_table(ctx.block(parts[0]));
        cutter_ = parts.size() == 1 && is_stonecutter(ctx.block(parts[0]));
        smith_ = parts.size() == 1 && is_smithing_table(ctx.block(parts[0]));
        grind_ = parts.size() == 1 && is_grindstone(ctx.block(parts[0]));
        loom_ = parts.size() == 1 && is_loom(ctx.block(parts[0]));
        anvil_ = parts.size() == 1 && is_anvil(ctx.block(parts[0]));
        enchant_ = parts.size() == 1 && is_enchanting_table(ctx.block(parts[0]));
        cartography_ = parts.size() == 1 && is_cartography_table(ctx.block(parts[0]));
        beacon_ = parts.size() == 1 && ctx.block(parts[0]).is(BlockType::beacon);
        const bool ender = parts.size() == 1 && ctx.block(parts[0]).is(BlockType::ender_chest);
        if (parts.size() == 1) block_ = parts[0];
        if (ender) {
            // An ender chest's window holds the player's ender inventory, and
            // works as a chest's.
            for (std::uint8_t i = 0; i < contracts::ender_slots; ++i)
                slots_.push_back({{}, static_cast<std::uint8_t>(contracts::ender_first + i), false});
        } else if (cartography_) {
            for (std::uint8_t i = 0; i < contracts::cartography_slots; ++i)
                slots_.push_back({{}, static_cast<std::uint8_t>(contracts::cartography_first + i), false});
        } else if (beacon_) {
            slots_.push_back({{}, contracts::beacon_first, false});
        } else if (enchant_) {
            for (std::uint8_t i = 0; i < contracts::enchant_slots; ++i)
                slots_.push_back({{}, static_cast<std::uint8_t>(contracts::enchant_first + i), false});
        } else if (anvil_) {
            for (std::uint8_t i = 0; i < contracts::anvil_slots; ++i)
                slots_.push_back({{}, static_cast<std::uint8_t>(contracts::anvil_first + i), false});
        } else if (loom_) {
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
        shulker_ = parts.size() == 1 && ctx.block(parts[0]).is(BlockType::shulker_box);
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
            // Any change of a crafting table's grid ends its placed recipe.
            const bool table_cell = s.index > contracts::table_first && s.index < contracts::table_first + contracts::table_slots;
            if (table_cell) inv_.table_recipe = -1;
            if (table_cell || (s.index > own_grid.first && s.index <= own_grid.first + own_grid.size())) grid_changed_ = true;
            if (s.index == contracts::cutter_first) cutter_input_changed(ctx_, inv_, before);
            if (s.index >= contracts::smith_first && s.index < contracts::smith_first + 3) set_smith_result(ctx_, inv_);
            if (s.index >= contracts::grind_first && s.index < contracts::grind_first + 2) set_grind_result(ctx_, inv_);
            if (s.index >= contracts::cartography_first && s.index < contracts::cartography_first + 2) set_cartography_result(ctx_, inv_);
            if (s.index >= contracts::anvil_first && s.index < contracts::anvil_first + 2) set_anvil_result(ctx_, inv_, creative_);
            if (enchant_ && s.index >= contracts::enchant_first && s.index < contracts::enchant_first + 2) set_enchant_offers(ctx_, inv_, *block_);
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
        // Taking from a furnace's result gives its experience.
        if (furnace_ && s.index == 2 && stack.count < before.count) furnace_result_taken(ctx_, s.pos, inv_.player);
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
        return ((own_ || table_) && i == 0) || (cutter_ && i == 1) || ((smith_ || loom_) && i == 3) || ((grind_ || anvil_ || cartography_) && i == 2);
    }
    [[nodiscard]] std::size_t result_slot() const { return smith_ || loom_ ? 3 : grind_ || anvil_ || cartography_ ? 2 : cutter_ ? 1 : 0; }
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
        if (cartography_ && i < 2) return cartography_takes(i, stack.item);
        if (loom_ && i < 3) return loom_takes(i, stack.item);
        // An enchanting table's lapis slot takes lapis lazuli.
        if (enchant_ && i == 1) return stack.item == items::lapis_lazuli;
        // A beacon's slot takes payments (client/beacon).
        if (beacon_ && i == 0) return is_beacon_payment(ctx_, stack.item);
        // A shulker box takes no shulker box (client/shulker).
        if (shulker_ && i < container_size_) return !is_shulker_box_item(stack.item);
        // A crafter's disabled slots take nothing.
        if (crafter_ && i < container_size_) {
            const auto* be = ctx_.peek_block_entity(slots_[i].pos);
            return !be || (be->disabled >> slots_[i].index & 1) == 0;
        }
        return true;
    }
    [[nodiscard]] bool may_pickup(std::size_t i) const {
        if (result(i)) return false;
        // A piece whose enchantment keeps it on (the curse of binding) stays
        // in its armor slot outside creative mode: no click takes it, swaps it
        // or collects it (client/ench_curses).
        if (own_ && i >= 5 && i <= 8 && !creative_) {
            const auto* data = ctx_.data();
            if (data && data->enchanted_with(get(i).components, &enchantments::Effects::prevent_armor_change)) return false;
        }
        return true;
    }
    [[nodiscard]] int max_stack(std::size_t i, ItemStack stack) const {
        if (own_ && i >= 5 && i <= 8) return 1;
        if (furnace_ && i == 1 && stack.item == items::bucket) return 1;
        if (brewing_ && i < 3) return 1;
        if (enchant_ && i == 0) return 1;
        if (beacon_ && i == 0) return 1;
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
        } else if (beacon_) {
            // The payment goes to the inventory from the hotbar's end; from
            // the inventory a single payment into the empty payment slot (a
            // stack of two moves on as any other: client/beacon), else
            // between the main inventory and the hotbar.
            if (i < 1) {
                moved = move_into(stack, 1, 37, true);
            } else if (stack.count == 1 && get(0).empty() && may_place(0, stack)) {
                set(0, stack);
                stack = {};
                moved = true;
            } else if (i < 28) {
                moved = move_into(stack, 28, 37, false);
            } else {
                moved = move_into(stack, 1, 28, false);
            }
        } else if (cartography_) {
            // The inputs go to the inventory from its start; from the
            // inventory a filled map goes into the map slot, paper, empty maps
            // and glass panes beside it, else nowhere; the rest between the
            // main inventory and the hotbar.
            if (i < 2) moved = move_into(stack, 3, 39, false);
            else if (cartography_takes(0, stack.item)) moved = move_into(stack, 0, 1, false);
            else if (cartography_takes(1, stack.item)) moved = move_into(stack, 1, 2, false);
            else if (i < 30) moved = move_into(stack, 30, 39, false);
            else moved = move_into(stack, 3, 30, false);
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
        } else if (enchant_) {
            // The item and the lapis go to the inventory from the hotbar's
            // end; from the inventory lapis goes into the lapis slot, another
            // item, one of it, into the empty item slot, else nowhere.
            if (i < 2) {
                moved = move_into(stack, 2, 38, true);
            } else if (stack.item == items::lapis_lazuli) {
                moved = move_into(stack, 1, 2, true);
            } else if (get(0).empty() && may_place(0, stack)) {
                auto one = stack;
                one.count = 1;
                stack.count = static_cast<std::uint8_t>(stack.count - 1);
                if (stack.count == 0) stack = {};
                set(0, one);
                moved = true;
            }
        } else if (anvil_) {
            // The inputs go to the inventory from its start; from the
            // inventory an item goes into the first input that is empty or
            // holds its kind with room, else between the main inventory and
            // the hotbar.
            std::optional<std::size_t> to;
            for (std::size_t k = 0; k < 2 && i >= 3 && !to; ++k) {
                const auto there = get(k);
                if (there.empty() || (there.same_kind(stack) && there.count < max_stack(k, there))) to = k;
            }
            if (i < 2) moved = move_into(stack, 3, 39, false);
            else if (to) moved = move_into(stack, *to, *to + 1, false);
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
        // A crafting grid's slot emptied by a pickup or a throw changes twice,
        // as both official servers count it (each change sends the result
        // again; client/own_grid_counts).
        if (existing.count == 0 && crafting_grid_slot(i)) record_slot(ctx_, inv_, slots_[i].index);
        return taken;
    }
    [[nodiscard]] bool crafting_grid_slot(std::size_t i) const {
        const auto& s = slots_[i];
        return !s.container && ((s.index >= 1 && s.index <= 4) || (s.index > contracts::table_first && s.index <= contracts::table_first + 9));
    }

    // Window slot of an own-inventory slot (hotbar 0-8, off hand 40) in this menu.
    [[nodiscard]] std::optional<std::size_t> menu_index_of_player_slot(std::uint8_t player_slot) const {
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (!slots_[i].container && slots_[i].index == player_slot) return i;
        return std::nullopt;
    }
    [[nodiscard]] bool own() const { return own_; }
    [[nodiscard]] bool table() const { return table_; }
    [[nodiscard]] bool furnace() const { return furnace_; }
    [[nodiscard]] bool cutter() const { return cutter_; }
    [[nodiscard]] bool smith() const { return smith_; }
    [[nodiscard]] bool grind() const { return grind_; }
    [[nodiscard]] bool loom() const { return loom_; }
    [[nodiscard]] bool anvil() const { return anvil_; }
    [[nodiscard]] bool enchant() const { return enchant_; }
    [[nodiscard]] bool cartography() const { return cartography_; }
    [[nodiscard]] bool beacon() const { return beacon_; }
    [[nodiscard]] bool creative() const { return creative_; }
    [[nodiscard]] CraftingGrid grid() const { return table_ ? table_grid : own_grid; }
    // Whether a crafting grid's cell changed through this menu.
    [[nodiscard]] bool grid_changed() const { return grid_changed_; }
    // The window slots of the main inventory and the hotbar.
    [[nodiscard]] std::size_t inventory_first() const {
        return smith_ || loom_ ? 4 : grind_ || anvil_ || cartography_ ? 3 : cutter_ || enchant_ ? 2 : beacon_ ? 1 : table_ ? 10 : 9;
    }
    [[nodiscard]] std::size_t inventory_end() const {
        return smith_ || loom_ ? 40 : grind_ || anvil_ || cartography_ ? 39 : cutter_ || enchant_ ? 38 : beacon_ ? 37 : table_ ? 46 : 45;
    }
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
    bool grid_changed_{};
    bool cutter_{};
    bool smith_{};
    bool grind_{};
    bool loom_{};
    bool anvil_{};
    bool enchant_{};
    bool cartography_{};
    bool beacon_{};
    bool brewing_{};
    bool shulker_{};
    bool creative_{};
    std::optional<BlockPos> block_;
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
// The slot changes twice then, as both official servers count it (each
// change sends the result again; client/special_a, client/own_grid_counts).
void use_grid(RuleContext& ctx, PlayerInventory& inv, CraftingGrid grid, const contracts::ContainerClick& c) {
    const bool table = grid.first == table_grid.first;
    // A special recipe's kept stack (a banner copied) is its remainder; an
    // extended map's new map is added (the id taken_marked_map gave it).
    std::optional<std::size_t> kept;
    if (const auto* data = ctx.data()) {
        std::vector<ItemStack> stacks;
        for (std::size_t k = 1; k <= grid.size(); ++k) stacks.push_back(player_slot(inv, grid.record(k)));
        if (const auto r = grid_recipe(*data, {grid.width, grid.width, stacks}, table ? inv.table_recipe : -1, map_lookup(ctx)); r.special) {
            kept = r.special->kept;
            if (r.special->kind == recipes::Special::Kind::map_extending) add_marked_map(ctx, r.special->result);
        }
    }
    // The grid changes: a crafting table's next result is the first recipe's
    // (a shift-click then stops when that is another item; client/ns_place2).
    if (table) inv.table_recipe = -1;
    for (std::size_t k = 1; k <= grid.size(); ++k) {
        const auto slot = grid.record(k);
        auto s = player_slot(inv, slot);
        if (s.empty()) continue;
        auto left = recipes::remainder(s.item);
        if (kept == k - 1) {
            left = s;
            left.count = 1;
        }
        s.count = static_cast<std::uint8_t>(s.count - 1);
        if (s.count == 0) s = {};
        if (!left.empty()) {
            player_slot(inv, slot) = s;
            record_slot(ctx, inv, slot);
            if (s.empty()) {
                s = left;
            } else if (s.same_kind(left)) {
                s.count = static_cast<std::uint8_t>(s.count + left.count);
            } else {
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
void use_grind(RuleContext& ctx, PlayerInventory& inv, std::optional<BlockPos> at) {
    // Experience at the grindstone's center: of the minimum costs i of the
    // inputs' enchantments that were not curses, ceil(i / 2) and a level
    // random draw below that (minecraft.wiki "Grindstone").
    if (const auto* data = ctx.data(); data && data->enchantments_of && at) {
        std::int32_t cost = 0;
        for (std::size_t k = 0; k < 2; ++k) {
            const auto& input = inv.grind[k];
            if (input.empty()) continue;
            for (const bool stored : {false, true})
                for (const auto& [id, level] : data->enchantments_of(input.components, stored)) {
                    if (std::ranges::binary_search(data->curses, id)) continue;
                    if (const auto it = data->enchantment_min_cost.find(id); it != data->enchantment_min_cost.end())
                        cost += it->second.first + it->second.second * (level - 1);
                }
        }
        if (cost > 0) {
            const auto half = (cost + 1) / 2;
            award_experience(ctx, {at->x + 0.5, at->y + 0.5, at->z + 0.5}, half + ctx.random_int(half));
        }
    }
    for (std::size_t k = 0; k < 2; ++k) {
        if (inv.grind[k].empty()) continue;
        inv.grind[k] = {};
        record_slot(ctx, inv, static_cast<std::uint8_t>(contracts::grind_first + k));
    }
    set_grind_result(ctx, inv);
}

// A loom's result was taken: the banner and the dye each give one item
// (the pattern item stays), and the result follows what is left.
// A cartography table's result was taken: a marked map's new map is added
// (the id taken_marked_map gave it), and one of each input is used.
void use_cartography(RuleContext& ctx, PlayerInventory& inv) {
    add_marked_map(ctx, inv.cartography[2]);
    for (std::size_t k = 0; k < 2; ++k) {
        auto& slot = inv.cartography[k];
        if (slot.empty()) continue;
        slot.count = static_cast<std::uint8_t>(slot.count - 1);
        if (slot.count == 0) slot = {};
        record_slot(ctx, inv, static_cast<std::uint8_t>(contracts::cartography_first + k));
    }
    set_cartography_result(ctx, inv);
}

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
// An anvil's result is taken only with its cost in levels (any in creative
// mode).
// The recipe whose result a player takes from a crafting grid, stonecutter
// or smithing table, for the server's recipe book (TraceKind::recipe_used).
void record_recipe_used(RuleContext& ctx, const Menu& menu, PlayerInventory& inv) {
    const auto* data = ctx.data();
    if (!data) return;
    const auto record = [&](std::uint64_t kind, std::size_t index) {
        ctx.record(TraceKind::recipe_used, {}, inv.player.hi, inv.player.lo, kind << 32 | index);
    };
    if (menu.smith()) {
        const auto fits = [](const std::optional<recipes::Ingredient>& i, const ItemStack& s) {
            return i ? !s.empty() && i->test(s.item) : s.empty();
        };
        for (std::size_t k = 0; k < data->smithing.size(); ++k) {
            const auto& r = data->smithing[k];
            if (fits(r.template_item, inv.smith[0]) && !inv.smith[1].empty() && r.base.test(inv.smith[1].item) && fits(r.addition, inv.smith[2]))
                return record(3, k);
        }
    } else if (menu.cutter()) {
        if (inv.cutter_recipe < 0 || inv.cutter[0].empty()) return;
        const auto list = recipes::cuttings(data->stonecutting, inv.cutter[0].item);
        if (static_cast<std::size_t>(inv.cutter_recipe) < list.size())
            record(2, static_cast<std::size_t>(list[static_cast<std::size_t>(inv.cutter_recipe)] - data->stonecutting.data()));
    } else if (!menu.grind() && !menu.anvil() && !menu.loom() && !menu.cartography()) {
        const auto grid = menu.grid();
        std::vector<ItemStack> stacks;
        for (std::size_t k = 1; k <= grid.size(); ++k) stacks.push_back(player_slot(inv, grid.record(k)));
        // A special recipe (one that comes first in id order too) is not
        // recorded; a crafting table's placed recipe is (not observed: the
        // player who placed it knows it already).
        if (const auto r = grid_recipe(*data, {grid.width, grid.width, stacks}, menu.table() ? inv.table_recipe : -1, map_lookup(ctx));
            r.made && r.made->recipe)
            record(0, static_cast<std::size_t>(r.made->recipe - data->crafting.data()));
    }
}

// Whether a crafting grid's result is an extended map (map_extending).
bool extends_map(RuleContext& ctx, const Menu& menu, PlayerInventory& inv) {
    const auto* data = ctx.data();
    if (!data || !(menu.own() || menu.table())) return false;
    const auto grid = menu.grid();
    std::vector<ItemStack> stacks;
    for (std::size_t k = 1; k <= grid.size(); ++k) stacks.push_back(player_slot(inv, grid.record(k)));
    const auto r = grid_recipe(*data, {grid.width, grid.width, stacks}, menu.table() ? inv.table_recipe : -1, map_lookup(ctx));
    return r.special && r.special->kind == recipes::Special::Kind::map_extending;
}

void click_result(RuleContext& ctx, Menu& menu, const contracts::ContainerClick& c) {
    auto& inv = menu.inventory();
    if (menu.anvil() && !anvil_result_takeable(inv, c.creative)) return;
    const auto made_now = [&] { return menu.get(menu.result_slot()); };
    const auto use = [&] {
        record_recipe_used(ctx, menu, inv);
        if (menu.smith()) use_smith(ctx, inv);
        else if (menu.grind()) use_grind(ctx, inv, c.container);
        else if (menu.anvil()) use_anvil(ctx, inv, c.container, c.creative);
        else if (menu.loom()) use_loom(ctx, inv);
        else if (menu.cartography()) use_cartography(ctx, inv);
        else if (menu.cutter()) use_cutter(ctx, inv);
        else use_grid(ctx, inv, menu.grid(), c);
    };
    // A marked map becomes the new map it makes as it is taken: a cartography
    // table's, and in a crafting grid an extended map (a marked map another
    // recipe makes, which only creative players can give, is taken as it is).
    const auto marked = [&] { return menu.cartography() || extends_map(ctx, menu, inv); };
    const auto taken = [&](const ItemStack& s, bool mark) { return mark ? taken_marked_map(ctx, s) : s; };
    const auto result = made_now();
    if (result.empty()) return;
    switch (c.mode) {
    case 0: { // onto the cursor, when all of it fits there
        if (c.button > 1) return;
        auto carried = menu.cursor();
        const auto made = taken(result, marked());
        if (carried.empty()) {
            carried = made;
        } else if (carried.same_kind(made) && carried.count + made.count <= items::max_stack(carried)) {
            carried.count = static_cast<std::uint8_t>(carried.count + made.count);
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
            const bool mark = marked();
            auto stack = taken(made, mark);
            if (!menu.move_into(stack, menu.inventory_first(), menu.inventory_end(), true)) return;
            use();
            // An extended map shift-clicked out makes a second new map, which
            // nothing holds (both official servers; client/map_extend2).
            if (mark && !menu.cartography()) add_marked_map(ctx, made);
            // What did not fit drops.
            throw_item(ctx, c.drop_position, c.drop_velocity, stack);
        }
        return;
    }
    case 2: { // into an empty hotbar slot or off hand
        if (!(c.button < 9 || c.button == 40)) return;
        const std::uint8_t slot = c.button == 40 ? contracts::offhand_slot : static_cast<std::uint8_t>(contracts::hotbar_first + c.button);
        if (!inv.slots[slot].empty()) return;
        inv.slots[slot] = taken(result, marked());
        record_slot(ctx, inv, slot);
        use();
        return;
    }
    case 4: { // thrown; with button 1, again while the result stays the same item
        if (!menu.cursor().empty()) return;
        for (auto made = made_now(); !made.empty() && made.item == result.item; made = made_now()) {
            throw_item(ctx, c.drop_position, c.drop_velocity, taken(made, marked()));
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
    // With doLimitedCrafting on, only recipes the player knows make a result
    // (in creative mode too; both official servers).
    const bool limited = ctx.bundle().schema.game_rule("doLimitedCrafting") && ctx.game_rule("doLimitedCrafting") != 0;
    const auto known = [&](const recipes::Recipe* r) {
        return std::ranges::binary_search(inv.known_crafting, static_cast<std::uint32_t>(r - data->crafting.data()));
    };
    const auto craft = [&](std::span<ItemStack> slots, std::uint8_t width, std::uint8_t first, std::int32_t placed) {
        const auto grid = slots.subspan(1);
        const bool empty = std::ranges::all_of(grid, [](const ItemStack& s) { return s.empty(); });
        const auto r = empty ? GridRecipe{} : grid_recipe(*data, {width, width, grid}, placed, map_lookup(ctx));
        // Special recipes are not limited.
        const auto result = r.made && (!limited || known(r.made->recipe)) ? r.made->result : r.special ? r.special->result : ItemStack{};
        if (slots[0] == result) return;
        slots[0] = result;
        record_slot(ctx, inv, first);
    };
    // Only a crafting table keeps a placed recipe: the own grid makes the
    // first recipe, placed or not (both official servers; client/ns_place).
    craft(std::span(inv.slots).first(5), 2, 0, -1);
    craft(std::span(inv.table), 3, contracts::table_first, inv.table_recipe);
}

contracts::Result<std::vector<BlockPos>> menu_parts(RuleContext& ctx, std::optional<BlockPos> container) {
    if (!container) return std::vector<BlockPos>{};
    const auto s = ctx.block(*container);
    if (!s.is_unloaded()
        && (is_crafting_table(s) || is_stonecutter(s) || is_smithing_table(s) || is_grindstone(s) || is_loom(s) || is_anvil(s) || is_enchanting_table(s) || is_cartography_table(s)
            || s.is(BlockType::ender_chest) || s.is(BlockType::beacon)))
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
    // A lectern's window has one slot, its book: a click takes it onto the
    // cursor, a number key into an empty hotbar slot, a throw out of the
    // window (client/lectern); nothing else moves it.
    if (c.container && ctx.block(*c.container).is(BlockType::lectern))
        return [&ctx, c] {
            if (c.slot != 0 || !(c.mode == 0 || c.mode == 2 || c.mode == 4)) return;
            auto& inv = ctx.player(c.player);
            if (c.mode == 0 && (c.button > 1 || !inv.cursor.empty())) return;
            const std::uint8_t hand = c.button == 40 ? contracts::offhand_slot : static_cast<std::uint8_t>(contracts::hotbar_first + c.button);
            if (c.mode == 2 && (!(c.button < 9 || c.button == 40) || !inv.slots[hand].empty())) return;
            if (c.mode == 4 && !inv.cursor.empty()) return;
            const auto book = take_lectern_book(ctx, *c.container);
            if (book.empty()) return;
            if (c.mode == 0) {
                inv.cursor = book;
                record_slot(ctx, inv, player_slots);
            } else if (c.mode == 2) {
                inv.slots[hand] = book;
                record_slot(ctx, inv, hand);
            } else {
                throw_item(ctx, c.drop_position, c.drop_velocity, book);
            }
        };
    auto parts = menu_parts(ctx, c.container);
    if (!parts) return std::unexpected(parts.error());
    if (c.mode > 6) return std::unexpected(contracts::ErrorCode::invalid_argument);
    return [&ctx, c, parts = std::move(*parts)] {
        auto& inv = ctx.player(c.player);
        Menu menu(ctx, inv, parts, c.creative);
        if (c.slot != outside && !menu.valid(c.slot) && !(c.mode == 5 && (c.button & 3) != 1)) return;
        // The grid's result follows a click that changed the grid, and only
        // such a click: a recipe the player no longer knows makes it until
        // then, and it can be taken (both official servers; client/recipe_take).
        struct Recraft {
            RuleContext& ctx;
            PlayerInventory& inv;
            const Menu& menu;
            ~Recraft() {
                if (menu.grid_changed()) update_crafting(ctx, inv);
            }
        } recraft{ctx, inv, menu};
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
// A beacon's window: with a payment in its slot, the payment is used up and
// the beacon takes the powers chosen (client/beacon: whatever its levels;
// an effect that is not a beacon's power counts as none). Without one,
// nothing happens.
contracts::Result<std::function<void()>> plan_set_beacon(RuleContext& ctx, const contracts::SetBeacon& c) {
    if (!c.container || !ctx.block(*c.container).is(BlockType::beacon)) return std::unexpected(contracts::ErrorCode::not_a_container);
    return [&ctx, c] {
        auto& inv = ctx.player(c.player);
        auto& payment = inv.beacon[0];
        if (payment.empty()) return;
        const auto power = [](std::optional<std::int32_t> effect) { return effect ? beacon_power(*effect) : std::uint8_t{0}; };
        choose_beacon_powers(ctx, *c.container, power(c.primary), power(c.secondary));
        if (--payment.count == 0) payment = {};
        record_slot(ctx, inv, contracts::beacon_first);
    };
}

contracts::Result<std::function<void()>> plan_button(RuleContext& ctx, const contracts::ContainerButton& c) {
    // A lectern's buttons turn its book's pages or take the book.
    if (c.container && ctx.block(*c.container).is(BlockType::lectern))
        return [&ctx, c] { press_lectern_button(ctx, ctx.player(c.player), *c.container, c.button); };
    auto parts = menu_parts(ctx, c.container);
    if (!parts) return std::unexpected(parts.error());
    const bool cutter = parts->size() == 1 && is_stonecutter(ctx.block(parts->front()));
    const bool loom = parts->size() == 1 && is_loom(ctx.block(parts->front()));
    const bool table = parts->size() == 1 && is_enchanting_table(ctx.block(parts->front()));
    return [&ctx, c, cutter, loom, table] {
        const auto* data = ctx.data();
        if (!data) return;
        // An enchanting table's offer.
        if (table) {
            enchant_by_offer(ctx, ctx.player(c.player), *c.container, c.button, c.creative);
            return;
        }
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

namespace {
// A recipe's ingredients as placing it lays them out: its cells row by row
// (an empty ingredient for an empty cell), `width` and `height` for a shaped
// recipe; the others fill the grid row by row.
struct Layout {
    std::vector<const recipes::Ingredient*> cells;
    std::uint8_t width{}, height{};
};
std::optional<Layout> layout_of(const GameData& data, std::uint8_t kind, std::uint32_t index) {
    Layout l;
    if (kind == 0 && index < data.crafting.size()) {
        const auto& r = data.crafting[index];
        for (const auto& i : r.ingredients) l.cells.push_back(&i);
        if (r.kind == recipes::Recipe::Kind::shaped) {
            l.width = r.width;
            l.height = r.height;
        }
    } else if (kind == 1 && index < data.cooking.size()) {
        l.cells.push_back(&data.cooking[index].ingredient);
    } else if (kind == 2 && index < data.stonecutting.size()) {
        l.cells.push_back(&data.stonecutting[index].ingredient);
    } else if (kind == 3 && index < data.smithing.size()) {
        const auto& r = data.smithing[index];
        if (r.template_item) l.cells.push_back(&*r.template_item);
        l.cells.push_back(&r.base);
        if (r.addition) l.cells.push_back(&*r.addition);
    } else {
        return std::nullopt;
    }
    return l;
}

// The inventory's slots placing takes from: the hotbar, then the main inventory.
constexpr auto placing_order = [] {
    std::array<std::uint8_t, 36> order{};
    for (std::size_t i = 0; i < 36; ++i) order[i] = static_cast<std::uint8_t>(i < 9 ? contracts::hotbar_first + i : i);
    return order;
}();
// Stacks without damage or components of their own.
bool plain(const ItemStack& s) { return !s.empty() && s.damage == 0 && s.components.empty(); }

// The item each non-empty cell takes `count` of: each of `kinds` (items and
// how many the inventory holds, in the order it first holds them) fills as
// many cells as it has `count`s, at most its stack limit each. Cells try the
// kinds in order, earlier cells giving way to later ones where needed; none
// when they cannot all be filled.
std::optional<std::vector<std::uint16_t>> choose_items(const Layout& l, const std::vector<std::pair<std::uint16_t, int>>& kinds, int count) {
    std::vector<std::size_t> cells;
    for (std::size_t j = 0; j < l.cells.size(); ++j)
        if (!l.cells[j]->empty()) cells.push_back(j);
    std::vector<std::size_t> room(kinds.size());
    for (std::size_t t = 0; t < kinds.size(); ++t)
        room[t] = items::max_stack(ItemStack{kinds[t].first, 1}) >= count ? static_cast<std::size_t>(kinds[t].second / count) : 0;
    std::vector<std::size_t> owner(cells.size());
    std::vector<std::vector<std::size_t>> holders(kinds.size());
    std::vector<bool> seen;
    std::function<bool(std::size_t)> augment = [&](std::size_t c) {
        for (std::size_t t = 0; t < kinds.size(); ++t) {
            if (seen[t] || room[t] == 0 || !l.cells[cells[c]]->test(kinds[t].first)) continue;
            seen[t] = true;
            if (holders[t].size() < room[t]) {
                owner[c] = t;
                holders[t].push_back(c);
                return true;
            }
            for (auto& other : holders[t])
                if (augment(other)) {
                    other = c;
                    owner[c] = t;
                    return true;
                }
        }
        return false;
    };
    for (std::size_t c = 0; c < cells.size(); ++c) {
        seen.assign(kinds.size(), false);
        if (!augment(c)) return std::nullopt;
    }
    std::vector<std::uint16_t> chosen(l.cells.size());
    for (std::size_t c = 0; c < cells.size(); ++c) chosen[cells[c]] = kinds[owner[c]].first;
    return chosen;
}

// Placing a recipe from the recipe book (both official servers;
// client/recipes_place). The window's grid (the own window's 2x2, a crafting
// table's 3x3, a furnace's input) goes back into the inventory first, as
// picked-up items go; when it would not all fit, nothing happens. With the
// inventory's items (the hotbar's and main inventory's stacks without damage
// or components of their own, in that order) the recipe is then made once more
// than the grid already holds it (its smallest stack), or as many times as
// fit: at most the stack limit of each item taken, and within what the
// inventory holds. A recipe the window does not take (a cooking recipe in a
// crafting grid, a crafting recipe in a furnace) changes nothing when there is
// enough of it. With too little for one, the grid is emptied, and the ghost
// recipe shown (TraceKind::recipe_ghost; a crafting table shows none of other
// recipes). The cells go into the grid row by row: a recipe narrower or lower
// than half the grid is centered, and what does not fit is left out.
void place_recipe(RuleContext& ctx, Menu& menu, const contracts::PlaceRecipe& c, const Layout& l) {
    const bool crafting = menu.own() || menu.table();
    if (!crafting && !menu.furnace()) return;
    const std::size_t width = crafting ? menu.grid().width : 1;
    const std::size_t first = crafting ? 1 : 0;
    auto& inv = menu.inventory();
    const auto* data = ctx.data();
    std::vector<ItemStack> grid;
    for (std::size_t k = 0; k < width * width; ++k) grid.push_back(menu.get(first + k));
    auto after = inv;
    for (const auto& s : grid)
        if (!s.empty() && !add_unrecorded(after, s, c.selected).empty()) return;
    std::vector<std::pair<std::uint16_t, int>> kinds;
    for (const auto slot : placing_order) {
        const auto& s = after.slots[slot];
        if (!plain(s)) continue;
        const auto it = std::ranges::find(kinds, s.item, &std::pair<std::uint16_t, int>::first);
        if (it == kinds.end()) kinds.emplace_back(s.item, s.count);
        else it->second += s.count;
    }
    int most = 0;
    for (const auto& [item, n] : kinds) most = std::max(most, n);
    most = std::min<int>(most, contracts::max_stack_size);
    int biggest = 0;
    for (int k = most; k >= 1 && biggest == 0; --k)
        if (choose_items(l, kinds, k)) biggest = k;
    const bool takes = crafting ? c.kind == 0 : c.kind == 1;
    if (biggest > 0 && !takes) return;
    // The grid goes back.
    for (std::size_t k = 0; k < grid.size(); ++k) {
        if (grid[k].empty()) continue;
        menu.set(first + k, {});
        add_to_player(ctx, inv, grid[k], c.selected);
    }
    if (biggest == 0) {
        if (crafting) update_crafting(ctx, inv);
        if (!(menu.table() && c.kind != 0)) ctx.record(TraceKind::recipe_ghost, {}, inv.player.hi, inv.player.lo, std::uint64_t{c.kind} << 32 | c.recipe);
        return;
    }
    // How many times the grid held the recipe: its smallest stack.
    int held = 0;
    const auto smallest = [&] {
        int n = 0;
        for (const auto& s : grid)
            if (!s.empty()) n = n == 0 ? s.count : std::min<int>(n, s.count);
        return n;
    };
    if (crafting) {
        if (recipes::match(std::span(&data->crafting[c.recipe], 1), {static_cast<std::uint8_t>(width), static_cast<std::uint8_t>(width), grid}))
            held = smallest();
    } else if (!grid[0].empty() && data->cooking[c.recipe].ingredient.test(grid[0].item)) {
        held = grid[0].count;
    }
    const int count = c.all ? biggest : std::min(held + 1, biggest);
    const auto chosen = choose_items(l, kinds, count);
    if (!chosen) return;
    const auto take = [&](std::uint16_t item) {
        int left = count;
        for (const auto slot : placing_order) {
            auto& s = inv.slots[slot];
            if (left == 0) break;
            if (!plain(s) || s.item != item) continue;
            const int n = std::min<int>(left, s.count);
            s.count = static_cast<std::uint8_t>(s.count - n);
            if (s.count == 0) s = {};
            record_slot(ctx, inv, slot);
            left -= n;
        }
        return ItemStack{item, static_cast<std::uint8_t>(count - left)};
    };
    const std::size_t w = l.width ? l.width : width, h = l.height ? l.height : width;
    const std::size_t across = std::min(w, width);
    const std::size_t x0 = w < width ? (width - w) / 2 : 0, y0 = h < width ? (width - h) / 2 : 0;
    for (std::size_t j = 0; j < l.cells.size(); ++j) {
        const auto x = x0 + j % across, y = y0 + j / across;
        if (y >= width) break;
        if (!l.cells[j]->empty()) menu.set(first + y * width + x, take((*chosen)[j]));
    }
    // A crafting table makes the placed recipe while its grid stays as placed,
    // also where an earlier recipe takes it, with doLimitedCrafting off and on
    // (both official servers; client/ns_place, client/ns_place2).
    if (menu.table()) inv.table_recipe = static_cast<std::int32_t>(c.recipe);
    if (crafting) update_crafting(ctx, inv);
}
} // namespace

contracts::Result<std::function<void()>> plan_place_recipe(RuleContext& ctx, const contracts::PlaceRecipe& c) {
    auto parts = menu_parts(ctx, c.container);
    if (!parts) return std::unexpected(parts.error());
    if (c.kind > 3) return std::unexpected(contracts::ErrorCode::invalid_argument);
    return [&ctx, c, parts = std::move(*parts)] {
        const auto* data = ctx.data();
        if (!data) return;
        const auto layout = layout_of(*data, c.kind, c.recipe);
        if (!layout) return;
        Menu menu(ctx, ctx.player(c.player), parts, false);
        place_recipe(ctx, menu, c, *layout);
    };
}

contracts::Result<std::function<void()>> plan_close(RuleContext& ctx, const contracts::CloseContainer& c) {
    (void)ctx;
    return [&ctx, c] {
        auto& inv = ctx.player(c.player);
        reset_drag(inv);
        // The player stops viewing the window's container (a close naming
        // another block, which the player left for this one, does not).
        if (!c.container || inv.viewing == c.container) stop_viewing(ctx, inv);
        // The crafting grids (the own window's, an open crafting table's)
        // and the carried stack go back into the inventory, or drop when the
        // player left.
        const auto put_back = [&](ItemStack s) { return c.drop ? s : add_to_player(ctx, inv, s, contracts::hotbar_first); };
        // Thrown, the carried stack goes first (client/ench_curses: a dead
        // player's, before the grid's, own or a crafting table's).
        const auto carried_out = [&] {
            if (inv.cursor.empty()) return;
            const auto carried = inv.cursor;
            inv.cursor = {};
            record_slot(ctx, inv, player_slots);
            throw_item(ctx, c.drop_position, c.drop_velocity, put_back(carried));
        };
        if (c.drop) carried_out();
        for (const auto grid : {own_grid, table_grid})
            for (std::size_t k = 1; k <= grid.size(); ++k) {
                auto& slot = player_slot(inv, grid.record(k));
                const auto s = slot;
                if (s.empty()) continue;
                slot = {};
                record_slot(ctx, inv, grid.record(k));
                throw_item(ctx, c.drop_position, c.drop_velocity, put_back(s));
            }
        inv.table_recipe = -1;
        update_crafting(ctx, inv);
        // An open stonecutter's input and smithing table's inputs too; their
        // results and the stonecutter's recipe clear.
        for (const auto slot : {contracts::cutter_first, contracts::smith_first, static_cast<std::uint8_t>(contracts::smith_first + 1),
                                static_cast<std::uint8_t>(contracts::smith_first + 2), contracts::grind_first,
                                static_cast<std::uint8_t>(contracts::grind_first + 1), contracts::loom_first,
                                static_cast<std::uint8_t>(contracts::loom_first + 1), static_cast<std::uint8_t>(contracts::loom_first + 2),
                                contracts::anvil_first, static_cast<std::uint8_t>(contracts::anvil_first + 1), contracts::enchant_first,
                                static_cast<std::uint8_t>(contracts::enchant_first + 1), contracts::cartography_first,
                                static_cast<std::uint8_t>(contracts::cartography_first + 1)}) {
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
        set_cartography_result(ctx, inv);
        inv.loom_pattern = -1;
        set_loom_result(ctx, inv);
        // An anvil's typed name goes with its window, and an enchanting
        // table's offers.
        inv.anvil_name.clear();
        set_anvil_result(ctx, inv, false);
        inv.enchant_costs = {};
        inv.enchant_clues = inv.enchant_clue_levels = {-1, -1, -1};
        carried_out();
        // A beacon's payment does not go back: it is thrown out
        // (client/beacon2, both versions).
        if (!inv.beacon[0].empty()) {
            const auto payment = inv.beacon[0];
            inv.beacon[0] = {};
            record_slot(ctx, inv, contracts::beacon_first);
            throw_item(ctx, c.drop_position, c.drop_velocity, payment);
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
        // feet up, off hand. The crafting grids, an open workstation's inputs
        // and the carried stack stay with the window until the body goes
        // (a CloseContainer that throws them; client/ench_curses), and so
        // does the viewing of an open container (Circuit's choice, as the
        // recorded crafting table's window stays).
        std::vector<std::uint8_t> order;
        for (std::uint8_t s = contracts::hotbar_first; s < contracts::offhand_slot; ++s) order.push_back(s);
        for (std::uint8_t s = 9; s < contracts::hotbar_first; ++s) order.push_back(s);
        for (std::uint8_t s = 8; s >= 5; --s) order.push_back(s);
        order.push_back(contracts::offhand_slot);
        std::size_t n = 0;
        // A stack whose enchantment makes it vanish (the curse of vanishing;
        // not one stored in a book or carried inside a shulker box or bundle)
        // empties its slot and drops nothing (client/ench_curses); the spread
        // pattern counts the drops alone.
        const auto* data = ctx.data();
        const auto vanishes = [&](const ItemStack& stack) {
            return data && data->enchanted_with(stack.components, &enchantments::Effects::prevent_equipment_drop);
        };
        for (const auto s : order) {
            auto& slot = inv.slots[s];
            if (slot.empty()) continue;
            const auto stack = slot;
            slot = {};
            record_slot(ctx, inv, s);
            if (in_world && !vanishes(stack)) spawn_dropped(ctx, cell, c.position, death_drop_velocity(n++), stack, 40);
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
bool is_cartography_table(BlockState s) {
    static const auto block = find_registry_block("cartography_table");
    return of_block(s, block);
}
bool is_enchanting_table(BlockState s) {
    static const auto block = find_registry_block("enchanting_table");
    return of_block(s, block);
}
bool is_anvil(BlockState s) {
    static const std::array blocks{find_registry_block("anvil"), find_registry_block("chipped_anvil"), find_registry_block("damaged_anvil")};
    return std::ranges::any_of(blocks, [&](const auto& block) { return of_block(s, block); });
}
} // namespace circuit::gameplay
