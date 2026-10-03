// SPDX-License-Identifier: AGPL-3.0-only
#include "circuit/plugin/api.hpp"

#include <cstdlib>
#include <string_view>

#ifdef CIRCUIT_PLUGIN_TEST_FIXTURE

#include <new>

namespace {

bool mode(std::string_view expected) noexcept {
    const auto* value = std::getenv("CIRCUIT_PLUGIN_TEST_MODE");
    return value && expected == value;
}

class Fixture final : public circuit::plugin::IPlugin {
public:
    ~Fixture() override { host_->log(circuit::plugin::LogLevel::Info, "fixture:destroy"); }

    bool on_load(circuit::plugin::IHost& host) noexcept override {
        host_ = &host;
        host_->log(circuit::plugin::LogLevel::Info, "fixture:load");
        return !mode("init-failure");
    }

    void on_tick() noexcept override {
        host_->log(circuit::plugin::LogLevel::Info, "fixture:tick");
    }

    void on_unload() noexcept override {
        host_->log(circuit::plugin::LogLevel::Info, "fixture:unload");
    }

private:
    circuit::plugin::IHost* host_{};
};

} // namespace

CIRCUIT_PLUGIN_EXPORT const circuit::plugin::PluginDescriptor*
circuit_plugin_descriptor() noexcept {
    static circuit::plugin::PluginDescriptor descriptor{};
    descriptor = {
        mode("small-descriptor") ? 0U : static_cast<std::uint32_t>(sizeof(circuit::plugin::PluginDescriptor)),
        mode("wrong-version") ? 99U : circuit::plugin::api_version,
        mode("wrong-abi") ? "incompatible-toolchain" : circuit::plugin::native_abi_tag,
        "test.fixture",
        mode("mod") ? circuit::plugin::PluginKind::Mod : circuit::plugin::PluginKind::Plugin,
    };
    return mode("null-descriptor") ? nullptr : &descriptor;
}

CIRCUIT_PLUGIN_EXPORT circuit::plugin::IPlugin* circuit_plugin_create() noexcept {
    return mode("factory-failure") ? nullptr : new (std::nothrow) Fixture;
}

CIRCUIT_PLUGIN_EXPORT void circuit_plugin_destroy(circuit::plugin::IPlugin* plugin) noexcept {
    delete static_cast<Fixture*>(plugin);
}

#else

#include "circuit/plugin/loader.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

class Host final : public circuit::plugin::IHost {
public:
    void log(circuit::plugin::LogLevel, std::string_view message) noexcept override {
        messages.emplace_back(message);
    }
    circuit::plugin::TickMetrics tick_metrics() const noexcept override { return metrics; }

    circuit::plugin::TickMetrics metrics{19, 2'500'000, 1'250'000};
    std::vector<std::string> messages;
};

void select_mode(const char* mode) {
    check(setenv("CIRCUIT_PLUGIN_TEST_MODE", mode, 1) == 0, "setenv failed");
}

} // namespace

int main(int argc, char** argv) {
    using namespace circuit::plugin;
    if (argc != 3) {
        std::cerr << "usage: plugin_tests <technical_tools.so> <plugin_fixture.so>\n";
        return 2;
    }
    try {
        Host host;
        {
            auto tools = PluginLoader::load(argv[1], host, LoadPhase::Running);
            check(tools.has_value(), "technical tools failed to load");
            check(tools->id() == "circuit.technical-tools", "wrong plugin identity");
            tools->tick();
            check(host.messages.size() == 1, "metrics logged before the sample interval");
            host.metrics.completed_ticks = 20;
            tools->tick();
            tools->tick();
            check(host.messages.size() == 2, "sample duplicated or missing");
            check(host.messages.back().find("last=2.500ms mean=1.250ms") != std::string::npos,
                "tick metrics did not reach the plugin correctly");
        }
        check(host.messages.back() == "technical-tools: unloaded", "plugin did not unload");

        select_mode("valid");
        host.messages.clear();
        {
            auto fixture = PluginLoader::load(argv[2], host, LoadPhase::Running);
            check(fixture.has_value(), "fixture failed to load");
            auto moved = std::move(*fixture);
            fixture->tick(); // A moved-from handle is inert.
            moved.tick();
        }
        check(host.messages == std::vector<std::string>{
            "fixture:load", "fixture:tick", "fixture:unload", "fixture:destroy"},
            "load/move/tick/unload/module-owned destruction order is incorrect");

        struct Rejection { const char* mode; LoadErrorCode error; };
        constexpr Rejection rejections[]{
            {"small-descriptor", LoadErrorCode::InvalidDescriptor},
            {"null-descriptor", LoadErrorCode::InvalidDescriptor},
            {"wrong-version", LoadErrorCode::VersionMismatch},
            {"wrong-abi", LoadErrorCode::AbiMismatch},
            {"mod", LoadErrorCode::RegistrationClosed},
            {"factory-failure", LoadErrorCode::CreationFailed},
            {"init-failure", LoadErrorCode::InitializationFailed},
        };
        for (const auto& rejection : rejections) {
            select_mode(rejection.mode);
            host.messages.clear();
            auto result = PluginLoader::load(argv[2], host, LoadPhase::Running);
            check(!result, "invalid plugin was accepted");
            check(result.error().code == rejection.error, "wrong plugin rejection reason");
            if (rejection.error == LoadErrorCode::InitializationFailed) {
                check(host.messages == std::vector<std::string>{
                    "fixture:load", "fixture:unload", "fixture:destroy"},
                    "failed initialization leaked its lifecycle");
            } else {
                check(host.messages.empty(), "rejected module executed plugin callbacks");
            }
        }

        select_mode("mod");
        {
            auto mod = PluginLoader::load(argv[2], host, LoadPhase::Registration);
            check(mod.has_value(), "mod was rejected during registration");
            check(mod->kind() == PluginKind::Mod, "mod kind was not preserved");
        }
        unsetenv("CIRCUIT_PLUGIN_TEST_MODE");
        std::cout << "native plugin loading, ABI rejection and lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        unsetenv("CIRCUIT_PLUGIN_TEST_MODE");
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#endif
