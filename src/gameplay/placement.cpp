// SPDX-License-Identifier: AGPL-3.0-only
// Blocks that items place, as the server's placement used them since M4,
// oriented by the player and the clicked face.
#include "internal.hpp"
#include <algorithm>
#include <string>
#include <vector>

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

std::optional<BlockState> placed_by(std::uint16_t item, const contracts::PlayerUse& use) {
    const auto face = use.face, horizontal = use.horizontal, look = use.look;
    if (const auto opening = placed_opening_by(item, face, horizontal, use.cursor_y > 0.5f)) return opening;
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
    case items::honey_block: return blocks::honey();
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
    // Its face looks the player's way (both official servers; tests/fixtures/placement).
    case items::observer: return blocks::observer(look);
    case items::piston: return blocks::piston(contracts::opposite(look));
    case items::sticky_piston: return blocks::piston(contracts::opposite(look), true);
    case items::dispenser: return blocks::dispenser(contracts::opposite(look));
    case items::dropper: return blocks::dispenser(contracts::opposite(look), true);
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
    case items::daylight_detector: return blocks::daylight_detector();
    case items::note_block: return blocks::note_block();
    // Along the player's view.
    case items::rail: case items::powered_rail: case items::activator_rail: case items::detector_rail: {
        const auto kind = item == items::rail ? blocks::RailKind::rail : item == items::powered_rail ? blocks::RailKind::powered
                        : item == items::activator_rail ? blocks::RailKind::activator : blocks::RailKind::detector;
        const bool east_west = horizontal == Direction::east || horizontal == Direction::west;
        return blocks::rail(kind, east_west ? blocks::RailShape::east_west : blocks::RailShape::north_south);
    }
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

// ---- building blocks without rules of their own ----
//
// A block item places the block of its name, as both official servers do
// (compared through a protocol client and with a report of every block item
// in fifteen contexts, tests/fixtures/placement): a full cube without
// properties (not spawners, beacons, sponges or test instance blocks), soul
// sand and mud as they are; a pillar along the clicked face's axis; carved
// pumpkins, jack o'lanterns, glazed terracotta, looms, stonecutters and end
// portal frames facing the player; end rods and lightning rods pointing out of
// the clicked face (an end rod set on one pointing at it points back); stairs
// facing the player's way; stairs and a
// single slab in the top half when the click is on a bottom face or in the
// upper half of a side, the bottom half otherwise; fences, panes, bars, walls
// and stairs with the shapes their neighbors give them; waterlogged in a
// water source. A slab clicked on its free half (the top face of a bottom
// slab, the bottom face of a top one, the other half of a side), or placed
// into a slab of its kind, doubles it. Torches go on the side of the clicked
// block or stand; lanterns hang under a clicked bottom or stand; ladders hang
// on a clicked side; carpets, flowers and saplings rest on what is below;
// each only where it is supported (support.cpp), replacing water.
namespace circuit::gameplay::detail {
namespace {
enum class Building { none, plain, pillar, stairs, slab, connector, toward_player, clicked_face, torch, lantern, ladder, resting };

Building building_kind(std::uint32_t block) {
    const auto info = registry::block(block);
    const std::string_view name = info.name;
    // Not spawners and beacons (their block entities), sponges (they soak up
    // water), or test instance blocks (operators' blocks).
    if (name == "spawner" || name == "beacon" || name == "sponge" || name == "wet_sponge" || name == "test_instance_block") return Building::none;
    std::vector<std::string> keys;
    for (const auto& [k, _] : registry::properties(info.default_state)) keys.push_back(k);
    const auto only = [&](std::initializer_list<std::string_view> allowed) {
        return std::ranges::all_of(keys, [&](const std::string& k) { return std::ranges::find(allowed, k) != allowed.end(); });
    };
    const auto has = [&](std::string_view k) { return std::ranges::find(keys, k) != keys.end(); };
    if (name.ends_with("_stairs") && has("shape")) return Building::stairs;
    // Blocks without block entities that face the player, or the clicked face.
    if (name == "carved_pumpkin" || name == "jack_o_lantern" || name == "loom" || name == "stonecutter" || name == "end_portal_frame"
        || name.ends_with("_glazed_terracotta"))
        return Building::toward_player;
    if (name == "end_rod" || name == "lightning_rod") return Building::clicked_face;
    // Blocks that need support (support.cpp).
    if (name == "torch" || name == "soul_torch") return Building::torch;
    if (name == "lantern" || name == "soul_lantern") return Building::lantern;
    if (name == "ladder") return Building::ladder;
    if (needs_support(BlockState::registered(info.default_state))) return Building::resting;
    if (name.ends_with("_slab") && has("type") && only({"type", "waterlogged"})) return Building::slab;
    if (registry::connector(info.default_state) != registry::Connector::none) return Building::connector;
    if (has("axis") && only({"axis", "waterlogged"})) return Building::pillar;
    if (keys.empty() && (registry::collision(info.default_state) == registry::Collision::full || name == "soul_sand" || name == "mud"))
        return Building::plain;
    return Building::none;
}

std::string value_of(BlockState s, std::string_view key) {
    for (const auto& [k, v] : registry::properties(s.registry_state()))
        if (k == key) return v;
    return {};
}

BlockState with(BlockState s, const std::vector<registry::Property>& changes) {
    auto props = registry::properties(s.registry_state());
    for (auto& [k, v] : props)
        for (const auto& [ck, cv] : changes)
            if (k == ck) v = cv;
    const auto block = registry::block_of(s.registry_state());
    const auto state = block ? registry::state_of(*block, props) : std::nullopt;
    return state ? BlockState::registered(*state) : s;
}

bool single_slab_of(std::uint32_t block, BlockState s) {
    return s.is(BlockType::other) && registry::block_of(s.registry_state()) == block && value_of(s, "type") != "double";
}

// The upper half: a click on a bottom face or in the upper half of a side.
bool top_half(const contracts::PlayerUse& c) {
    return c.face == Direction::down || (contracts::is_horizontal(c.face) && c.cursor_y > 0.5f);
}

std::string_view axis_of(Direction d) {
    switch (d) {
    case Direction::down: case Direction::up: return "y";
    case Direction::north: case Direction::south: return "z";
    default: return "x";
    }
}

std::string_view name_of(Direction d) {
    switch (d) {
    case Direction::down: return "down";
    case Direction::up: return "up";
    case Direction::north: return "north";
    case Direction::south: return "south";
    case Direction::west: return "west";
    default: return "east";
    }
}
} // namespace

std::optional<std::uint32_t> building_block(RuleContext& ctx, std::uint16_t item) {
    const auto* info = items::info(item);
    if (!info) return std::nullopt;
    const auto block = registry::find_block(ctx.bundle().schema.content_release, info->name);
    if (!block || !BlockState::decode(BlockState::registered(registry::block(*block).default_state).raw())) return std::nullopt;
    if (building_kind(*block) == Building::none) return std::nullopt;
    return block;
}

bool doubles_slab(std::uint32_t block, BlockState s, const contracts::PlayerUse& c, bool clicked) {
    if (building_kind(block) != Building::slab || !single_slab_of(block, s)) return false;
    if (!clicked) return true;
    const bool bottom = value_of(s, "type") == "bottom";
    const bool horizontal = contracts::is_horizontal(c.face);
    return bottom ? c.face == Direction::up || (horizontal && c.cursor_y > 0.5f) : c.face == Direction::down || (horizontal && c.cursor_y <= 0.5f);
}

std::optional<BlockState> placed_building(RuleContext& ctx, std::uint32_t block, const contracts::PlayerUse& c, BlockPos target) {
    const auto here = ctx.block(target);
    auto s = BlockState::registered(registry::block(block).default_state);
    const std::string water = here == blocks::water_source() ? "true" : "false";
    const auto stays = [&](BlockState placed) -> std::optional<BlockState> {
        if (supported_at(ctx, target, placed)) return placed;
        return std::nullopt;
    };
    switch (building_kind(block)) {
    // A torch goes on the side of the clicked block, else stands.
    case Building::torch: {
        if (contracts::is_horizontal(c.face)) {
            const auto wall = registry::find_block(ctx.bundle().schema.content_release,
                                                   registry::block(block).name == "torch" ? "wall_torch" : "soul_wall_torch");
            if (wall)
                if (const auto hung = stays(with(BlockState::registered(registry::block(*wall).default_state),
                                                 {{"facing", std::string(name_of(c.face))}})))
                    return hung;
        }
        return stays(s);
    }
    // A lantern hangs when the bottom of a block was clicked, else stands;
    // when it cannot, the other way.
    case Building::lantern: {
        const auto standing = with(s, {{"hanging", "false"}, {"waterlogged", water}});
        const auto hanging = with(s, {{"hanging", "true"}, {"waterlogged", water}});
        const auto first = c.face == Direction::down ? hanging : standing;
        if (const auto placed = stays(first)) return placed;
        return stays(first == hanging ? standing : hanging);
    }
    // A ladder hangs on the side of the clicked block.
    case Building::ladder:
        if (!contracts::is_horizontal(c.face)) return std::nullopt;
        return stays(with(s, {{"facing", std::string(name_of(c.face))}, {"waterlogged", water}}));
    // Carpets, flowers and saplings rest on what is below.
    case Building::resting: return stays(s);
    case Building::pillar: return with(s, {{"axis", std::string(axis_of(c.face))}, {"waterlogged", water}});
    case Building::stairs:
        s = with(s, {{"facing", std::string(name_of(c.horizontal))}, {"half", top_half(c) ? "top" : "bottom"}, {"waterlogged", water}});
        return shaped_stairs(ctx, target, s);
    case Building::slab:
        if (single_slab_of(block, here)) return with(s, {{"type", "double"}, {"waterlogged", "false"}});
        return with(s, {{"type", top_half(c) ? "top" : "bottom"}, {"waterlogged", water}});
    case Building::connector: return joined_state(ctx, target, with(s, {{"waterlogged", water}}));
    case Building::toward_player: return with(s, {{"facing", std::string(name_of(contracts::opposite(c.horizontal)))}});
    case Building::clicked_face: {
        // An end rod set on an end rod pointing at it points back.
        const auto behind = ctx.block(target.relative(contracts::opposite(c.face)));
        const bool back = registry::block(block).name == "end_rod" && behind.is(BlockType::other)
                       && registry::block_of(behind.registry_state()) == block && value_of(behind, "facing") == name_of(c.face);
        return with(s, {{"facing", std::string(name_of(back ? contracts::opposite(c.face) : c.face))}, {"waterlogged", water}});
    }
    default: return s;
    }
}

} // namespace circuit::gameplay::detail
