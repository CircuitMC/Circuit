// SPDX-License-Identifier: AGPL-3.0-only
#include "block_registry_data.hpp"
#include <circuit/gameplay/block_registry.hpp>
#include <algorithm>
#include <unordered_map>

namespace circuit::gameplay::registry {
namespace {
const data::VersionTable* version(std::string_view release) noexcept {
    for (std::size_t i = 0; i < data::version_count; ++i)
        if (data::versions[i].release == release) return &data::versions[i];
    return nullptr;
}

// Per release, the internal block of each game name. Built once.
const std::unordered_map<std::string_view, std::uint32_t>& names(const data::VersionTable& v) {
    static const auto all = [] {
        std::vector<std::unordered_map<std::string_view, std::uint32_t>> out(data::version_count);
        for (std::size_t i = 0; i < data::version_count; ++i)
            for (std::uint32_t b = 0; b < data::versions[i].block_count; ++b) {
                const auto block = data::versions[i].blocks[b].block;
                out[i].emplace(game_name(block), block);
            }
        return out;
    }();
    return all[static_cast<std::size_t>(&v - data::versions)];
}

// Per release, its wire block of each internal block it has. Built once.
const std::unordered_map<std::uint32_t, std::uint32_t>& wire_blocks(const data::VersionTable& v) {
    static const auto all = [] {
        std::vector<std::unordered_map<std::uint32_t, std::uint32_t>> out(data::version_count);
        for (std::size_t i = 0; i < data::version_count; ++i)
            for (std::uint32_t b = 0; b < data::versions[i].block_count; ++b) out[i].emplace(data::versions[i].blocks[b].block, b);
        return out;
    }();
    return all[static_cast<std::size_t>(&v - data::versions)];
}

// Value index of each property of a state, last property varying fastest.
std::vector<std::uint16_t> value_indices(const data::BlockData& b, std::uint32_t offset) {
    std::vector<std::uint16_t> index(b.property_count);
    for (std::size_t i = b.property_count; i-- > 0;) {
        const auto count = data::properties[b.first_property + i].value_count;
        index[i] = static_cast<std::uint16_t>(offset % count);
        offset /= count;
    }
    return index;
}
} // namespace

std::string_view id() noexcept { return data::registry_id; }
std::uint32_t state_count() noexcept { return data::state_count; }
std::uint32_t block_count() noexcept { return data::block_count; }

BlockInfo block(std::uint32_t index) noexcept {
    const auto& b = data::blocks[index];
    return {b.name, b.first_state, b.state_count, b.first_state + b.default_offset, b.hardness, b.opaque != 0,
            static_cast<Tool>(b.tool), b.tier, b.resistance, static_cast<PushClass>(b.push),
            static_cast<Instrument>(b.instrument), static_cast<Conductor>(b.conductor), static_cast<Connector>(b.connector & 7)};
}

std::optional<std::uint32_t> block_of(std::uint32_t state) noexcept {
    if (state < 1 || state > data::state_count) return std::nullopt;
    const auto* end = data::blocks + data::block_count;
    const auto it = std::upper_bound(data::blocks, end, state, [](std::uint32_t s, const data::BlockData& b) { return s < b.first_state; });
    return static_cast<std::uint32_t>(it - data::blocks - 1);
}

std::string_view game_name(std::uint32_t block) noexcept {
    const std::string_view name = data::blocks[block].name;
    return name.substr(0, name.find('@'));
}

std::vector<std::string_view> releases() {
    std::vector<std::string_view> out;
    for (std::size_t i = 0; i < data::version_count; ++i) out.emplace_back(data::versions[i].release);
    return out;
}

std::optional<std::uint32_t> find_block(std::string_view release, std::string_view name) {
    const auto* v = version(release);
    if (!v) return std::nullopt;
    if (name.starts_with("minecraft:")) name.remove_prefix(10);
    const auto& index = names(*v);
    const auto it = index.find(name);
    if (it == index.end()) return std::nullopt;
    return it->second;
}

std::vector<Property> properties(std::uint32_t state) {
    const auto b = block_of(state);
    if (!b) return {};
    const auto& data = data::blocks[*b];
    const auto index = value_indices(data, state - data.first_state);
    std::vector<Property> out;
    for (std::size_t i = 0; i < data.property_count; ++i) {
        const auto& p = data::properties[data.first_property + i];
        out.emplace_back(p.name, data::values[p.first_value + index[i]]);
    }
    return out;
}

std::string notation(std::uint32_t state) {
    const auto b = block_of(state);
    if (!b) return "minecraft:air";
    auto props = properties(state);
    std::ranges::sort(props);
    std::string out = "minecraft:" + std::string(game_name(*b));
    if (!props.empty()) {
        out += '[';
        for (std::size_t i = 0; i < props.size(); ++i) {
            if (i) out += ',';
            out += props[i].first + '=' + props[i].second;
        }
        out += ']';
    }
    return out;
}

std::optional<std::uint32_t> state_of(std::uint32_t block, std::span<const Property> properties) {
    if (block >= data::block_count) return std::nullopt;
    const auto& b = data::blocks[block];
    auto index = value_indices(b, b.default_offset);
    for (const auto& [key, value] : properties) {
        std::size_t i = 0;
        while (i < b.property_count && data::properties[b.first_property + i].name != key) ++i;
        if (i == b.property_count) return std::nullopt;
        const auto& p = data::properties[b.first_property + i];
        std::uint16_t v = 0;
        while (v < p.value_count && data::values[p.first_value + v] != value) ++v;
        if (v == p.value_count) return std::nullopt;
        index[i] = v;
    }
    std::uint32_t offset = 0;
    for (std::size_t i = 0; i < b.property_count; ++i) offset = offset * data::properties[b.first_property + i].value_count + index[i];
    return b.first_state + offset;
}

Collision collision(std::uint32_t state) noexcept {
    if (state < 1 || state > data::state_count) return Collision::empty;
    return static_cast<Collision>(data::shapes[state - 1] & 3);
}

bool conducts(std::uint32_t state) noexcept {
    const auto b = block_of(state);
    if (!b) return false;
    switch (static_cast<Conductor>(data::blocks[*b].conductor)) {
    case Conductor::always: return true;
    case Conductor::full_cube: return collision(state) == Collision::full;
    default: return false;
    }
}

bool face_covered(std::uint32_t state, contracts::Direction face) noexcept {
    if (state < 1 || state > data::state_count) return false;
    return (data::shapes[state - 1] >> (2 + static_cast<unsigned>(face))) & 1;
}

bool joins_face(std::uint32_t state, contracts::Direction face) noexcept {
    if (state < 1 || state > data::state_count) return false;
    return (data::join_faces[state - 1] >> static_cast<unsigned>(face)) & 1;
}

Connector connector(std::uint32_t state) noexcept {
    const auto b = block_of(state);
    return b ? static_cast<Connector>(data::blocks[*b].connector & 7) : Connector::none;
}

bool keeps_wall_post(std::uint32_t state) noexcept {
    const auto b = block_of(state);
    return b && (data::blocks[*b].connector & 8) != 0;
}

namespace {
std::uint8_t support_of(std::uint32_t state, std::string_view release) noexcept {
    for (std::size_t i = 0; i < data::support_override_count; ++i)
        if (data::support_overrides[i].state == state && release == data::support_overrides[i].release) return data::support_overrides[i].support;
    return data::support[state - 1];
}
} // namespace

bool supports_middle(std::uint32_t state, contracts::Direction face, std::string_view release) noexcept {
    if (state < 1 || state > data::state_count) return false;
    if (face == contracts::Direction::up) return (support_of(state, release) & 1) != 0;
    if (face == contracts::Direction::down) return (support_of(state, release) & 2) != 0;
    return false;
}

bool sturdy_face(std::uint32_t state, contracts::Direction face, std::string_view release) noexcept {
    if (state < 1 || state > data::state_count) return false;
    return (support_of(state, release) >> (2 + static_cast<unsigned>(face))) & 1;
}

bool soil(std::uint32_t state) noexcept {
    const auto b = block_of(state);
    return b && (data::blocks[*b].connector & 16) != 0;
}

std::uint8_t light_emission(std::uint32_t state) noexcept {
    if (state < 1 || state > data::state_count) return 0;
    return data::light[state - 1] & 15;
}

std::span<const Box> collision_boxes(std::uint32_t state, std::string_view release) noexcept {
    if (state < 1 || state > data::state_count) return {};
    auto shape = data::collision_shape[state - 1];
    for (std::size_t i = 0; i < data::collision_override_count; ++i)
        if (data::collision_overrides[i].state == state && release == data::collision_overrides[i].release) shape = data::collision_overrides[i].shape;
    const auto& s = data::collision_shapes[shape];
    return {data::collision_boxes + s.first_box, s.box_count};
}

bool motion_blocking(std::uint32_t state) noexcept {
    if (state < 1 || state > data::state_count) return false;
    return (data::light[state - 1] >> 6 & 1) != 0;
}

std::uint8_t light_dampening(std::uint32_t state) noexcept {
    if (state < 1 || state > data::state_count) return 15;
    static constexpr std::uint8_t values[4]{0, 1, 15, 15};
    return values[(data::light[state - 1] >> 4) & 3];
}

std::uint16_t light_shape(std::uint32_t state) noexcept {
    if (state < 1 || state > data::state_count) return 0;
    return data::light_shape[state - 1];
}

bool light_occluded(std::uint16_t from, std::uint16_t to, contracts::Direction side) noexcept {
    if (from > data::light_shape_count || to > data::light_shape_count) return true;
    const auto face = static_cast<std::size_t>(side);
    const auto back = static_cast<std::size_t>(contracts::opposite(side));
    for (std::size_t k = 0; k < 4; ++k)
        if ((data::light_faces[from].faces[face][k] | data::light_faces[to].faces[back][k]) != ~std::uint64_t{0}) return false;
    return true;
}

std::uint32_t light_shape_count() noexcept { return data::light_shape_count; }

std::array<std::uint64_t, 4> light_face(std::uint16_t shape, contracts::Direction face) noexcept {
    if (shape > data::light_shape_count) return {};
    const auto& f = data::light_faces[shape].faces[static_cast<std::size_t>(face)];
    return {f[0], f[1], f[2], f[3]};
}

std::uint32_t wire_state_count(std::string_view release) noexcept {
    const auto* v = version(release);
    return v ? v->state_count : 0;
}

std::optional<std::int32_t> to_wire(std::string_view release, std::uint32_t state) {
    const auto* v = version(release);
    const auto b = block_of(state);
    if (!v || !b) return std::nullopt;
    const auto& index = wire_blocks(*v);
    const auto it = index.find(*b);
    if (it == index.end()) return std::nullopt;
    return static_cast<std::int32_t>(v->blocks[it->second].first_wire + (state - data::blocks[*b].first_state));
}

std::optional<std::uint32_t> from_wire(std::string_view release, std::int32_t wire) {
    const auto* v = version(release);
    if (!v || wire < 0 || static_cast<std::uint32_t>(wire) >= v->state_count) return std::nullopt;
    const auto* end = v->blocks + v->block_count;
    const auto it = std::upper_bound(v->blocks, end, static_cast<std::uint32_t>(wire),
                                     [](std::uint32_t w, const data::VersionBlock& b) { return w < b.first_wire; });
    const auto& vb = *(it - 1);
    return data::blocks[vb.block].first_state + (static_cast<std::uint32_t>(wire) - vb.first_wire);
}

} // namespace circuit::gameplay::registry
