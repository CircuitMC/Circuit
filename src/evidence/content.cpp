// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/evidence/content.hpp>
#include <circuit/evidence/trace_format.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <array>
#include <charconv>
#include <format>
#include <map>
#include <optional>
#include <set>

namespace circuit::evidence {
namespace {
using gameplay::BlockState;
using gameplay::BlockType;
using contracts::Direction;
namespace blocks = gameplay::blocks;

constexpr ContentMapping java_1_21_8{"1.21.8"};
// The modeled blocks and items have the same names and properties in 1.21.4
// (checked against the 1.21.4 content table in evidence_tests).
constexpr ContentMapping java_1_21_4{"1.21.4"};

std::string_view direction_name(Direction d) {
    switch (d) {
    case Direction::down: return "down";
    case Direction::up: return "up";
    case Direction::north: return "north";
    case Direction::south: return "south";
    case Direction::west: return "west";
    case Direction::east: return "east";
    }
    return "down";
}
std::optional<Direction> direction_from(std::string_view s) {
    for (auto d : contracts::all_directions)
        if (direction_name(d) == s) return d;
    return std::nullopt;
}

// Property bag of one notation with strict accounting: every property must be
// consumed as modeled, ignored, or fixed to its only supported value.
class Props {
public:
    Props(std::string id, const std::vector<std::pair<std::string, std::string>>& properties) : id_(std::move(id)) {
        for (const auto& [k, v] : properties) values_[k] = v;
    }
    std::expected<std::string, std::string> required(std::string_view key) {
        const auto it = values_.find(std::string(key));
        if (it == values_.end()) return std::unexpected(std::format("{} requires property `{}`", id_, key));
        used_.insert(it->first);
        return it->second;
    }
    std::optional<std::string> optional(std::string_view key) {
        const auto it = values_.find(std::string(key));
        if (it == values_.end()) return std::nullopt;
        used_.insert(it->first);
        return it->second;
    }
    std::expected<bool, std::string> flag(std::string_view key) {
        const auto v = required(key);
        if (!v) return std::unexpected(v.error());
        if (*v == "true") return true;
        if (*v == "false") return false;
        return std::unexpected(std::format("{}: `{}` must be true or false", id_, key));
    }
    std::expected<unsigned, std::string> integer(std::string_view key, unsigned low, unsigned high) {
        const auto v = required(key);
        if (!v) return std::unexpected(v.error());
        unsigned n{};
        const auto [end, error] = std::from_chars(v->data(), v->data() + v->size(), n);
        if (error != std::errc{} || end != v->data() + v->size() || n < low || n > high)
            return std::unexpected(std::format("{}: `{}` must be in [{}, {}]", id_, key, low, high));
        return n;
    }
    std::expected<unsigned, std::string> integer_or(std::string_view key, unsigned low, unsigned high, unsigned fallback) {
        if (!values_.contains(std::string(key))) return fallback;
        return integer(key, low, high);
    }
    std::expected<Direction, std::string> direction(std::string_view key) {
        const auto v = required(key);
        if (!v) return std::unexpected(v.error());
        const auto d = direction_from(*v);
        if (!d) return std::unexpected(std::format("{}: invalid direction `{}`", id_, *v));
        return *d;
    }
    // Index of the value among `allowed`; `fallback` when the property is absent.
    std::expected<unsigned, std::string> choice(std::string_view key, std::initializer_list<std::string_view> allowed,
                                                unsigned fallback) {
        const auto it = values_.find(std::string(key));
        if (it == values_.end()) return fallback;
        used_.insert(it->first);
        unsigned i = 0;
        for (auto a : allowed) {
            if (a == it->second) return i;
            ++i;
        }
        return std::unexpected(std::format("{}: invalid value `{}` for `{}`", id_, it->second, key));
    }
    std::expected<Direction, std::string> direction_or(std::string_view key, Direction fallback) {
        if (!values_.contains(std::string(key))) return fallback;
        return direction(key);
    }
    // Accepted for any of the listed values, but not represented.
    std::expected<void, std::string> ignored(std::string_view key, std::initializer_list<std::string_view> allowed) {
        const auto it = values_.find(std::string(key));
        if (it == values_.end()) return {};
        used_.insert(it->first);
        for (auto a : allowed)
            if (a == it->second) return {};
        return std::unexpected(std::format("{}: invalid value `{}` for `{}`", id_, it->second, key));
    }
    // Representable only with the given value; other valid values are unsupported content.
    std::expected<void, std::string> only(std::string_view key, std::string_view supported) {
        const auto it = values_.find(std::string(key));
        if (it == values_.end()) return {};
        used_.insert(it->first);
        if (it->second == supported) return {};
        return std::unexpected(std::format("unsupported: {} with {}={} is not modeled", id_, key, it->second));
    }
    std::expected<void, std::string> done() const {
        for (const auto& [k, v] : values_)
            if (!used_.contains(k)) return std::unexpected(std::format("{}: unknown property `{}`", id_, k));
        return {};
    }

private:
    std::string id_;
    std::map<std::string, std::string> values_;
    std::set<std::string> used_;
};

#define CIRCUIT_TRY(var, expr)                                     \
    auto var##_result = (expr);                                    \
    if (!var##_result) return std::unexpected(var##_result.error()); \
    auto var = *var##_result
#define CIRCUIT_CHECK(expr)                                         \
    if (auto check_result = (expr); !check_result) return std::unexpected(check_result.error())

} // namespace

const ContentMapping* ContentMapping::for_game_version(std::string_view game_version) noexcept {
    if (game_version == java_1_21_8.game_version()) return &java_1_21_8;
    if (game_version == java_1_21_4.game_version()) return &java_1_21_4;
    return nullptr;
}

std::expected<BlockState, std::string> ContentMapping::block(std::string_view notation) const {
    const auto spec = parse_state(notation);
    if (!spec) return std::unexpected(spec.error());
    const auto& id = spec->id;
    Props p(id, spec->properties);
    const auto simple = [&](BlockState s) -> std::expected<BlockState, std::string> {
        CIRCUIT_CHECK(p.done());
        return s;
    };
    if (id == "minecraft:air") return simple(blocks::air());
    if (id == "minecraft:cave_air") return simple(blocks::cave_air());
    if (id == "minecraft:void_air") return simple(blocks::void_air());
    if (id == "minecraft:stone") return simple(blocks::stone());
    if (id == "minecraft:glass") return simple(blocks::glass());
    if (id == "minecraft:obsidian") return simple(blocks::obsidian());
    if (id == "minecraft:redstone_block") return simple(blocks::redstone_block());
    if (id == "minecraft:slime_block") return simple(blocks::slime());
    if (id == "minecraft:honey_block") return simple(blocks::honey());
    if (id == "minecraft:lever") {
        CIRCUIT_TRY(powered, p.flag("powered"));
        // Absent mounting properties take the target version's defaults (wall, north).
        CIRCUIT_TRY(face, p.choice("face", {"floor", "wall", "ceiling"}, 1));
        CIRCUIT_TRY(facing, p.direction_or("facing", Direction::north));
        if (!contracts::is_horizontal(facing)) return std::unexpected("minecraft:lever: facing must be horizontal");
        return simple(blocks::lever(powered, facing, static_cast<blocks::LeverFace>(face)));
    }
    if (id == "minecraft:redstone_wire") {
        CIRCUIT_TRY(power, p.integer("power", 0, 15));
        // The stored shape matters: a dust without sides (a dot) points nowhere.
        auto s = blocks::wire(static_cast<std::uint8_t>(power));
        for (const auto d : {Direction::north, Direction::south, Direction::west, Direction::east}) {
            CIRCUIT_TRY(side, p.choice(direction_name(d), {"none", "side", "up"}, 0));
            s = s.with_wire_side(d, static_cast<gameplay::WireSide>(side));
        }
        return simple(s);
    }
    if (id == "minecraft:repeater") {
        CIRCUIT_TRY(delay, p.integer("delay", 1, 4));
        CIRCUIT_TRY(facing, p.direction("facing"));
        CIRCUIT_TRY(powered, p.flag("powered"));
        CIRCUIT_TRY(locked, p.choice("locked", {"false", "true"}, 0));
        if (!contracts::is_horizontal(facing)) return std::unexpected("minecraft:repeater: facing must be horizontal");
        // `facing` names the input side; the output is opposite (confirmed by traces).
        return simple(blocks::repeater(contracts::opposite(facing), static_cast<std::uint8_t>(delay), powered, locked == 1));
    }
    if (id == "minecraft:redstone_lamp") {
        CIRCUIT_TRY(lit, p.flag("lit"));
        return simple(blocks::lamp(lit));
    }
    if (id == "minecraft:observer") {
        CIRCUIT_TRY(facing, p.direction("facing"));
        CIRCUIT_TRY(powered, p.flag("powered"));
        return simple(blocks::observer(facing, powered));
    }
    if (id == "minecraft:dispenser" || id == "minecraft:dropper") {
        // Absent properties take the default state's (facing north, not triggered).
        CIRCUIT_TRY(facing, p.direction_or("facing", Direction::north));
        CIRCUIT_TRY(triggered, p.choice("triggered", {"false", "true"}, 0));
        return simple(blocks::dispenser(facing, id == "minecraft:dropper", triggered == 1));
    }
    if (id == "minecraft:furnace" || id == "minecraft:smoker" || id == "minecraft:blast_furnace") {
        // Absent properties take the default state's (facing north, not lit).
        CIRCUIT_TRY(facing, p.direction_or("facing", Direction::north));
        if (!contracts::is_horizontal(facing)) return std::unexpected(std::format("{}: `facing` must be horizontal", id));
        CIRCUIT_TRY(lit, p.choice("lit", {"false", "true"}, 0));
        const auto kind = id == "minecraft:smoker" ? blocks::FurnaceKind::smoker
                        : id == "minecraft:blast_furnace" ? blocks::FurnaceKind::blast_furnace : blocks::FurnaceKind::furnace;
        return simple(blocks::furnace(kind, facing, lit == 1));
    }
    if (id == "minecraft:crafter") {
        // Absent properties take the default state's (north_up, neither triggered nor crafting).
        CIRCUIT_TRY(crafting, p.choice("crafting", {"false", "true"}, 0));
        CIRCUIT_TRY(triggered, p.choice("triggered", {"false", "true"}, 0));
        // The front, then the direction the top faces.
        CIRCUIT_TRY(orientation, p.choice("orientation", {"down_east", "down_north", "down_south", "down_west", "up_east", "up_north", "up_south",
                                                          "up_west", "west_up", "east_up", "north_up", "south_up"},
                                          10));
        using D = Direction;
        constexpr std::array<std::pair<D, D>, 12> sides{{{D::down, D::east}, {D::down, D::north}, {D::down, D::south}, {D::down, D::west},
                                                         {D::up, D::east}, {D::up, D::north}, {D::up, D::south}, {D::up, D::west},
                                                         {D::west, D::up}, {D::east, D::up}, {D::north, D::up}, {D::south, D::up}}};
        return simple(blocks::crafter(sides[orientation].first, sides[orientation].second, triggered == 1, crafting == 1));
    }
    if (id == "minecraft:lectern") {
        // Absent properties take the default state's (facing north, no book, not powered).
        CIRCUIT_TRY(facing, p.direction_or("facing", Direction::north));
        if (!contracts::is_horizontal(facing)) return std::unexpected(std::format("{}: `facing` must be horizontal", id));
        CIRCUIT_TRY(has_book, p.choice("has_book", {"false", "true"}, 0));
        CIRCUIT_TRY(powered, p.choice("powered", {"false", "true"}, 0));
        return simple(blocks::lectern(facing, has_book == 1, powered == 1));
    }
    if (id == "minecraft:brewing_stand") {
        // Absent properties take the default state's (no bottles).
        std::uint8_t bottles = 0;
        for (std::uint8_t k = 0; k < 3; ++k) {
            CIRCUIT_TRY(has, p.choice(std::format("has_bottle_{}", k), {"false", "true"}, 0));
            if (has == 1) bottles = static_cast<std::uint8_t>(bottles | 1u << k);
        }
        return simple(blocks::brewing_stand(bottles));
    }
    if (id == "minecraft:piston" || id == "minecraft:sticky_piston") {
        CIRCUIT_TRY(facing, p.direction("facing"));
        CIRCUIT_TRY(extended, p.flag("extended"));
        return simple(blocks::piston(facing, id == "minecraft:sticky_piston").with_flag(extended));
    }
    if (id == "minecraft:piston_head") {
        CIRCUIT_TRY(facing, p.direction("facing"));
        CIRCUIT_TRY(type, p.required("type"));
        if (type != "normal" && type != "sticky") return std::unexpected("minecraft:piston_head: invalid type");
        CIRCUIT_TRY(short_arm, p.choice("short", {"false", "true"}, 0));
        return simple(BlockState::of(BlockType::piston_head).with_facing(facing).with_flag(type == "sticky").with_level(
            static_cast<std::uint8_t>(short_arm)));
    }
    if (id == "minecraft:moving_piston") {
        CIRCUIT_TRY(facing, p.direction("facing"));
        CIRCUIT_TRY(type, p.required("type"));
        if (type != "normal" && type != "sticky") return std::unexpected("minecraft:moving_piston: invalid type");
        // The moving block itself is block-entity state and not part of the notation.
        return simple(blocks::moving_piston(facing, type == "sticky"));
    }
    if (id == "minecraft:hopper") {
        CIRCUIT_TRY(facing, p.direction("facing"));
        CIRCUIT_TRY(enabled, p.flag("enabled"));
        if (facing == Direction::up) return std::unexpected("minecraft:hopper: facing cannot be up");
        return simple(blocks::hopper(facing, enabled));
    }
    if (id == "minecraft:chest") {
        CIRCUIT_TRY(facing, p.direction("facing"));
        CIRCUIT_TRY(type, p.required("type"));
        CIRCUIT_TRY(waterlogged, p.choice("waterlogged", {"false", "true"}, 0));
        if (!contracts::is_horizontal(facing)) return std::unexpected("minecraft:chest: facing must be horizontal");
        // Unverified convention: a left half pairs clockwise of its facing, a right half counter-clockwise.
        const auto kind = type == "single" ? blocks::ChestType::single
                        : type == "left"   ? blocks::ChestType::left
                        : type == "right"  ? blocks::ChestType::right
                                           : static_cast<blocks::ChestType>(255);
        if (static_cast<int>(kind) == 255) return std::unexpected("minecraft:chest: invalid type");
        return simple(blocks::chest(facing, kind).with_flag(waterlogged == 1));
    }
    if (id == "minecraft:comparator") {
        CIRCUIT_TRY(facing, p.direction("facing"));
        CIRCUIT_TRY(mode, p.required("mode"));
        CIRCUIT_TRY(powered, p.flag("powered"));
        if (!contracts::is_horizontal(facing)) return std::unexpected("minecraft:comparator: facing must be horizontal");
        if (mode != "compare" && mode != "subtract") return std::unexpected("minecraft:comparator: invalid mode");
        // Unverified convention, as for repeaters: `facing` names the input side.
        return simple(blocks::comparator(contracts::opposite(facing), mode == "subtract", powered));
    }
    if (id == "minecraft:water") {
        CIRCUIT_TRY(level, p.integer("level", 0, 15));
        // 0 source, 1-7 flowing distance, 8-15 falling (the rules treat every falling level alike).
        if (level == 0) return simple(blocks::water_source());
        if (level <= 7) return simple(blocks::water_flowing(static_cast<std::uint8_t>(level)));
        return simple(blocks::water_falling().with_level(static_cast<std::uint8_t>(level - 8)));
    }
    if (id == "minecraft:wheat") {
        CIRCUIT_TRY(age, p.integer("age", 0, 7));
        return simple(blocks::wheat(static_cast<std::uint8_t>(age)));
    }
    // Absent properties take the target version's defaults (lit, north, wall).
    if (id == "minecraft:redstone_torch") {
        CIRCUIT_TRY(lit, p.choice("lit", {"false", "true"}, 1));
        return simple(blocks::torch(lit == 1));
    }
    if (id == "minecraft:redstone_wall_torch") {
        CIRCUIT_TRY(facing, p.direction_or("facing", Direction::north));
        CIRCUIT_TRY(lit, p.choice("lit", {"false", "true"}, 1));
        if (!contracts::is_horizontal(facing)) return std::unexpected("minecraft:redstone_wall_torch: facing must be horizontal");
        return simple(blocks::wall_torch(facing, lit == 1));
    }
    if (id.starts_with("minecraft:") && id.ends_with("_button")) {
        const auto kind = id.substr(10, id.size() - 10 - 7);
        for (std::uint8_t k = 0; k < blocks::button_kinds; ++k) {
            if (gameplay::button_kind_name(k) != kind) continue;
            CIRCUIT_TRY(face, p.choice("face", {"floor", "wall", "ceiling"}, 1));
            CIRCUIT_TRY(facing, p.direction_or("facing", Direction::north));
            CIRCUIT_TRY(powered, p.choice("powered", {"false", "true"}, 0));
            if (!contracts::is_horizontal(facing)) return std::unexpected(std::format("{}: facing must be horizontal", id));
            return simple(blocks::button(static_cast<blocks::ButtonKind>(k), powered == 1, facing,
                                         static_cast<blocks::LeverFace>(face)));
        }
    }
    if (id == "minecraft:light_weighted_pressure_plate" || id == "minecraft:heavy_weighted_pressure_plate") {
        CIRCUIT_TRY(power, p.integer_or("power", 0, 15, 0));
        return simple(blocks::weighted_plate(id == "minecraft:heavy_weighted_pressure_plate", static_cast<std::uint8_t>(power)));
    }
    if (id == "minecraft:target") {
        CIRCUIT_TRY(power, p.integer_or("power", 0, 15, 0));
        return simple(blocks::target(static_cast<std::uint8_t>(power)));
    }
    for (std::uint8_t v = 0; v < 2 * blocks::coral_kinds; ++v)
        for (const bool wall : {false, true}) {
            if (id != "minecraft:" + gameplay::coral_fan_name(v, wall)) continue;
            CIRCUIT_TRY(waterlogged, p.choice("waterlogged", {"false", "true"}, 1));
            const auto kind = static_cast<blocks::CoralKind>(v % blocks::coral_kinds);
            const bool dead = v >= blocks::coral_kinds;
            if (!wall) return simple(blocks::coral_fan(kind, dead, waterlogged == 1));
            CIRCUIT_TRY(facing, p.direction_or("facing", Direction::north));
            if (!contracts::is_horizontal(facing)) return std::unexpected(std::format("{}: facing must be horizontal", id));
            return simple(blocks::coral_wall_fan(kind, facing, dead, waterlogged == 1));
        }
    for (std::uint8_t k = 0; k < blocks::rail_kinds; ++k) {
        constexpr std::array<std::string_view, 4> names{"minecraft:rail", "minecraft:powered_rail", "minecraft:activator_rail",
                                                         "minecraft:detector_rail"};
        if (id != names[k]) continue;
        // Absent properties take the target version's defaults (north_south, unpowered, dry).
        std::uint8_t shape = 0;
        if (const auto value = p.optional("shape")) {
            while (shape < (k == 0 ? 10 : 6) && gameplay::rail_shape_name(shape) != *value) ++shape;
            if (shape == (k == 0 ? 10 : 6)) return std::unexpected(std::format("{}: invalid shape `{}`", id, *value));
        }
        CIRCUIT_TRY(waterlogged, p.choice("waterlogged", {"false", "true"}, 0));
        unsigned powered = 0;
        if (k != 0) {
            CIRCUIT_TRY(on, p.choice("powered", {"false", "true"}, 0));
            powered = on;
        }
        return simple(blocks::rail(static_cast<blocks::RailKind>(k), static_cast<blocks::RailShape>(shape), powered == 1)
                          .with_extra(static_cast<std::uint8_t>(waterlogged)));
    }
    if (id == "minecraft:note_block") {
        // Absent properties take the target version's defaults (harp, 0, unpowered).
        std::uint8_t instrument = 0;
        if (const auto value = p.optional("instrument")) {
            while (instrument < gameplay::note_instruments && gameplay::instrument_name(instrument) != *value) ++instrument;
            if (instrument == gameplay::note_instruments) return std::unexpected(std::format("{}: unknown instrument `{}`", id, *value));
        }
        CIRCUIT_TRY(note, p.integer_or("note", 0, 24, 0));
        CIRCUIT_TRY(powered, p.choice("powered", {"false", "true"}, 0));
        return simple(blocks::note_block(instrument, static_cast<std::uint8_t>(note), powered == 1));
    }
    if (id == "minecraft:daylight_detector") {
        CIRCUIT_TRY(inverted, p.choice("inverted", {"false", "true"}, 0));
        CIRCUIT_TRY(power, p.integer_or("power", 0, 15, 0));
        return simple(blocks::daylight_detector(inverted == 1, static_cast<std::uint8_t>(power)));
    }
    if (id.starts_with("minecraft:") && id.ends_with("_pressure_plate")) {
        const auto kind = id.substr(10, id.size() - 10 - 15);
        for (std::uint8_t k = 0; k < blocks::button_kinds; ++k) {
            if (gameplay::button_kind_name(k) != kind) continue;
            CIRCUIT_TRY(powered, p.choice("powered", {"false", "true"}, 0));
            return simple(blocks::pressure_plate(static_cast<blocks::ButtonKind>(k), powered == 1));
        }
    }
    for (const auto& [suffix, type] : {std::pair{std::string_view("_door"), BlockType::door}, std::pair{std::string_view("_trapdoor"), BlockType::trapdoor},
                                      std::pair{std::string_view("_fence_gate"), BlockType::fence_gate}}) {
        if (!id.starts_with("minecraft:") || !id.ends_with(suffix)) continue;
        const auto kind = id.substr(10, id.size() - 10 - suffix.size());
        const auto kinds = type == BlockType::fence_gate ? blocks::fence_gate_kinds : blocks::door_kinds;
        for (std::uint8_t k = 0; k < kinds; ++k) {
            if (gameplay::door_kind_name(k) != kind) continue;
            const auto dk = static_cast<blocks::DoorKind>(k);
            CIRCUIT_TRY(facing, p.direction_or("facing", Direction::north));
            if (!contracts::is_horizontal(facing)) return std::unexpected(std::format("{}: facing must be horizontal", id));
            CIRCUIT_TRY(open, p.choice("open", {"false", "true"}, 0));
            CIRCUIT_TRY(powered, p.choice("powered", {"false", "true"}, 0));
            if (type == BlockType::door) {
                CIRCUIT_TRY(half, p.choice("half", {"lower", "upper"}, 0));
                CIRCUIT_TRY(hinge, p.choice("hinge", {"left", "right"}, 0));
                return simple(blocks::door(dk, facing, half == 1, hinge == 1, open == 1, powered == 1));
            }
            if (type == BlockType::trapdoor) {
                CIRCUIT_TRY(half, p.choice("half", {"bottom", "top"}, 0));
                CIRCUIT_TRY(waterlogged, p.choice("waterlogged", {"false", "true"}, 0));
                return simple(blocks::trapdoor(dk, facing, half == 1, open == 1, powered == 1)
                                  .with_level(static_cast<std::uint8_t>(half | (waterlogged << 1))));
            }
            CIRCUIT_TRY(in_wall, p.choice("in_wall", {"false", "true"}, 0));
            return simple(blocks::fence_gate(dk, facing, open == 1, powered == 1, in_wall == 1));
        }
    }
    // Every other block is a registry state without rules of its own.
    const auto block = gameplay::registry::find_block(game_version_, id);
    if (!block || gameplay::modeled_block(*block))
        return std::unexpected(std::format("unsupported: block `{}` is not in the {} registry", id, game_version_));
    const auto state = gameplay::registry::state_of(*block, spec->properties);
    if (!state) return std::unexpected(std::format("{}: unknown property or value in `{}`", id, notation));
    return BlockState::registered(*state);
}

std::string ContentMapping::notation(BlockState s) const { return gameplay::block_notation(s); }

std::expected<std::uint16_t, std::string> ContentMapping::item(std::string_view id) const {
    if (const auto item = gameplay::items::by_name(id)) return *item;
    return std::unexpected(std::format("unsupported: item `{}` is not in the reference registry", id));
}

std::string ContentMapping::item_name(std::uint16_t item) const {
    if (const auto* info = gameplay::items::info(item)) return std::string(info->name);
    return std::format("circuit:item_{}", item);
}

#undef CIRCUIT_TRY
#undef CIRCUIT_CHECK

} // namespace circuit::evidence
