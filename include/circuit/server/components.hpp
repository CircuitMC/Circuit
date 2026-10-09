// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/version.hpp>
#include <circuit/protocol/components.hpp>
#include <cstdint>

namespace circuit::server {

// What item components may refer to in a version bundle served by Circuit:
// its item and block registries and the registries the server synchronizes,
// with their entry names.
protocol::ComponentContext component_context(const contracts::VersionBundle& bundle);
// The same for the bundle that plays a protocol.
protocol::ComponentContext component_context(std::int32_t protocol);

} // namespace circuit::server
