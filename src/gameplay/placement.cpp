// SPDX-License-Identifier: AGPL-3.0-only
// Blocks that items place, as the server's placement used them since M4,
// oriented by the player and the clicked face.
#include "internal.hpp"

namespace circuit::gameplay::detail {
namespace {
constexpr std::array<std::uint16_t, blocks::button_kinds> button_items{
    items::oak_button, items::spruce_button, items::birch_button, items::jungle_button, items::acacia_button,
    items::cherry_button, items::dark_oak_button, items::pale_oak_button, items::mangrove_button, items::bamboo_button,
    items::crimson_button, items::warped_button, items::stone_button, items::polished_blackstone_button};
constexpr std::array<std::uint16_t, blocks::button_kinds> plate_items{
    items::oak_pressure_plate, items::spruce_pressure_plate, items::birch_pressure_plate, items::jungle_pressure_plate,
    items::acacia_pressure_plate, items::cherry_pressure_plate, items::dark_oak_pressure_plate, items::pale_oak_pressure_plate,
    items::mangrove_pressure_plate, items::bamboo_pressure_plate, items::crimson_pressure_plate, items::warped_pressure_plate,
    items::stone_pressure_plate, items::polished_blackstone_pressure_plate};
} // namespace

std::uint16_t button_item(BlockState s) { return button_items[s.variant() % blocks::button_kinds]; }
std::uint16_t plate_item(BlockState s) {
    if (s.is(BlockType::weighted_plate)) return s.variant() == 1 ? items::heavy_weighted_pressure_plate : items::light_weighted_pressure_plate;
    return plate_items[s.variant() % blocks::button_kinds];
}

std::optional<BlockState> placed_by(std::uint16_t item, Direction face, Direction horizontal, Direction look) {
    if (const auto opening = placed_opening_by(item, face, horizontal)) return opening;
    // Mounted on the clicked face as levers are.
    if (const auto it = std::ranges::find(button_items, item); it != button_items.end()) {
        const auto kind = static_cast<blocks::ButtonKind>(it - button_items.begin());
        if (face == Direction::up) return blocks::button(kind, false, horizontal, blocks::LeverFace::floor);
        if (face == Direction::down) return blocks::button(kind, false, horizontal, blocks::LeverFace::ceiling);
        return blocks::button(kind, false, face, blocks::LeverFace::wall);
    }
    if (const auto it = std::ranges::find(plate_items, item); it != plate_items.end())
        return blocks::pressure_plate(static_cast<blocks::ButtonKind>(it - plate_items.begin()));
    switch (item) {
    case items::stone: return blocks::stone();
    case items::glass: return blocks::glass();
    case items::obsidian: return blocks::obsidian();
    case items::redstone_block: return blocks::redstone_block();
    case items::slime_block: return blocks::slime();
    case items::redstone_lamp: return blocks::lamp();
    case items::redstone: return blocks::wire();
    case items::lever:
        // Mounted on the clicked face: floor and ceiling levers face the
        // player's way, wall levers point away from the wall.
        if (face == Direction::up) return blocks::lever(false, horizontal, blocks::LeverFace::floor);
        if (face == Direction::down) return blocks::lever(false, horizontal, blocks::LeverFace::ceiling);
        return blocks::lever(false, face, blocks::LeverFace::wall);
    case items::repeater: return blocks::repeater(horizontal);
    case items::comparator: return blocks::comparator(horizontal);
    case items::observer: return blocks::observer(contracts::opposite(look));
    case items::piston: return blocks::piston(contracts::opposite(look));
    case items::sticky_piston: return blocks::piston(contracts::opposite(look), true);
    case items::hopper:
        return blocks::hopper(face == Direction::up || face == Direction::down ? Direction::down : contracts::opposite(face));
    case items::chest: return blocks::chest(contracts::opposite(horizontal));
    case items::water_bucket: return blocks::water_source();
    case items::wheat_seeds: return blocks::wheat();
    // On the side of the clicked block, or standing (torches do not hang
    // under blocks; an approximation of the official choice by the
    // player's view).
    case items::redstone_torch: return contracts::is_horizontal(face) ? blocks::wall_torch(face) : blocks::torch();
    case items::light_weighted_pressure_plate: return blocks::weighted_plate(false);
    case items::heavy_weighted_pressure_plate: return blocks::weighted_plate(true);
    case items::target: return blocks::target();
    default: return std::nullopt;
    }
}

bool has_collision(BlockState s) {
    switch (s.type()) {
    case BlockType::air: case BlockType::redstone_wire: case BlockType::lever: case BlockType::wheat: case BlockType::water:
    case BlockType::redstone_torch: case BlockType::redstone_wall_torch: case BlockType::button:
    case BlockType::pressure_plate: case BlockType::weighted_plate:
        return false;
    case BlockType::other: return other_has_collision(s);
    default: return true;
    }
}

void record_slot(RuleContext& ctx, const PlayerInventory& inventory, std::uint8_t slot) {
    const auto stack = slot == contracts::player_slots ? inventory.cursor : inventory.slots[slot];
    ctx.record(TraceKind::player_slot_changed, {}, inventory.player.hi, inventory.player.lo,
               (static_cast<std::uint64_t>(slot) << 48) | contracts::pack_stack(stack));
}

// The inventory slots of the player's own window, in the target version's
// fill order for picked-up items: the selected hotbar slot and the off hand
// first when they hold the same item with room, then the hotbar and the main
// inventory; otherwise the first empty slot of the hotbar and main inventory.
std::uint8_t player_window_slot(std::size_t inventory_index) {
    return static_cast<std::uint8_t>(inventory_index < 9 ? contracts::hotbar_first + inventory_index : inventory_index);
}
int slot_with_room(const PlayerInventory& inv, ItemStack stack, std::uint8_t selected) {
    const auto room = [&](std::uint8_t slot) {
        const auto& s = inv.slots[slot];
        return !s.empty() && s.same_kind(stack) && s.count < items::max_stack(s);
    };
    if (room(selected)) return selected;
    if (room(contracts::offhand_slot)) return contracts::offhand_slot;
    for (std::size_t i = 0; i < 36; ++i)
        if (room(player_window_slot(i))) return player_window_slot(i);
    return -1;
}
int free_slot(const PlayerInventory& inv) {
    for (std::size_t i = 0; i < 36; ++i)
        if (inv.slots[player_window_slot(i)].empty()) return player_window_slot(i);
    return -1;
}
// Adds as much of `stack` as fits; returns what is left.
ItemStack add_to_player(RuleContext& ctx, PlayerInventory& inv, ItemStack stack, std::uint8_t selected) {
    if (stack.damage > 0) {
        // Damaged items take a free slot whole.
        if (const auto slot = free_slot(inv); slot >= 0) {
            inv.slots[static_cast<std::size_t>(slot)] = stack;
            record_slot(ctx, inv, static_cast<std::uint8_t>(slot));
            return {};
        }
        return stack;
    }
    while (!stack.empty()) {
        auto slot = slot_with_room(inv, stack, selected);
        if (slot < 0) slot = free_slot(inv);
        if (slot < 0) break;
        auto& s = inv.slots[static_cast<std::size_t>(slot)];
        if (s.empty()) {
            s = stack;
            s.count = 0;
        }
        const auto moved = std::min<int>(stack.count, items::max_stack(s) - s.count);
        s.count = static_cast<std::uint8_t>(s.count + moved);
        stack.count = static_cast<std::uint8_t>(stack.count - moved);
        record_slot(ctx, inv, static_cast<std::uint8_t>(slot));
    }
    if (stack.count == 0) stack = {};
    return stack;
}

bool player_has_room(const PlayerInventory& inv, ItemStack stack, std::uint8_t selected) {
    return free_slot(inv) >= 0 || (stack.damage == 0 && slot_with_room(inv, stack, selected) >= 0);
}

} // namespace circuit::gameplay::detail
