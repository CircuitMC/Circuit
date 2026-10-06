// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <string_view>

// Line-oriented server log on standard error; safe to call from any thread.
namespace circuit::server {

void log_info(std::string_view message);
void log_warn(std::string_view message);
void log_error(std::string_view message);

} // namespace circuit::server
