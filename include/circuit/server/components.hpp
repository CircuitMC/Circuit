// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/protocol/components.hpp>
#include <cstdint>

namespace circuit::server {

// What item components may refer to on a protocol served by Circuit: its
// item and block registries and the registries the server synchronizes.
protocol::ComponentContext component_context(std::int32_t protocol);

} // namespace circuit::server
