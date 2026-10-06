// SPDX-License-Identifier: AGPL-3.0-only
#include "log.hpp"
#include <chrono>
#include <format>
#include <iostream>
#include <mutex>

namespace circuit::server {
namespace {
std::mutex mutex;
void write(std::string_view level, std::string_view message) {
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    const auto line = std::format("[{:%H:%M:%S}] [{}] {}\n", now, level, message);
    std::lock_guard lock(mutex);
    std::cerr << line << std::flush;
}
} // namespace

void log_info(std::string_view message) { write("INFO", message); }
void log_warn(std::string_view message) { write("WARN", message); }
void log_error(std::string_view message) { write("ERROR", message); }

} // namespace circuit::server
