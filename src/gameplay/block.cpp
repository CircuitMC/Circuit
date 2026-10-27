// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/gameplay/block.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <array>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace circuit::gameplay {
namespace {
// Registry blocks with a BlockType of their own (see evidence::ContentMapping),
// besides the buttons and pressure plates of each kind.
constexpr std::array<std::string_view, 42> modeled_names{
    "air", "cave_air", "void_air", "stone", "glass", "obsidian", "redstone_block", "slime_block", "lever", "redstone_wire",
    "repeater", "redstone_lamp", "observer", "piston", "sticky_piston", "piston_head", "moving_piston", "hopper", "chest",
    "comparator", "water", "wheat", "redstone_torch", "redstone_wall_torch", "light_weighted_pressure_plate",
    "heavy_weighted_pressure_plate", "target", "honey_block", "daylight_detector", "note_block", "dispenser", "dropper", "furnace", "smoker",
    "blast_furnace", "crafter", "brewing_stand", "lectern", "trapped_chest", "barrel", "composter", "ender_chest"};
constexpr std::array<std::string_view, note_instruments> instrument_names{
    "harp", "basedrum", "snare", "hat", "bass", "flute", "bell", "guitar", "chime", "xylophone", "iron_xylophone", "cow_bell",
    "didgeridoo", "bit", "banjo", "pling", "zombie", "skeleton", "creeper", "dragon", "wither_skeleton", "piglin", "custom_head"};
constexpr std::array<std::string_view, blocks::button_kinds> button_names{
    "oak", "spruce", "birch", "jungle", "acacia", "cherry", "dark_oak", "pale_oak", "mangrove", "bamboo", "crimson", "warped",
    "stone", "polished_blackstone"};
constexpr std::array<std::string_view, blocks::door_kinds> door_names{
    "oak", "spruce", "birch", "jungle", "acacia", "cherry", "dark_oak", "pale_oak", "mangrove", "bamboo", "crimson", "warped",
    "iron", "copper", "exposed_copper", "weathered_copper", "oxidized_copper", "waxed_copper", "waxed_exposed_copper",
    "waxed_weathered_copper", "waxed_oxidized_copper"};

struct Properties {
    bool facing, level, flag, delay;
};
constexpr Properties properties(BlockType t) noexcept {
    switch (t) {
    case BlockType::air: return {false, true, false, false};
    case BlockType::lever: return {true, true, true, false};
    case BlockType::redstone_lamp: return {false, false, true, false};
    case BlockType::redstone_wire: case BlockType::wheat: return {false, true, false, false};
    case BlockType::repeater: return {true, true, true, true};
    case BlockType::observer: case BlockType::piston: case BlockType::sticky_piston:
    case BlockType::hopper: case BlockType::moving_piston:
        return {true, false, true, false};
    case BlockType::piston_head: return {true, true, true, false};
    case BlockType::water: return {false, true, true, false};
    case BlockType::chest: return {true, true, true, false};
    case BlockType::comparator: return {true, true, true, false};
    case BlockType::redstone_torch: return {false, false, true, false};
    case BlockType::redstone_wall_torch: return {true, false, true, false};
    case BlockType::button: return {true, true, true, false};
    case BlockType::pressure_plate: return {false, false, true, false};
    case BlockType::weighted_plate: case BlockType::target: return {false, true, false, false};
    case BlockType::door: case BlockType::trapdoor: case BlockType::fence_gate: return {true, true, true, true};
    case BlockType::daylight_detector: return {false, true, true, false};
    case BlockType::note_block: return {false, true, true, true};
    case BlockType::rail: return {false, true, true, true};
    case BlockType::coral_fan: return {false, false, true, false};
    case BlockType::coral_wall_fan: return {true, false, true, false};
    case BlockType::dispenser: case BlockType::furnace: return {true, false, true, false};
    case BlockType::crafter: return {true, false, true, true};
    case BlockType::brewing_stand: return {false, true, false, false};
    case BlockType::lectern: return {true, false, true, true};
    case BlockType::barrel: return {true, false, true, false};
    case BlockType::composter: return {false, true, false, false};
    case BlockType::ender_chest: return {true, false, true, false};
    default: return {false, false, false, false};
    }
}
const std::vector<bool>& modeled_blocks() {
    static const auto modeled = [] {
        std::vector<bool> out(registry::block_count());
        const auto mark = [&](std::string_view name) {
            for (const auto release : registry::releases())
                if (const auto b = registry::find_block(release, name)) out[*b] = true;
        };
        for (const auto name : modeled_names) mark(name);
        for (const auto kind : button_names) {
            mark(std::string(kind) + "_button");
            mark(std::string(kind) + "_pressure_plate");
        }
        for (const auto name : {"rail", "powered_rail", "activator_rail", "detector_rail"}) mark(name);
        for (std::uint8_t v = 0; v < 2 * blocks::coral_kinds; ++v) {
            mark(std::string(coral_fan_name(v, false)));
            mark(std::string(coral_fan_name(v, true)));
        }
        for (std::size_t k = 0; k < door_names.size(); ++k) {
            mark(std::string(door_names[k]) + "_door");
            mark(std::string(door_names[k]) + "_trapdoor");
            if (k < blocks::fence_gate_kinds) mark(std::string(door_names[k]) + "_fence_gate");
        }
        return out;
    }();
    return modeled;
}
} // namespace

std::string_view button_kind_name(std::uint8_t variant) noexcept {
    return variant < button_names.size() ? button_names[variant] : std::string_view{};
}

std::string coral_fan_name(std::uint8_t variant, bool wall) {
    constexpr std::array<std::string_view, blocks::coral_kinds> kinds{"tube", "brain", "bubble", "fire", "horn"};
    const bool dead = variant >= blocks::coral_kinds;
    return std::format("{}{}_coral_{}fan", dead ? "dead_" : "", kinds[variant % blocks::coral_kinds], wall ? "wall_" : "");
}

std::string_view rail_shape_name(std::uint8_t shape) noexcept {
    constexpr std::array<std::string_view, 10> names{"north_south", "east_west", "ascending_east", "ascending_west", "ascending_north",
                                                    "ascending_south", "south_east", "south_west", "north_west", "north_east"};
    return shape < names.size() ? names[shape] : std::string_view{};
}

std::string_view instrument_name(std::uint8_t instrument) noexcept {
    return instrument < instrument_names.size() ? instrument_names[instrument] : std::string_view{};
}

std::string_view door_kind_name(std::uint8_t variant) noexcept {
    return variant < door_names.size() ? door_names[variant] : std::string_view{};
}

bool modeled_block(std::uint32_t internal_block) noexcept {
    const auto& modeled = modeled_blocks();
    return internal_block < modeled.size() && modeled[internal_block];
}

std::uint32_t registry_state_count() noexcept { return registry::state_count(); }

bool same_block(BlockState a, BlockState b) noexcept {
    if (a.type() != b.type()) return false;
    // Each kind of button or pressure plate is a block of its own.
    if (has_variant(a.type())) return a.variant() == b.variant();
    return !a.is(BlockType::other) || registry::block_of(a.registry_state()) == registry::block_of(b.registry_state());
}

registry::PushClass push_class(BlockState s) noexcept {
    const auto block = registry::block_of(s.registry_state());
    return block ? registry::block(*block).push : registry::PushClass::immovable;
}

bool other_movable(BlockState s) noexcept {
    const auto c = push_class(s);
    return c == registry::PushClass::move || c == registry::PushClass::push_only;
}
bool other_destroyed_by_piston(BlockState s) noexcept { return push_class(s) == registry::PushClass::destroy; }
bool other_push_only(BlockState s) noexcept { return push_class(s) == registry::PushClass::push_only; }

bool other_has_collision(BlockState s) noexcept { return registry::collision(s.registry_state()) != registry::Collision::empty; }



contracts::Result<BlockState> BlockState::decode(std::uint32_t raw) noexcept {
    const auto invalid = std::unexpected(contracts::ErrorCode::invalid_block_state);
    if ((raw & 0xFF) >= static_cast<std::uint32_t>(BlockType::count_)) return invalid;
    const BlockState s(raw);
    if (s.is(BlockType::other)) {
        // Blocks with rules of their own never use the registry encoding.
        const auto block = registry::block_of(s.registry_state());
        if (!block || modeled_block(*block)) return invalid;
        return s;
    }
    if (raw >> 26) return invalid;
    if (raw >> 18) {
        // Only dust stores sides, each none, side or up, and only blocks of
        // several kinds a variant.
        if (has_variant(s.type())) {
            if (s.variant() >= variant_count(s.type())) return invalid;
        } else if (s.is(BlockType::note_block)) {
            if (s.variant() >= note_instruments) return invalid;
        } else if (!s.is(BlockType::redstone_wire)) {
            return invalid;
        } else {
            for (auto d : contracts::horizontal_directions)
                if (static_cast<std::uint8_t>(s.wire_side(d)) > 2) return invalid;
        }
    }
    const auto p = properties(s.type());
    const auto facing = (raw >> 8) & 7;
    const auto delay_bits = (raw >> 16) & 3;
    if (facing > 5 || (!p.facing && facing != 0)) return invalid;
    if (!p.level && s.level() != 0) return invalid;
    if (!p.flag && s.flag()) return invalid;
    if (!p.delay && delay_bits != 0) return invalid;
    switch (s.type()) {
    case BlockType::repeater:
        if (!contracts::is_horizontal(s.facing()) || s.level() > 1) return invalid;
        break;
    case BlockType::lever: case BlockType::button:
        if (!contracts::is_horizontal(s.facing()) || s.level() > 2) return invalid;
        break;
    case BlockType::redstone_wall_torch: case BlockType::coral_wall_fan:
        if (!contracts::is_horizontal(s.facing())) return invalid;
        break;
    // Powered is one bit; a fence gate's level only says whether it is in a wall.
    case BlockType::door: case BlockType::trapdoor:
        if (!contracts::is_horizontal(s.facing()) || s.level() > 3 || s.extra() > 1) return invalid;
        break;
    case BlockType::fence_gate:
        if (!contracts::is_horizontal(s.facing()) || s.level() > 1 || s.extra() > 1) return invalid;
        break;
    case BlockType::note_block:
        if (note_of(s) > 24) return invalid;
        break;
    // Plain rails curve and are never powered; the others do not curve.
    case BlockType::rail:
        if (s.extra() > 1 || s.level() >= (s.variant() == 0 ? 10 : 6) || (s.variant() == 0 && s.flag())) return invalid;
        break;
    case BlockType::hopper:
        if (s.facing() == Direction::up) return invalid;
        break;
    case BlockType::furnace:
        if (!contracts::is_horizontal(s.facing())) return invalid;
        break;
    // A horizontal front has its top up; an up or down front a horizontal top.
    case BlockType::crafter: {
        const auto top = static_cast<Direction>(s.variant());
        if (s.extra() > 1 || (contracts::is_horizontal(s.facing()) ? top != Direction::up : !contracts::is_horizontal(top))) return invalid;
        break;
    }
    case BlockType::chest:
        if (!contracts::is_horizontal(s.facing()) || s.level() > 2) return invalid;
        break;
    case BlockType::brewing_stand:
        if (s.level() > 7) return invalid;
        break;
    case BlockType::lectern:
        if (!contracts::is_horizontal(s.facing()) || s.extra() > 1) return invalid;
        break;
    case BlockType::composter:
        if (s.level() > 8) return invalid;
        break;
    case BlockType::ender_chest:
        if (!contracts::is_horizontal(s.facing())) return invalid;
        break;
    case BlockType::comparator:
        if (!contracts::is_horizontal(s.facing()) || s.level() > 1) return invalid;
        break;
    case BlockType::water:
        if (s.level() > 7) return invalid;
        break;
    case BlockType::air:
        if (s.level() > 2) return invalid;
        break;
    case BlockType::piston_head:
        if (s.level() > 1) return invalid;
        break;
    case BlockType::wheat:
        if (s.level() > 7) return invalid;
        break;
    default:
        break;
    }
    return s;
}

namespace {
std::string_view direction_name(Direction d) {
    switch (d) {
    case Direction::down: return "down";
    case Direction::up: return "up";
    case Direction::north: return "north";
    case Direction::south: return "south";
    case Direction::west: return "west";
    case Direction::east: return "east";
    }
    return "north";
}
} // namespace

std::string block_notation(BlockState s) {
    const auto b = [](bool v) { return v ? "true" : "false"; };
    switch (s.type()) {
    case BlockType::air: return s.level() == 1 ? "minecraft:cave_air" : s.level() == 2 ? "minecraft:void_air" : "minecraft:air";
    case BlockType::stone: return "minecraft:stone";
    case BlockType::glass: return "minecraft:glass";
    case BlockType::obsidian: return "minecraft:obsidian";
    case BlockType::redstone_block: return "minecraft:redstone_block";
    case BlockType::slime_block: return "minecraft:slime_block";
    case BlockType::honey_block: return "minecraft:honey_block";
    case BlockType::lever: {
        constexpr std::array<std::string_view, 3> faces{"floor", "wall", "ceiling"};
        return std::format("minecraft:lever[face={},facing={},powered={}]", faces[s.level() % 3], direction_name(s.facing()),
                           b(s.flag()));
    }
    case BlockType::redstone_wire: {
        constexpr std::array<std::string_view, 3> sides{"none", "side", "up"};
        const auto side = [&](Direction d) { return sides[static_cast<std::size_t>(s.wire_side(d)) % 3]; };
        return std::format("minecraft:redstone_wire[east={},north={},power={},south={},west={}]", side(Direction::east),
                           side(Direction::north), s.level(), side(Direction::south), side(Direction::west));
    }
    case BlockType::repeater:
        return std::format("minecraft:repeater[delay={},facing={},locked={},powered={}]", s.delay(),
                           direction_name(contracts::opposite(s.facing())), b(repeater_locked(s)), b(s.flag()));
    case BlockType::redstone_lamp: return std::format("minecraft:redstone_lamp[lit={}]", b(s.flag()));
    case BlockType::observer:
        return std::format("minecraft:observer[facing={},powered={}]", direction_name(s.facing()), b(s.flag()));
    case BlockType::piston:
    case BlockType::sticky_piston:
        return std::format("minecraft:{}[extended={},facing={}]", s.is(BlockType::piston) ? "piston" : "sticky_piston",
                           b(s.flag()), direction_name(s.facing()));
    case BlockType::piston_head:
        return std::format("minecraft:piston_head[facing={},short={},type={}]", direction_name(s.facing()), b(s.level() == 1),
                           s.flag() ? "sticky" : "normal");
    case BlockType::moving_piston:
        return std::format("minecraft:moving_piston[facing={},type={}]", direction_name(s.facing()), s.flag() ? "sticky" : "normal");
    case BlockType::hopper:
        return std::format("minecraft:hopper[enabled={},facing={}]", b(s.flag()), direction_name(s.facing()));
    case BlockType::chest: {
        static constexpr std::string_view types[]{"single", "left", "right"};
        return std::format("minecraft:{}[facing={},type={},waterlogged={}]", s.variant() == 1 ? "trapped_chest" : "chest",
                           direction_name(s.facing()), types[s.level() % 3], b(s.flag()));
    }
    case BlockType::comparator:
        return std::format("minecraft:comparator[facing={},mode={},powered={}]", direction_name(contracts::opposite(s.facing())),
                           s.level() == 1 ? "subtract" : "compare", b(s.flag()));
    case BlockType::water:
        return std::format("minecraft:water[level={}]", s.flag() ? 8 + s.level() : s.level());
    case BlockType::wheat: return std::format("minecraft:wheat[age={}]", s.level());
    case BlockType::redstone_torch: return std::format("minecraft:redstone_torch[lit={}]", b(s.flag()));
    case BlockType::redstone_wall_torch:
        return std::format("minecraft:redstone_wall_torch[facing={},lit={}]", direction_name(s.facing()), b(s.flag()));
    case BlockType::button: {
        constexpr std::array<std::string_view, 3> faces{"floor", "wall", "ceiling"};
        return std::format("minecraft:{}_button[face={},facing={},powered={}]", button_kind_name(s.variant()), faces[s.level() % 3],
                           direction_name(s.facing()), b(s.flag()));
    }
    case BlockType::pressure_plate:
        return std::format("minecraft:{}_pressure_plate[powered={}]", button_kind_name(s.variant()), b(s.flag()));
    case BlockType::weighted_plate:
        return std::format("minecraft:{}_weighted_pressure_plate[power={}]", s.variant() == 1 ? "heavy" : "light", s.level());
    case BlockType::target: return std::format("minecraft:target[power={}]", s.level());
    case BlockType::daylight_detector:
        return std::format("minecraft:daylight_detector[inverted={},power={}]", b(s.flag()), s.level());
    case BlockType::note_block:
        return std::format("minecraft:note_block[instrument={},note={},powered={}]", instrument_name(s.variant()), note_of(s), b(s.flag()));
    case BlockType::coral_fan: return std::format("minecraft:{}[waterlogged={}]", coral_fan_name(s.variant(), false), b(s.flag()));
    case BlockType::coral_wall_fan:
        return std::format("minecraft:{}[facing={},waterlogged={}]", coral_fan_name(s.variant(), true), direction_name(s.facing()), b(s.flag()));
    case BlockType::rail: {
        if (s.variant() == 0) return std::format("minecraft:rail[shape={},waterlogged={}]", rail_shape_name(s.level()), b(s.extra() != 0));
        constexpr std::array<std::string_view, 4> kinds{"rail", "powered_rail", "activator_rail", "detector_rail"};
        return std::format("minecraft:{}[powered={},shape={},waterlogged={}]", kinds[s.variant() % 4], b(s.flag()), rail_shape_name(s.level()),
                           b(s.extra() != 0));
    }
    case BlockType::door:
        return std::format("minecraft:{}_door[facing={},half={},hinge={},open={},powered={}]", door_kind_name(s.variant()),
                           direction_name(s.facing()), upper_half(s) ? "upper" : "lower", (s.level() & 2) ? "right" : "left", b(s.flag()),
                           b(s.extra() != 0));
    case BlockType::trapdoor:
        return std::format("minecraft:{}_trapdoor[facing={},half={},open={},powered={},waterlogged={}]", door_kind_name(s.variant()),
                           direction_name(s.facing()), (s.level() & 1) ? "top" : "bottom", b(s.flag()), b(s.extra() != 0),
                           b((s.level() & 2) != 0));
    case BlockType::fence_gate:
        return std::format("minecraft:{}_fence_gate[facing={},in_wall={},open={},powered={}]", door_kind_name(s.variant()),
                           direction_name(s.facing()), b(s.level() != 0), b(s.flag()), b(s.extra() != 0));
    case BlockType::dispenser:
        return std::format("minecraft:{}[facing={},triggered={}]", s.variant() == 1 ? "dropper" : "dispenser", direction_name(s.facing()),
                           b(s.flag()));
    case BlockType::furnace: {
        constexpr std::array<std::string_view, 3> kinds{"furnace", "smoker", "blast_furnace"};
        return std::format("minecraft:{}[facing={},lit={}]", kinds[s.variant() % 3], direction_name(s.facing()), b(s.flag()));
    }
    case BlockType::crafter:
        return std::format("minecraft:crafter[crafting={},orientation={}_{},triggered={}]", b(s.extra() == 1), direction_name(s.facing()),
                           direction_name(static_cast<Direction>(s.variant())), b(s.flag()));
    case BlockType::brewing_stand:
        return std::format("minecraft:brewing_stand[has_bottle_0={},has_bottle_1={},has_bottle_2={}]", b(s.level() & 1), b(s.level() & 2),
                           b(s.level() & 4));
    case BlockType::lectern:
        return std::format("minecraft:lectern[facing={},has_book={},powered={}]", direction_name(s.facing()), b(s.extra() == 1), b(s.flag()));
    case BlockType::barrel: return std::format("minecraft:barrel[facing={},open={}]", direction_name(s.facing()), b(s.flag()));
    case BlockType::composter: return std::format("minecraft:composter[level={}]", s.level());
    case BlockType::ender_chest:
        return std::format("minecraft:ender_chest[facing={},waterlogged={}]", direction_name(s.facing()), b(s.flag()));
    case BlockType::other: return registry::notation(s.registry_state());
    default: return std::format("circuit:unrepresentable[raw={}]", s.raw());
    }
}

std::optional<std::uint32_t> registry_equivalent(BlockState s) {
    if (s.is(BlockType::other)) return s.registry_state();
    const auto text = block_notation(s);
    if (!text.starts_with("minecraft:")) return std::nullopt;
    const auto open = text.find('[');
    const auto block = registry::find_block("1.21.8", std::string_view(text).substr(10, open == std::string::npos ? std::string::npos : open - 10));
    if (!block) return std::nullopt;
    std::vector<registry::Property> props;
    if (open != std::string::npos)
        for (std::size_t i = open + 1; i < text.size() - 1;) {
            const auto comma = std::min(text.find(',', i), text.size() - 1);
            const auto eq = text.find('=', i);
            props.emplace_back(text.substr(i, eq - i), text.substr(eq + 1, comma - eq - 1));
            i = comma + 1;
        }
    return registry::state_of(*block, props);
}

} // namespace circuit::gameplay
