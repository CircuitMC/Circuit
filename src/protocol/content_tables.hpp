// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/protocol/content.hpp>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace circuit::protocol {

struct ContentTables::Table {
    struct PropertyDef {
        std::string_view name;
        std::vector<std::string_view> values;
    };
    struct Block {
        std::string_view name;
        std::int32_t min_state;
        std::int32_t default_state;
        std::vector<PropertyDef> properties;
        BlockShape shape;
    };
    struct Named {
        std::int32_t id;
        std::string_view name;
    };
    std::int32_t protocol;
    std::string_view commit;
    std::int32_t states;
    std::vector<Block> blocks; // sorted by min_state
    std::vector<Named> items;  // sorted by id, contiguous from 0
    std::vector<Named> entities;
};

namespace detail {
using ContentTable = ContentTables::Table;
extern const ContentTable content_772;
extern const ContentTable content_769;
} // namespace detail

} // namespace circuit::protocol
