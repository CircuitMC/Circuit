// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/survival.hpp>

namespace circuit::gameplay::detail {
namespace {
// Items dropped by blocks wait 10 ticks before players can pick them up.
constexpr std::uint16_t block_drop_delay = 10;

// A block's drop, as both official servers place it: within 0.25 of the
// block's center on each axis (lowered by half an item's height), moving 0.2
// up and up to 0.1 sideways, all drawn from the level random in that order
// (checked against their traces).
void spawn(RuleContext& ctx, BlockPos pos, ItemStack stack, std::uint16_t pickup_delay = block_drop_delay) {
    const auto offset = [&] { return -0.25 + ctx.random_double() * 0.5; };
    const double x = static_cast<double>(pos.x) + 0.5 + offset();
    const double y = static_cast<double>(pos.y) + 0.5 + offset() - 0.125;
    const double z = static_cast<double>(pos.z) + 0.5 + offset();
    const double vx = ctx.random_double() * 0.2 - 0.1;
    const double vz = ctx.random_double() * 0.2 - 0.1;
    Entity e;
    e.position = {x, y, z};
    e.velocity = {vx, 0.2, vz};
    e.stack = stack;
    e.pickup_delay = pickup_delay;
    record_spawn(ctx, ctx.spawn_entity(e), e);
}

// The block's own item when a block breaks; nothing for glass, water and piston parts.
std::optional<ItemStack> own_item(BlockState s) {
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
// Drops of a block without rules of its own: its loot table, when the data
// packs have one the rules evaluate. A roll that needs a random number (the
// table compiled, but the tool has fortune) drops nothing.
void table_drops(RuleContext& ctx, BlockPos pos, BlockState s, const ItemStack& tool) {
    const auto* data = ctx.data();
    const auto block = data ? registry::block_of(s.registry_state()) : std::nullopt;
    const auto table = block ? data->block_loot.find(*block) : decltype(data->block_loot.end()){};
    if (!block || table == data->block_loot.end()) return;
    const loot::BreakContext context{registry::properties(s.registry_state()), tool, [&](std::uint32_t enchantment) {
                                         return data->enchantment_level ? data->enchantment_level(tool, enchantment) : 0L;
                                     }};
    if (const auto drops = loot::drops(table->second, context))
        for (const auto& d : *drops) spawn(ctx, pos, d);
}
} // namespace

// Drops without random quantities.
void drop_items(RuleContext& ctx, BlockPos pos, BlockState destroyed, ItemStack tool) {
    if (destroyed.is(BlockType::wheat)) {
        if (destroyed.level() >= 7) spawn(ctx, pos, {items::wheat, 1});
        spawn(ctx, pos, {items::wheat_seeds, 1});
        return;
    }
    if (!harvests(destroyed, tool, ctx.data())) return;
    if (destroyed.is(BlockType::other)) return table_drops(ctx, pos, destroyed, tool);
    if (const auto item = own_item(destroyed)) spawn(ctx, pos, *item);
}

void drop_contents(RuleContext& ctx, BlockPos pos, const std::vector<ItemStack>& slots) {
    // Each slot's stack drops whole, at the block center, still, in slot
    // order (the target versions split stacks and scatter them at random).
    for (const auto& s : slots) {
        if (s.empty()) continue;
        Entity e;
        e.position = {static_cast<double>(pos.x) + 0.5, static_cast<double>(pos.y) + 0.5, static_cast<double>(pos.z) + 0.5};
        e.stack = s;
        e.pickup_delay = block_drop_delay;
        record_spawn(ctx, ctx.spawn_entity(e), e);
    }
}

void spawn_dropped(RuleContext& ctx, BlockPos cell, contracts::Vec3 position, contracts::Vec3 velocity, ItemStack stack,
                   std::uint16_t pickup_delay) {
    (void)cell;
    Entity e;
    e.position = position;
    e.velocity = velocity;
    e.stack = stack;
    e.pickup_delay = pickup_delay;
    record_spawn(ctx, ctx.spawn_entity(e), e);
}

void record_spawn(RuleContext& ctx, EntityId id, const Entity& e) {
    const auto cell = contracts::block_of(e.position);
    ctx.record(TraceKind::entity_spawned, cell, id.value, contracts::pack_stack(e.stack), static_cast<std::uint64_t>(e.kind));
    ctx.record(TraceKind::entity_spawn_position, cell, contracts::double_bits(e.position.x), contracts::double_bits(e.position.y),
               contracts::double_bits(e.position.z));
    ctx.record(TraceKind::entity_spawn_velocity, cell, contracts::double_bits(e.velocity.x), contracts::double_bits(e.velocity.y),
               contracts::double_bits(e.velocity.z));
}

} // namespace circuit::gameplay::detail
