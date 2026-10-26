// SPDX-License-Identifier: AGPL-3.0-only
// Blocks that items place, as the server's placement used them since M4,
// oriented by the player and the clicked face.
#include "internal.hpp"
#include <algorithm>
#include <cmath>
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
    // A crafter's front faces the player; its top the way the player faces
    // when looking down at it, the other way when looking up (tests/fixtures/placement).
    case items::crafter: {
        const auto front = contracts::opposite(look);
        const auto top = front == Direction::up ? horizontal : front == Direction::down ? contracts::opposite(horizontal) : Direction::up;
        return blocks::crafter(front, top);
    }
    // Furnaces face the player (tests/fixtures/placement).
    case items::furnace: return blocks::furnace(blocks::FurnaceKind::furnace, contracts::opposite(horizontal));
    case items::smoker: return blocks::furnace(blocks::FurnaceKind::smoker, contracts::opposite(horizontal));
    case items::blast_furnace: return blocks::furnace(blocks::FurnaceKind::blast_furnace, contracts::opposite(horizontal));
    case items::hopper:
        return blocks::hopper(face == Direction::up || face == Direction::down ? Direction::down : contracts::opposite(face));
    case items::chest: return blocks::chest(contracts::opposite(horizontal));
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
    const auto stack = slot == contracts::player_slots ? inventory.cursor
                     : slot >= contracts::grind_first  ? inventory.grind[slot - contracts::grind_first]
                     : slot >= contracts::smith_first  ? inventory.smith[slot - contracts::smith_first]
                     : slot >= contracts::cutter_first ? inventory.cutter[slot - contracts::cutter_first]
                     : slot >= contracts::table_first  ? inventory.table[slot - contracts::table_first]
                                                       : inventory.slots[slot];
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
enum class Building {
    none, plain, pillar, stairs, slab, connector, toward_player, toward_look, clicked_face, fixed, torch, lantern, ladder, resting, tall,
    sign, head, bed, snowy, mounted
};

Building building_kind(std::uint32_t block) {
    const auto info = registry::block(block);
    const std::string_view name = info.name;
    // Not spawners (their block entity), sponges (they soak up water), or
    // test instance blocks (operators' blocks).
    if (name == "spawner" || name == "sponge" || name == "wet_sponge" || name == "test_instance_block") return Building::none;
    std::vector<std::string> keys;
    for (const auto& [k, _] : registry::properties(info.default_state)) keys.push_back(k);
    const auto only = [&](std::initializer_list<std::string_view> allowed) {
        return std::ranges::all_of(keys, [&](const std::string& k) { return std::ranges::find(allowed, k) != allowed.end(); });
    };
    const auto has = [&](std::string_view k) { return std::ranges::find(keys, k) != keys.end(); };
    if (name.ends_with("_stairs") && has("shape")) return Building::stairs;
    // Blocks that face the player, look back at them, or face the clicked face,
    // and blocks placed as they are (those with block entities keep an empty
    // one).
    if (name == "carved_pumpkin" || name == "jack_o_lantern" || name == "loom" || name == "stonecutter" || name == "end_portal_frame"
        || name.ends_with("_glazed_terracotta") || name == "furnace" || name == "smoker" || name == "blast_furnace" || name == "lectern"
        || name == "bee_nest" || name == "beehive" || name == "ender_chest" || name == "chiseled_bookshelf")
        return Building::toward_player;
    if (name == "barrel") return Building::toward_look;
    // Mounted on the clicked face as levers are, without needing support.
    if (name == "grindstone") return Building::mounted;
    if (name == "end_rod" || name == "lightning_rod" || name.ends_with("shulker_box")) return Building::clicked_face;
    if (name == "beacon" || name == "enchanting_table" || name == "jukebox" || name == "brewing_stand") return Building::fixed;
    // Blocks that need support (support.cpp).
    if (name == "torch" || name == "soul_torch") return Building::torch;
    if (name == "lantern" || name == "soul_lantern") return Building::lantern;
    if (name == "ladder") return Building::ladder;
    if (name == "sunflower" || name == "lilac" || name == "rose_bush" || name == "peony" || name == "tall_grass" || name == "large_fern")
        return Building::tall;
    if ((name.ends_with("_sign") && !name.ends_with("hanging_sign") && !name.ends_with("_wall_sign"))
        || (name.ends_with("_banner") && !name.ends_with("_wall_banner")))
        return Building::sign;
    if ((name.ends_with("_head") || name.ends_with("_skull")) && !name.contains("_wall_") && name != "piston_head") return Building::head;
    if (name.ends_with("_bed")) return Building::bed;
    // A lily pad is placed on water by using it on the water, not on a block's
    // face; small dripleaves (two halves) and dripleaf stems are not placed yet.
    if (name == "lily_pad" || name == "small_dripleaf" || name == "big_dripleaf_stem") return Building::none;
    if (needs_support(BlockState::registered(info.default_state))) return Building::resting;
    if (name.ends_with("_slab") && has("type") && only({"type", "waterlogged"})) return Building::slab;
    if (registry::connector(info.default_state) != registry::Connector::none) return Building::connector;
    if (has("axis") && only({"axis", "waterlogged"})) return Building::pillar;
    // Grass blocks, podzol and mycelium, snowy under snow.
    if (has("snowy") && only({"snowy"})) return Building::snowy;
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

// Sixteen steps of the player's yaw (0 south): a head's rotation; a sign or
// banner faces back at the player, half a turn on.
std::string rotation_of(float yaw) {
    return std::to_string(static_cast<int>(std::floor(yaw * 16.0f / 360.0f + 0.5f)) & 15);
}

// The wall variant of a standing sign, banner or head: "oak_sign" to
// "oak_wall_sign".
std::optional<std::uint32_t> wall_variant(RuleContext& ctx, std::string_view name) {
    const auto cut = name.rfind('_');
    if (cut == std::string_view::npos) return std::nullopt;
    const auto wall = std::string(name.substr(0, cut)) + "_wall" + std::string(name.substr(cut));
    return registry::find_block(ctx.bundle().schema.content_release, wall);
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
    // Bamboo is planted as a bamboo shoot, glow berries as cave vines
    // (tests/fixtures/placement).
    const auto name = info->name == "minecraft:bamboo" ? "bamboo_sapling" : info->name == "minecraft:glow_berries" ? "cave_vines" : info->name;
    const auto block = registry::find_block(ctx.bundle().schema.content_release, name);
    if (!block || !BlockState::decode(BlockState::registered(registry::block(*block).default_state).raw())) return std::nullopt;
    if (building_kind(*block) == Building::none) return std::nullopt;
    return block;
}

std::optional<std::pair<BlockPos, BlockState>> second_part(BlockState placed, BlockPos pos) {
    if (!placed.is(BlockType::other)) return std::nullopt;
    const auto block = registry::block_of(placed.registry_state());
    if (!block) return std::nullopt;
    if (building_kind(*block) == Building::tall) return std::pair{pos.relative(Direction::up), with(placed, {{"half", "upper"}})};
    if (building_kind(*block) == Building::bed) {
        const auto f = value_of(placed, "facing");
        const auto facing = f == "north" ? Direction::north : f == "south" ? Direction::south : f == "west" ? Direction::west : Direction::east;
        return std::pair{pos.relative(facing), with(placed, {{"part", "head"}})};
    }
    return std::nullopt;
}

namespace {
// The number of layers of a snow layer; 0 for other blocks.
int snow_layers(BlockState s) {
    const auto at = s.is(BlockType::other) ? registry::block_of(s.registry_state()) : std::nullopt;
    return at && registry::game_name(*at) == "snow" ? std::stoi(value_of(s, "layers")) : 0;
}
} // namespace

bool single_snow_layer(BlockState s) { return snow_layers(s) == 1; }

bool snow_replaced(std::uint32_t block, BlockState s, Direction face, bool clicked) {
    const int layers = snow_layers(s);
    if (layers == 0) return false;
    if (registry::game_name(block) != "snow") return layers == 1;
    return layers < 8 && (!clicked || face == Direction::up);
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
    // Snow placed into snow is a layer more.
    if (registry::game_name(block) == "snow" && snow_replaced(block, here, c.face, false))
        return with(here, {{"layers", std::to_string(std::stoi(value_of(here, "layers")) + 1)}});
    const std::string water = here == blocks::water_source() ? "true" : "false";
    const auto stays = [&](BlockState placed) -> std::optional<BlockState> {
        if (supported_at(ctx, target, placed)) return placed;
        return std::nullopt;
    };
    switch (building_kind(block)) {
    // Snowy under a snow layer, a snow block or powder snow.
    case Building::snowy: {
        const auto above = ctx.block(target.relative(Direction::up));
        const auto block_above = above.is(BlockType::other) ? registry::block_of(above.registry_state()) : std::nullopt;
        const auto n = block_above ? registry::game_name(*block_above) : std::string_view{};
        return with(s, {{"snowy", n == "snow" || n == "snow_block" || n == "powder_snow" ? "true" : "false"}});
    }
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
    // Carpets, flowers and saplings rest on what is below; a tall flower's
    // lower half too (its upper half goes above it, which must be free).
    // Petals and wildflowers face the player; a mangrove propagule is set
    // grown (tests/fixtures/placement).
    case Building::resting: {
        const auto block_name = registry::block(block).name;
        if (block_name == "pink_petals" || block_name == "wildflowers") s = with(s, {{"facing", std::string(name_of(contracts::opposite(c.horizontal)))}});
        if (block_name == "mangrove_propagule") s = with(s, {{"age", "4"}});
        if (block_name == "sea_pickle") s = with(s, {{"waterlogged", water}});
        // A big dripleaf faces the player (tests/fixtures/placement).
        if (block_name == "big_dripleaf")
            s = with(s, {{"facing", std::string(name_of(contracts::opposite(c.horizontal)))}, {"waterlogged", water}});
        // A vine is placed as a body where its kind grows on from it, else as
        // a head of a random age (a level-random draw); kelp alike, but only
        // into a water source (tests/fixtures/placement).
        if (block_name == "kelp" && water != "true") return std::nullopt;
        if (block_name == "weeping_vines" || block_name == "twisting_vines" || block_name == "cave_vines" || block_name == "kelp") {
            const auto growth = block_name == "twisting_vines" || block_name == "kelp" ? Direction::up : Direction::down;
            const auto next = ctx.block(target.relative(growth));
            const auto body = registry::find_block(ctx.bundle().schema.content_release, std::string(block_name) + "_plant");
            const bool grows_on = next.is(BlockType::other) && (registry::block_of(next.registry_state()) == block
                                                                || registry::block_of(next.registry_state()) == body);
            if (grows_on) return stays(BlockState::registered(registry::block(*body).default_state));
            // The age is drawn before the vine is found unsupported (whether a
            // refused placement draws is not observed).
            return stays(with(s, {{"age", std::to_string(ctx.random_int(25))}}));
        }
        return stays(s);
    }
    case Building::tall: return stays(with(s, {{"half", "lower"}}));
    // Signs and banners hang on the clicked side, else stand turned to the
    // player; heads alike, but stand turned away from them.
    case Building::sign: case Building::head: {
        const bool sign = building_kind(block) == Building::sign;
        if (contracts::is_horizontal(c.face))
            if (const auto wall = wall_variant(ctx, registry::block(block).name)) {
                const auto hung = with(BlockState::registered(registry::block(*wall).default_state),
                                       {{"facing", std::string(name_of(c.face))}, {"waterlogged", water}});
                if (!sign) return hung;
                if (const auto placed = stays(hung)) return placed;
            }
        const auto standing = with(s, {{"rotation", rotation_of(sign ? c.yaw + 180.0f : c.yaw)}, {"waterlogged", water}});
        return sign ? stays(standing) : std::optional(standing);
    }
    // A bed's foot faces the player's way; its head goes ahead of it.
    case Building::bed: return with(s, {{"facing", std::string(name_of(c.horizontal))}, {"part", "foot"}});
    case Building::pillar: return with(s, {{"axis", std::string(axis_of(c.face))}, {"waterlogged", water}});
    // Floor and ceiling ones face the player's way, wall ones away from the
    // wall (tests/fixtures/placement).
    case Building::mounted:
        if (c.face == Direction::up) return with(s, {{"face", "floor"}, {"facing", std::string(name_of(c.horizontal))}});
        if (c.face == Direction::down) return with(s, {{"face", "ceiling"}, {"facing", std::string(name_of(c.horizontal))}});
        return with(s, {{"face", "wall"}, {"facing", std::string(name_of(c.face))}});
    case Building::stairs:
        s = with(s, {{"facing", std::string(name_of(c.horizontal))}, {"half", top_half(c) ? "top" : "bottom"}, {"waterlogged", water}});
        return shaped_stairs(ctx, target, s);
    case Building::slab:
        if (single_slab_of(block, here)) return with(s, {{"type", "double"}, {"waterlogged", "false"}});
        return with(s, {{"type", top_half(c) ? "top" : "bottom"}, {"waterlogged", water}});
    case Building::connector: return joined_state(ctx, target, with(s, {{"waterlogged", water}}));
    case Building::toward_player: return with(s, {{"facing", std::string(name_of(contracts::opposite(c.horizontal)))}, {"waterlogged", water}});
    case Building::toward_look: return with(s, {{"facing", std::string(name_of(contracts::opposite(c.look)))}});
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
