// SPDX-License-Identifier: AGPL-3.0-only
#include "internal.hpp"
#include <algorithm>
#include <circuit/gameplay/game_data.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/survival.hpp>
#include <array>
#include <string_view>

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
    // A stack of none makes no item, after its position (loot_rolls).
    if (stack.count == 0) return;
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
    case BlockType::chest: return ItemStack{s.variant() == 1 ? items::trapped_chest : items::chest, 1};
    case BlockType::dispenser: return ItemStack{s.variant() == 1 ? items::dropper : items::dispenser, 1};
    case BlockType::crafter: return ItemStack{items::crafter, 1};
    case BlockType::brewing_stand: return ItemStack{items::brewing_stand, 1};
    case BlockType::lectern: return ItemStack{items::lectern, 1};
    case BlockType::barrel: return ItemStack{items::barrel, 1};
    case BlockType::furnace: {
        constexpr std::array<std::uint16_t, 3> kinds{items::furnace, items::smoker, items::blast_furnace};
        return ItemStack{kinds[s.variant() % 3], 1};
    }
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
// Drops of a block by its loot table, when the data packs have one the rules
// evaluate (nullopt otherwise). Rolls draw from the table's random sequence;
// one that needs a weighted choice drops nothing.
std::optional<std::vector<ItemStack>> table_drops(RuleContext& ctx, BlockPos pos, BlockState s, const ItemStack& tool, bool by_entity) {
    const auto* data = ctx.data();
    const auto state = data ? physics::registry_state_of(s) : std::nullopt;
    const auto block = state ? registry::block_of(*state) : std::nullopt;
    const auto table = block ? data->block_loot.find(*block) : decltype(data->block_loot.end()){};
    if (!block || table == data->block_loot.end()) return std::nullopt;
    loot::BreakContext context{registry::properties(*state), tool,
                               [&](std::uint32_t enchantment) { return data->enchantment_level ? data->enchantment_level(tool, enchantment) : 0L; },
                               nullptr, {}, by_entity};
    if (!table->second.sequence.empty()) context.random = &ctx.random_sequence(table->second.sequence, table->second.sequence_hash);
    // The block at an offset is the one named, with the properties given.
    context.location = [&](const loot::Condition& c) {
        const auto there = physics::registry_state_of(ctx.block({pos.x + c.dx, pos.y + c.dy, pos.z + c.dz}));
        if (!there || (c.block != UINT32_MAX && registry::block_of(*there) != c.block)) return false;
        const auto props = registry::properties(*there);
        return std::ranges::all_of(c.properties, [&](const loot::Condition::Property& p) {
            return std::ranges::any_of(props, [&](const auto& kv) { return kv.first == p.name && p.exact && kv.second == *p.exact; });
        });
    };
    auto drops = loot::drops(table->second, context);
    return drops ? std::move(*drops) : std::vector<ItemStack>{};
}
} // namespace

std::vector<ItemStack> block_drops(RuleContext& ctx, BlockPos pos, BlockState destroyed, const ItemStack& tool, bool exploded,
                                   bool by_entity) {
    // Wheat drops by its loot table when the data packs give one.
    if (destroyed.is(BlockType::wheat))
        if (auto drops = table_drops(ctx, pos, destroyed, tool, by_entity)) return std::move(*drops);
    if (destroyed.is(BlockType::wheat)) {
        std::vector<ItemStack> out;
        if (destroyed.level() >= 7) out.push_back({items::wheat, 1});
        out.push_back({items::wheat_seeds, 1});
        return out;
    }
    // An explosion needs no tool (stone gives cobblestone).
    if (!exploded && !harvests(destroyed, tool, ctx.data())) return {};
    if (destroyed.is(BlockType::other)) {
        if (auto drops = table_drops(ctx, pos, destroyed, tool, by_entity)) return std::move(*drops);
        // Without a table the rules evaluate (no data packs, or one they do not
        // compile yet), blocks that need support drop as their rules say.
        if (const auto own = supported_drop(destroyed)) return {*own};
        return {};
    }
    if (const auto item = own_item(destroyed)) return {*item};
    return {};
}

void pop_resource(RuleContext& ctx, BlockPos pos, ItemStack stack) { spawn(ctx, pos, std::move(stack)); }

// A destroyed block's drops.
void drop_items(RuleContext& ctx, BlockPos pos, BlockState destroyed, ItemStack tool, bool by_entity) {
    for (auto& d : block_drops(ctx, pos, destroyed, tool, false, by_entity)) spawn(ctx, pos, std::move(d));
}

void drop_experience(RuleContext& ctx, BlockPos pos, BlockState destroyed, const ItemStack& tool) {
    // minecraft.wiki "Experience": ores and the sculk family give a uniform
    // amount (one draw of the level random), sculk and its catalyst,
    // sensors and shrieker a constant one, a spawner 15 and two draws of 15;
    // checked against both official servers' traces of mined ores.
    if (!destroyed.is(BlockType::other)) return;
    const auto block = registry::block_of(destroyed.registry_state());
    if (!block) return;
    const auto name = registry::game_name(*block);
    struct Range {
        std::string_view block;
        std::int32_t low, high;
    };
    static constexpr std::array<Range, 18> ranges{{
        {"coal_ore", 0, 2}, {"deepslate_coal_ore", 0, 2}, {"diamond_ore", 3, 7}, {"deepslate_diamond_ore", 3, 7},
        {"emerald_ore", 3, 7}, {"deepslate_emerald_ore", 3, 7}, {"lapis_ore", 2, 5}, {"deepslate_lapis_ore", 2, 5},
        {"redstone_ore", 1, 5}, {"deepslate_redstone_ore", 1, 5}, {"nether_quartz_ore", 2, 5}, {"nether_gold_ore", 0, 1},
        {"sculk", 1, 1}, {"sculk_catalyst", 5, 5}, {"sculk_sensor", 5, 5}, {"calibrated_sculk_sensor", 5, 5},
        {"sculk_shrieker", 5, 5}, {"spawner", 15, 43}}};
    const auto it = std::ranges::find(ranges, name, &Range::block);
    if (it == ranges.end()) return;
    std::int32_t amount = it->low;
    if (it->block == "spawner") amount = 15 + ctx.random_int(15) + ctx.random_int(15);
    else if (it->high > it->low) amount = it->low + ctx.random_int(it->high - it->low + 1);
    // Silk Touch sets it to 0 (an enchantment's block_experience effect).
    if (const auto* data = ctx.data(); data && data->enchantment_level && !tool.empty())
        for (const auto& [enchantment, value] : data->block_experience_set)
            if (data->enchantment_level(tool, enchantment) > 0) amount = static_cast<std::int32_t>(value);
    if (amount > 0) award_experience(ctx, {pos.x + 0.5, pos.y + 0.5, pos.z + 0.5}, amount);
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
