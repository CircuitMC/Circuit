// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
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
    case BlockType::honey_block: return ItemStack{items::honey_block, 1};
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
    case BlockType::dispenser: return ItemStack{s.variant() == 1 ? items::dropper : items::dispenser, 1};
    case BlockType::redstone_torch: case BlockType::redstone_wall_torch: return ItemStack{items::redstone_torch, 1};
    case BlockType::button: return ItemStack{button_item(s), 1};
    case BlockType::pressure_plate: case BlockType::weighted_plate: return ItemStack{plate_item(s), 1};
    case BlockType::target: return ItemStack{items::target, 1};
    case BlockType::daylight_detector: return ItemStack{items::daylight_detector, 1};
    case BlockType::note_block: return ItemStack{items::note_block, 1};
    case BlockType::rail: return ItemStack{rail_item(s), 1};
    // A door drops from its lower half.
    case BlockType::door:
        if (upper_half(s)) return std::nullopt;
        return ItemStack{opening_item(s), 1};
    case BlockType::trapdoor: case BlockType::fence_gate: return ItemStack{opening_item(s), 1};
    default: return std::nullopt;
    }
}
// Drops of a block without rules of its own: its loot table, when the data
// packs have one the rules evaluate. A roll that needs a random number (the
// table compiled, but the tool has fortune) drops nothing.
std::vector<ItemStack> table_drops(RuleContext& ctx, BlockState s, const ItemStack& tool) {
    const auto* data = ctx.data();
    const auto block = data ? registry::block_of(s.registry_state()) : std::nullopt;
    const auto table = block ? data->block_loot.find(*block) : decltype(data->block_loot.end()){};
    if (!block || table == data->block_loot.end()) return {};
    const loot::BreakContext context{registry::properties(s.registry_state()), tool, [&](std::uint32_t enchantment) {
                                         return data->enchantment_level ? data->enchantment_level(tool, enchantment) : 0L;
                                     }};
    auto drops = loot::drops(table->second, context);
    return drops ? std::move(*drops) : std::vector<ItemStack>{};
}
} // namespace

std::vector<ItemStack> block_drops(RuleContext& ctx, BlockState destroyed, const ItemStack& tool, bool exploded) {
    if (destroyed.is(BlockType::wheat)) {
        std::vector<ItemStack> out;
        if (destroyed.level() >= 7) out.push_back({items::wheat, 1});
        out.push_back({items::wheat_seeds, 1});
        return out;
    }
    // An explosion needs no tool (stone gives cobblestone).
    if (!exploded && !harvests(destroyed, tool, ctx.data())) return {};
    if (destroyed.is(BlockType::other)) {
        // Blocks that need support drop themselves when no loot table says otherwise.
        if (!ctx.data())
            if (const auto own = supported_drop(destroyed)) return {*own};
        return table_drops(ctx, destroyed, tool);
    }
    if (const auto item = own_item(destroyed)) return {*item};
    return {};
}

void pop_resource(RuleContext& ctx, BlockPos pos, ItemStack stack) { spawn(ctx, pos, std::move(stack)); }

// Drops without random quantities.
void drop_items(RuleContext& ctx, BlockPos pos, BlockState destroyed, ItemStack tool) {
    for (auto& d : block_drops(ctx, destroyed, tool, false)) spawn(ctx, pos, std::move(d));
}

void drop_contents(RuleContext& ctx, BlockPos pos, const std::vector<ItemStack>& slots) {
    // As both official servers scatter a removed chest (fitted to their
    // traces, every value exact): each slot, empty or not, draws a point in
    // the cell, 0.125 to 0.875 across and 0 to 0.75 up; its stack leaves in
    // parts of 10 to 30 items. Each part draws the velocity every new item
    // draws (as a block drop's), then gets a velocity around (0, 0.2, 0) that
    // differs from it by the difference of two draws times a spread on each
    // axis. The spread is the only double that gives every recorded velocity
    // exactly. Parts can be picked up at once.
    constexpr double spread = 0.11485000171139836;
    const auto triangle = [&](double mode) {
        const double a = ctx.random_double();
        const double b = ctx.random_double();
        return mode + spread * (a - b);
    };
    for (const auto& slot : slots) {
        const double x = static_cast<double>(pos.x) + ctx.random_double() * 0.75 + 0.125;
        const double y = static_cast<double>(pos.y) + ctx.random_double() * 0.75;
        const double z = static_cast<double>(pos.z) + ctx.random_double() * 0.75 + 0.125;
        auto rest = slot;
        while (!rest.empty()) {
            const auto part = static_cast<std::uint8_t>(std::min<std::int32_t>(rest.count, ctx.random_int(21) + 10));
            Entity e;
            e.position = {x, y, z};
            e.stack = rest;
            e.stack.count = part;
            rest.count = static_cast<std::uint8_t>(rest.count - part);
            (void)ctx.random_double(); // the new item's own velocity, replaced below
            (void)ctx.random_double();
            const double vx = triangle(0.0);
            const double vy = triangle(0.2);
            const double vz = triangle(0.0);
            e.velocity = {vx, vy, vz};
            record_spawn(ctx, ctx.spawn_entity(e), e);
        }
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
    ctx.record(TraceKind::entity_spawned, cell, id.value, entity_payload(e), static_cast<std::uint64_t>(e.kind));
    ctx.record(TraceKind::entity_spawn_position, cell, contracts::double_bits(e.position.x), contracts::double_bits(e.position.y),
               contracts::double_bits(e.position.z));
    ctx.record(TraceKind::entity_spawn_velocity, cell, contracts::double_bits(e.velocity.x), contracts::double_bits(e.velocity.y),
               contracts::double_bits(e.velocity.z));
}

} // namespace circuit::gameplay::detail
