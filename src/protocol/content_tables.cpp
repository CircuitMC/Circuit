// SPDX-License-Identifier: AGPL-3.0-only
#include "content_tables.hpp"
#include <algorithm>
#include <unordered_map>

namespace circuit::protocol {

struct ContentTables::Index {
    std::unordered_map<std::string_view, std::size_t> blocks;
    std::unordered_map<std::string_view, std::int32_t> items;
    std::unordered_map<std::string_view, std::int32_t> entities;
};

ContentTables::ContentTables(const Table& table) : table_(table), index_(new Index) {
    for (std::size_t i = 0; i < table.blocks.size(); ++i) index_->blocks.emplace(table.blocks[i].name, i);
    for (const auto& it : table.items) index_->items.emplace(it.name, it.id);
    for (const auto& e : table.entities) index_->entities.emplace(e.name, e.id);
}
ContentTables::~ContentTables() { delete index_; }

const ContentTables* ContentTables::for_protocol(std::int32_t protocol) noexcept {
    static const ContentTables v772(detail::content_772), v769(detail::content_769);
    switch (protocol) {
    case 772: return &v772;
    case 769: return &v769;
    default: return nullptr;
    }
}

std::int32_t ContentTables::protocol() const noexcept { return table_.protocol; }
std::string_view ContentTables::source_commit() const noexcept { return table_.commit; }
std::int32_t ContentTables::state_count() const noexcept { return table_.states; }

std::optional<std::int32_t> ContentTables::state_id(std::string_view block, const std::vector<Property>& properties) const {
    const auto name = block.find(':') == std::string_view::npos ? std::string("minecraft:") + std::string(block) : std::string(block);
    const auto it = index_->blocks.find(name);
    if (it == index_->blocks.end()) return std::nullopt;
    const auto& b = table_.blocks[it->second];
    // Start from the default state's value indices.
    std::vector<std::size_t> index(b.properties.size());
    {
        auto rest = static_cast<std::size_t>(b.default_state - b.min_state);
        for (std::size_t i = b.properties.size(); i-- > 0;) {
            index[i] = rest % b.properties[i].values.size();
            rest /= b.properties[i].values.size();
        }
    }
    for (const auto& [key, value] : properties) {
        const auto p = std::ranges::find(b.properties, key, &Table::PropertyDef::name);
        if (p == b.properties.end()) return std::nullopt;
        const auto v = std::ranges::find(p->values, value);
        if (v == p->values.end()) return std::nullopt;
        index[static_cast<std::size_t>(p - b.properties.begin())] = static_cast<std::size_t>(v - p->values.begin());
    }
    std::int64_t offset = 0;
    for (std::size_t i = 0; i < b.properties.size(); ++i)
        offset = offset * static_cast<std::int64_t>(b.properties[i].values.size()) + static_cast<std::int64_t>(index[i]);
    return b.min_state + static_cast<std::int32_t>(offset);
}

std::optional<std::int32_t> ContentTables::state_id(std::string_view notation) const {
    const auto parsed = parse_notation(notation);
    if (!parsed) return std::nullopt;
    return state_id(parsed->first, parsed->second);
}

std::optional<std::int32_t> ContentTables::default_state(std::string_view block) const {
    const auto it = index_->blocks.find(block);
    if (it == index_->blocks.end()) return std::nullopt;
    return table_.blocks[it->second].default_state;
}

std::optional<BlockStateInfo> ContentTables::state(std::int32_t id) const {
    if (id < 0 || id >= table_.states) return std::nullopt;
    const auto it = std::ranges::upper_bound(table_.blocks, id, {}, &Table::Block::min_state);
    const auto& b = *std::prev(it);
    BlockStateInfo info{b.name, {}};
    auto rest = static_cast<std::size_t>(id - b.min_state);
    std::vector<Property> props(b.properties.size());
    for (std::size_t i = b.properties.size(); i-- > 0;) {
        const auto& p = b.properties[i];
        props[i] = {std::string(p.name), std::string(p.values[rest % p.values.size()])};
        rest /= p.values.size();
    }
    info.properties = std::move(props);
    return info;
}

std::optional<std::string> ContentTables::notation(std::int32_t id) const {
    const auto s = state(id);
    if (!s) return std::nullopt;
    std::string out(s->block);
    if (!s->properties.empty()) {
        out += '[';
        for (std::size_t i = 0; i < s->properties.size(); ++i) {
            if (i) out += ',';
            out += s->properties[i].first + "=" + s->properties[i].second;
        }
        out += ']';
    }
    return out;
}

std::optional<std::int32_t> ContentTables::item_id(std::string_view name) const {
    const auto full = name.find(':') == std::string_view::npos ? std::string("minecraft:") + std::string(name) : std::string(name);
    const auto it = index_->items.find(full);
    if (it == index_->items.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string_view> ContentTables::item_name(std::int32_t id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= table_.items.size()) return std::nullopt;
    return table_.items[static_cast<std::size_t>(id)].name;
}

std::optional<std::int32_t> ContentTables::entity_type(std::string_view name) const {
    const auto full = name.find(':') == std::string_view::npos ? std::string("minecraft:") + std::string(name) : std::string(name);
    const auto it = index_->entities.find(full);
    if (it == index_->entities.end()) return std::nullopt;
    return it->second;
}

std::optional<std::pair<std::string, std::vector<Property>>> parse_notation(std::string_view notation) {
    const auto bracket = notation.find('[');
    std::string block(notation.substr(0, bracket));
    if (block.empty()) return std::nullopt;
    if (block.find(':') == std::string::npos) block = "minecraft:" + block;
    std::vector<Property> props;
    if (bracket != std::string_view::npos) {
        if (!notation.ends_with(']')) return std::nullopt;
        auto body = notation.substr(bracket + 1, notation.size() - bracket - 2);
        while (!body.empty()) {
            const auto comma = body.find(',');
            const auto pair = body.substr(0, comma);
            const auto eq = pair.find('=');
            if (eq == std::string_view::npos || eq == 0 || eq + 1 == pair.size()) return std::nullopt;
            props.emplace_back(std::string(pair.substr(0, eq)), std::string(pair.substr(eq + 1)));
            if (comma == std::string_view::npos) break;
            body.remove_prefix(comma + 1);
        }
    }
    return std::pair{block, props};
}

} // namespace circuit::protocol
