// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/protocol/nbt.hpp>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace circuit::server {

// One synchronized registry sent during configuration, as entry identifiers
// in id order.
struct RegistryEntries {
    std::string_view registry;
    std::vector<std::string_view> entries;
};

// A registry sent with entry data, for clients that do not share the vanilla
// data pack (protocol libraries, bots). Each list is a prefix of the
// corresponding synchronized registry, so numeric ids agree for all clients.
struct RegistryData {
    std::string_view registry;
    std::vector<std::pair<std::string_view, protocol::nbt::Tag>> entries;
};

// Tags a client needs bound for the registries it receives, as entry names.
struct RegistryTags {
    std::string_view registry;
    std::vector<std::pair<std::string_view, std::vector<std::string_view>>> tags;
};

// nullptr for undocumented protocol versions.
const std::vector<RegistryEntries>* synced_registries(std::int32_t protocol) noexcept;
const std::vector<RegistryData>* registries_with_data(std::int32_t protocol);
// Tags sent with the registries (empty when the client's pack needs none).
const std::vector<RegistryTags>& synced_tags(std::int32_t protocol);
std::optional<std::int32_t> registry_index(const std::vector<RegistryEntries>& registries, std::string_view registry,
                                           std::string_view entry) noexcept;

} // namespace circuit::server
