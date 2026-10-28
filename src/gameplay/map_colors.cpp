// SPDX-License-Identifier: AGPL-3.0-only
// How held maps draw block states (tests/fixtures/maps/colors-*.txt: every
// state the probe could place, on both official servers), from the table
// tools/compile_map_colors.py compiles.
#include "internal.hpp"
#include "map_colors_data.hpp"
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/maps.hpp>
#include <algorithm>
#include <charconv>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace circuit::gameplay {
namespace {

const map_color_data::Entry* entry_of(std::string_view block, std::string_view release) {
    const map_color_data::Entry* any = nullptr;
    for (std::size_t i = 0; i < map_color_data::entry_count; ++i) {
        const auto& e = map_color_data::entries[i];
        if (e.block != block) continue;
        if (e.release == release) return &e;
        if (std::string_view(e.release).empty()) any = &e;
    }
    return any;
}

bool listed(const char* const* names, std::size_t count, std::string_view name) {
    return std::any_of(names, names + count, [&](const char* n) { return n == name; });
}

// The color of a registry state without its water.
std::uint8_t dry_color(const map_color_data::Entry& e, const std::vector<registry::Property>& props) {
    if (!e.property) return e.color;
    for (const auto& [key, value] : props) {
        if (key != e.property) continue;
        std::string_view values = e.values;
        while (!values.empty()) {
            const auto comma = std::min(values.find(','), values.size());
            const auto item = values.substr(0, comma);
            const auto eq = item.find('=');
            if (item.substr(0, eq) == value) {
                std::uint8_t c{};
                std::from_chars(item.data() + eq + 1, item.data() + item.size(), c);
                return c;
            }
            values.remove_prefix(std::min(values.size(), comma + 1));
        }
    }
    return e.color;
}

MapLook registry_look(std::uint32_t state, std::string_view release) {
    const auto block = registry::block_of(state);
    if (!block) return {};
    const auto name = registry::game_name(*block);
    const auto* e = entry_of(name, release);
    if (!e) return {};
    const auto props = registry::properties(state);
    MapLook look{dry_color(*e, props), detail::holds_water_fluid(BlockState::registered(state))};
    const bool waterlogged = std::ranges::any_of(props, [](const auto& p) { return p.first == "waterlogged" && p.second == "true"; });
    if (waterlogged) {
        const bool wet = listed(map_color_data::always_water, map_color_data::always_water_count, name)
                      || (!listed(map_color_data::never_water, map_color_data::never_water_count, name)
                          && !registry::sturdy_face(state, contracts::Direction::up, release));
        if (wet) look.color = map_water;
    }
    return look;
}

} // namespace

MapLooks::MapLooks(std::string_view release) {
    registry_.resize(registry::state_count() + 1);
    for (std::uint32_t id = 1; id <= registry::state_count(); ++id) registry_[id] = registry_look(id, release);
    for_each_modeled_state([&](BlockState s) {
        if (const auto id = registry_equivalent(s)) {
            auto look = registry_[*id];
            look.water = detail::holds_water_fluid(s);
            modeled_.emplace(s.raw(), look);
        }
    });
}

MapLook MapLooks::of(BlockState s) const {
    if (s.is(BlockType::other)) return s.registry_state() < registry_.size() ? registry_[s.registry_state()] : MapLook{};
    const auto it = modeled_.find(s.raw());
    return it == modeled_.end() ? MapLook{} : it->second;
}

MapLook MapLooks::of_registry(std::uint32_t state) const { return state < registry_.size() ? registry_[state] : MapLook{}; }

const MapLooks& map_looks(std::string_view release) {
    static std::mutex mutex;
    static std::map<std::string, std::unique_ptr<MapLooks>, std::less<>> looks;
    const std::lock_guard lock(mutex);
    auto it = looks.find(release);
    if (it == looks.end()) it = looks.emplace(std::string(release), std::make_unique<MapLooks>(release)).first;
    return *it->second;
}

} // namespace circuit::gameplay
