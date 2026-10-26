// SPDX-License-Identifier: AGPL-3.0-only
// Brewing stands, as both official servers run them (checked against their
// traces; minecraft.wiki "Brewing Stand", "Brewing"). The block entity holds
// three bottles (slots 0-2), the ingredient (3) and the fuel (4), the brews
// its fuel has left, the ticks its brew still takes and the item it brews
// with. In its block-entity tick:
// - without fuel left, an item of the fuel slot (the item tag brewing_fuel:
//   blaze powder) gives fuel for 20 brews and is used up, brewable or not;
// - brewing, the brew time falls; at 0 with something to brew the bottles
//   are brewed, and a brew whose ingredient changed, or that has nothing left
//   to brew, stops;
// - not brewing, with fuel and something to brew (an ingredient that some
//   bottle mixes with, potions.hpp) it uses a brew of fuel and brews for 400
//   ticks;
// - it shows which bottle slots hold something, without neighbor updates.
// Brewing turns each bottle that mixes with the ingredient into its mix and
// uses one ingredient; of a stack of more than one, the ingredient's
// remainder (a glass bottle of dragon's breath) is thrown out of the stand,
// while the last one leaves nothing.
//
// Hoppers reach the ingredient from above, the bottles and the fuel from
// the sides; from below they take the bottles and a glass bottle in the
// ingredient slot. A bottle slot takes a potion, splash potion, lingering
// potion or glass bottle when it is empty, the ingredient slot an
// ingredient and the fuel slot fuel.
#include "internal.hpp"
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/potions.hpp>
#include <circuit/gameplay/recipes.hpp>
#include <algorithm>

namespace circuit::gameplay::detail {
namespace {
using contracts::pack_stack;
constexpr std::int32_t brew_ticks = 400;
constexpr std::int32_t fuel_brews = 20;

bool is_fuel(const GameData* data, std::uint16_t item) {
    if (!data) return item == items::blaze_powder;
    return std::ranges::binary_search(data->brewing_fuel, item);
}

bool is_bottle(std::uint16_t item) {
    return item == items::potion || item == items::splash_potion || item == items::lingering_potion || item == items::glass_bottle;
}

// What a bottle becomes with the ingredient; nullopt without a mix (or the
// data packs' component encoding to read its potion).
std::optional<ItemStack> mixed(const GameData* data, const ItemStack& bottle, std::uint16_t ingredient) {
    if (bottle.empty() || !data || !data->potion_of || !data->with_potion) return std::nullopt;
    const auto potion = data->potion_of(bottle.components);
    if (!potion) return std::nullopt;
    const auto mix = potions::mix({bottle.item, *potion}, ingredient);
    if (!mix) return std::nullopt;
    auto components = data->with_potion(bottle.components, mix->potion);
    if (!components) return std::nullopt;
    auto out = bottle;
    out.item = mix->item;
    out.components = std::move(*components);
    return out;
}

bool brewable(const GameData* data, const BlockEntity& be) {
    const auto& ingredient = be.slots[3];
    if (ingredient.empty() || !potions::ingredient(ingredient.item)) return false;
    for (std::size_t k = 0; k < 3; ++k)
        if (mixed(data, be.slots[k], ingredient.item)) return true;
    return false;
}

void set_slot(RuleContext& ctx, BlockPos pos, BlockEntity& be, std::size_t slot, ItemStack value) {
    const auto before = be.slots[slot];
    be.slots[slot] = std::move(value);
    ctx.record(TraceKind::inventory_changed, pos, slot, pack_stack(before), pack_stack(be.slots[slot]));
}

void brew(RuleContext& ctx, BlockPos pos, BlockEntity& be) {
    const auto* data = ctx.data();
    const auto ingredient = be.slots[3];
    for (std::size_t k = 0; k < 3; ++k)
        if (auto out = mixed(data, be.slots[k], ingredient.item)) set_slot(ctx, pos, be, k, std::move(*out));
    auto rest = ingredient;
    rest.count = static_cast<std::uint8_t>(rest.count - 1);
    if (rest.count == 0) {
        rest = {};
        set_slot(ctx, pos, be, 3, rest);
        return;
    }
    set_slot(ctx, pos, be, 3, rest);
    if (const auto left = recipes::remainder(ingredient.item); !left.empty()) drop_contents(ctx, pos, {left});
}
} // namespace

void tick_brewing_stand(RuleContext& ctx, BlockPos pos, BlockState s) {
    auto* be = ctx.block_entity(pos);
    if (!be || be->slots.size() < 5) return;
    const auto* data = ctx.data();
    bool changed = false;
    if (be->lit_time <= 0 && !be->slots[4].empty() && is_fuel(data, be->slots[4].item)) {
        be->lit_time = fuel_brews;
        auto fuel = be->slots[4];
        fuel.count = static_cast<std::uint8_t>(fuel.count - 1);
        if (fuel.count == 0) fuel = {};
        set_slot(ctx, pos, *be, 4, fuel);
        changed = true;
    }
    const bool can = brewable(data, *be);
    if (be->cook_time > 0) {
        --be->cook_time;
        if (be->cook_time == 0 && can) brew(ctx, pos, *be);
        else if (!can || be->slots[3].item != be->cook_total) be->cook_time = 0;
        changed = true;
    } else if (can && be->lit_time > 0) {
        --be->lit_time;
        be->cook_time = brew_ticks;
        be->cook_total = be->slots[3].item;
        changed = true;
    }
    std::uint8_t bottles = 0;
    for (std::uint8_t k = 0; k < 3; ++k)
        if (!be->slots[k].empty()) bottles = static_cast<std::uint8_t>(bottles | 1u << k);
    if (bottles != s.level()) set_block_quiet(ctx, pos, s.with_level(bottles));
    if (changed) container_changed(ctx, pos);
}

std::vector<std::size_t> brewing_slots(Direction face) {
    if (face == Direction::up) return {3};
    if (face == Direction::down) return {0, 1, 2, 3};
    return {0, 1, 2, 4};
}

bool brewing_accepts(const GameData* data, const BlockEntity& be, std::size_t slot, const ItemStack& stack) {
    if (slot == 3) return potions::ingredient(stack.item);
    if (slot == 4) return is_fuel(data, stack.item);
    return slot < 3 && is_bottle(stack.item) && be.slots[slot].empty();
}

bool brewing_gives(std::size_t slot, const ItemStack& stack) { return slot != 3 || stack.item == items::glass_bottle; }

} // namespace circuit::gameplay::detail
