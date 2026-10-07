// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/survival.hpp>

namespace circuit::gameplay::detail {
namespace {
// Items dropped by blocks wait 10 ticks before players can pick them up.
constexpr std::uint16_t block_drop_delay = 10;

void spawn(RuleContext& ctx, BlockPos pos, ItemStack stack, std::uint16_t pickup_delay = block_drop_delay) {
    const contracts::FixedVec center{pos.x * contracts::fixed_one + contracts::fixed_one / 2,
                                     pos.y * contracts::fixed_one + contracts::fixed_one / 2,
                                     pos.z * contracts::fixed_one + contracts::fixed_one / 2};
    Entity e{{}, center, {}, stack, 0, 0, false};
    e.pickup_delay = pickup_delay;
    const auto id = ctx.spawn_entity(e);
    ctx.record(TraceKind::entity_spawned, pos, id.value, contracts::pack_stack(stack));
}

// The block's own item when a block breaks; nothing for glass, water and piston parts.
std::optional<ItemStack> loot(BlockState s) {
    switch (s.type()) {
    case BlockType::stone: return ItemStack{items::cobblestone, 1};
    case BlockType::obsidian: return ItemStack{items::obsidian, 1};
    case BlockType::redstone_block: return ItemStack{items::redstone_block, 1};
    case BlockType::slime_block: return ItemStack{items::slime_block, 1};
    case BlockType::redstone_lamp: return ItemStack{items::redstone_lamp, 1};
    case BlockType::redstone_wire: return ItemStack{items::redstone, 1};
    case BlockType::lever: return ItemStack{items::lever, 1};
    case BlockType::repeater: return ItemStack{items::repeater, 1};
    case BlockType::comparator: return ItemStack{items::comparator, 1};
    case BlockType::observer: return ItemStack{items::observer, 1};
    case BlockType::piston: return ItemStack{items::piston, 1};
    case BlockType::sticky_piston: return ItemStack{items::sticky_piston, 1};
    case BlockType::hopper: return ItemStack{items::hopper, 1};
    case BlockType::chest: return ItemStack{items::chest, 1};
    default: return std::nullopt;
    }
}
} // namespace

// Fixed drops without random quantities or offsets (declared simplification).
void drop_items(RuleContext& ctx, BlockPos pos, BlockState destroyed, ItemStack tool) {
    if (destroyed.is(BlockType::wheat)) {
        if (destroyed.level() >= 7) spawn(ctx, pos, {items::wheat, 1});
        spawn(ctx, pos, {items::wheat_seeds, 1});
        return;
    }
    if (!harvests(destroyed, tool)) return;
    if (const auto item = loot(destroyed)) spawn(ctx, pos, *item);
}

void drop_contents(RuleContext& ctx, BlockPos pos, const std::vector<ItemStack>& slots) {
    // Each slot's stack drops whole, at the block center, in slot order.
    for (const auto& s : slots)
        if (!s.empty()) spawn(ctx, pos, s);
}

void spawn_dropped(RuleContext& ctx, BlockPos cell, contracts::FixedVec position, contracts::FixedVec velocity, ItemStack stack,
                   std::uint16_t pickup_delay) {
    Entity e{{}, position, velocity, stack, 0, 0, false};
    e.pickup_delay = pickup_delay;
    const auto id = ctx.spawn_entity(e);
    ctx.record(TraceKind::entity_spawned, cell, id.value, contracts::pack_stack(stack));
}

} // namespace circuit::gameplay::detail
