// SPDX-License-Identifier: AGPL-3.0-only
#include "circuit/plugin/api.hpp"

#include <cstdio>
#include <new>

namespace {

class TechnicalTools final : public circuit::plugin::IPlugin {
public:
    bool on_load(circuit::plugin::IHost& host) noexcept override {
        host_ = &host;
        host_->log(circuit::plugin::LogLevel::Info, "technical-tools: loaded");
        return true;
    }

    void on_tick() noexcept override {
        const auto metrics = host_->tick_metrics();
        // One sample per 20 completed ticks; avoids duplicate control callbacks.
        if (metrics.completed_ticks == 0 || metrics.completed_ticks % 20 != 0
            || metrics.completed_ticks == last_reported_tick_) {
            return;
        }
        last_reported_tick_ = metrics.completed_ticks;
        char message[192];
        std::snprintf(message, sizeof(message),
            "technical-tools: tick=%llu last=%.3fms mean=%.3fms",
            static_cast<unsigned long long>(metrics.completed_ticks),
            static_cast<double>(metrics.last_tick_ns) / 1'000'000.0,
            static_cast<double>(metrics.mean_tick_ns) / 1'000'000.0);
        host_->log(circuit::plugin::LogLevel::Info, message);
    }

    void on_unload() noexcept override {
        if (host_) {
            host_->log(circuit::plugin::LogLevel::Info, "technical-tools: unloaded");
            host_ = nullptr;
        }
    }

private:
    circuit::plugin::IHost* host_{};
    std::uint64_t last_reported_tick_{};
};

constexpr circuit::plugin::PluginDescriptor descriptor{
    sizeof(circuit::plugin::PluginDescriptor),
    circuit::plugin::api_version,
    circuit::plugin::native_abi_tag,
    "circuit.technical-tools",
    circuit::plugin::PluginKind::Plugin,
};

} // namespace

CIRCUIT_PLUGIN_EXPORT const circuit::plugin::PluginDescriptor*
circuit_plugin_descriptor() noexcept {
    return &descriptor;
}

CIRCUIT_PLUGIN_EXPORT circuit::plugin::IPlugin* circuit_plugin_create() noexcept {
    return new (std::nothrow) TechnicalTools;
}

CIRCUIT_PLUGIN_EXPORT void circuit_plugin_destroy(circuit::plugin::IPlugin* plugin) noexcept {
    delete static_cast<TechnicalTools*>(plugin);
}
