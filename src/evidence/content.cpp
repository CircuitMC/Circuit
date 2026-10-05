// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/evidence/content.hpp>
#include <circuit/evidence/trace_format.hpp>
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
    std::expected<Direction, std::string> direction(std::string_view key) {
        const auto v = required(key);
        if (!v) return std::unexpected(v.error());
        const auto d = direction_from(*v);
        if (!d) return std::unexpected(std::format("{}: invalid direction `{}`", id_, *v));
        return *d;
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

constexpr std::array<std::pair<std::string_view, std::uint16_t>, 4> item_ids{{
    {"minecraft:stone", gameplay::items::stone},
    {"minecraft:redstone", gameplay::items::redstone},
    {"minecraft:wheat", gameplay::items::wheat},
    {"minecraft:iron_ingot", gameplay::items::iron_ingot},
}};
} // namespace

const ContentMapping* ContentMapping::for_game_version(std::string_view game_version) noexcept {
    return game_version == java_1_21_8.game_version() ? &java_1_21_8 : nullptr;
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
    if (id == "minecraft:air" || id == "minecraft:cave_air" || id == "minecraft:void_air") return simple(blocks::air());
    if (id == "minecraft:stone") return simple(blocks::stone());
    if (id == "minecraft:glass") return simple(blocks::glass());
    if (id == "minecraft:obsidian") return simple(blocks::obsidian());
    if (id == "minecraft:redstone_block") return simple(blocks::redstone_block());
    if (id == "minecraft:slime_block") return simple(blocks::slime());
    if (id == "minecraft:lever") {
        CIRCUIT_TRY(powered, p.flag("powered"));
        CIRCUIT_CHECK(p.ignored("face", {"floor", "wall", "ceiling"}));
        CIRCUIT_CHECK(p.ignored("facing", {"north", "south", "west", "east"}));
        return simple(blocks::lever(powered));
    }
    if (id == "minecraft:redstone_wire") {
        CIRCUIT_TRY(power, p.integer("power", 0, 15));
        for (auto side : {"north", "south", "west", "east"}) CIRCUIT_CHECK(p.ignored(side, {"up", "side", "none"}));
        return simple(blocks::wire(static_cast<std::uint8_t>(power)));
    }
    if (id == "minecraft:repeater") {
        CIRCUIT_TRY(delay, p.integer("delay", 1, 4));
        CIRCUIT_TRY(facing, p.direction("facing"));
        CIRCUIT_TRY(powered, p.flag("powered"));
        CIRCUIT_CHECK(p.only("locked", "false"));
        if (!contracts::is_horizontal(facing)) return std::unexpected("minecraft:repeater: facing must be horizontal");
        // Unverified convention: `facing` names the input side; the output is opposite.
        return simple(blocks::repeater(contracts::opposite(facing), static_cast<std::uint8_t>(delay), powered));
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
    if (id == "minecraft:piston" || id == "minecraft:sticky_piston") {
        CIRCUIT_TRY(facing, p.direction("facing"));
        CIRCUIT_TRY(extended, p.flag("extended"));
        return simple(blocks::piston(facing, id == "minecraft:sticky_piston").with_flag(extended));
    }
    if (id == "minecraft:piston_head") {
        CIRCUIT_TRY(facing, p.direction("facing"));
        CIRCUIT_TRY(type, p.required("type"));
        if (type != "normal" && type != "sticky") return std::unexpected("minecraft:piston_head: invalid type");
        CIRCUIT_CHECK(p.ignored("short", {"true", "false"}));
        return simple(BlockState::of(BlockType::piston_head).with_facing(facing).with_flag(type == "sticky"));
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
        CIRCUIT_CHECK(p.only("waterlogged", "false"));
        if (!contracts::is_horizontal(facing)) return std::unexpected("minecraft:chest: facing must be horizontal");
        // Unverified convention: a left half pairs clockwise of its facing, a right half counter-clockwise.
        const auto kind = type == "single" ? blocks::ChestType::single
                        : type == "left"   ? blocks::ChestType::left
                        : type == "right"  ? blocks::ChestType::right
                                           : static_cast<blocks::ChestType>(255);
        if (static_cast<int>(kind) == 255) return std::unexpected("minecraft:chest: invalid type");
        return simple(blocks::chest(facing, kind));
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
        // Unverified convention: 0 source, 1-7 flowing distance, 8-15 falling.
        if (level == 0) return simple(blocks::water_source());
        if (level <= 7) return simple(blocks::water_flowing(static_cast<std::uint8_t>(level)));
        return simple(blocks::water_falling());
    }
    if (id == "minecraft:wheat") {
        CIRCUIT_TRY(age, p.integer("age", 0, 7));
        return simple(blocks::wheat(static_cast<std::uint8_t>(age)));
    }
    return std::unexpected(std::format("unsupported: block `{}` is not in the reference registry", id));
}

std::string ContentMapping::notation(BlockState s) const {
    const auto b = [](bool v) { return v ? "true" : "false"; };
    switch (s.type()) {
    case BlockType::air: return "minecraft:air";
    case BlockType::stone: return "minecraft:stone";
    case BlockType::glass: return "minecraft:glass";
    case BlockType::obsidian: return "minecraft:obsidian";
    case BlockType::redstone_block: return "minecraft:redstone_block";
    case BlockType::slime_block: return "minecraft:slime_block";
    case BlockType::lever: return std::format("minecraft:lever[powered={}]", b(s.flag()));
    case BlockType::redstone_wire: return std::format("minecraft:redstone_wire[power={}]", s.level());
    case BlockType::repeater:
        return std::format("minecraft:repeater[delay={},facing={},powered={}]", s.delay(),
                           direction_name(contracts::opposite(s.facing())), b(s.flag()));
    case BlockType::redstone_lamp: return std::format("minecraft:redstone_lamp[lit={}]", b(s.flag()));
    case BlockType::observer:
        return std::format("minecraft:observer[facing={},powered={}]", direction_name(s.facing()), b(s.flag()));
    case BlockType::piston:
    case BlockType::sticky_piston:
        return std::format("minecraft:{}[extended={},facing={}]", s.is(BlockType::piston) ? "piston" : "sticky_piston",
                           b(s.flag()), direction_name(s.facing()));
    case BlockType::piston_head:
        return std::format("minecraft:piston_head[facing={},type={}]", direction_name(s.facing()), s.flag() ? "sticky" : "normal");
    case BlockType::moving_piston:
        return std::format("minecraft:moving_piston[facing={},type={}]", direction_name(s.facing()), s.flag() ? "sticky" : "normal");
    case BlockType::hopper:
        return std::format("minecraft:hopper[enabled={},facing={}]", b(s.flag()), direction_name(s.facing()));
    case BlockType::chest: {
        static constexpr std::string_view types[]{"single", "left", "right"};
        return std::format("minecraft:chest[facing={},type={}]", direction_name(s.facing()), types[s.level() % 3]);
    }
    case BlockType::comparator:
        return std::format("minecraft:comparator[facing={},mode={},powered={}]", direction_name(contracts::opposite(s.facing())),
                           s.level() == 1 ? "subtract" : "compare", b(s.flag()));
    case BlockType::water:
        return std::format("minecraft:water[level={}]", s.flag() ? 8 : s.level());
    case BlockType::wheat: return std::format("minecraft:wheat[age={}]", s.level());
    default: return std::format("circuit:unrepresentable[raw={}]", s.raw());
    }
}

std::expected<std::uint16_t, std::string> ContentMapping::item(std::string_view id) const {
    for (const auto& [name, value] : item_ids)
        if (name == id) return value;
    return std::unexpected(std::format("unsupported: item `{}` is not in the reference registry", id));
}

std::string ContentMapping::item_name(std::uint16_t item) const {
    for (const auto& [name, value] : item_ids)
        if (value == item) return std::string(name);
    return std::format("circuit:item_{}", item);
}

#undef CIRCUIT_TRY
#undef CIRCUIT_CHECK

} // namespace circuit::evidence
