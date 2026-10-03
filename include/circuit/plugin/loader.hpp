// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "circuit/plugin/api.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace circuit::plugin {

enum class LoadPhase { Registration, Running };
enum class LoadErrorCode {
    OpenFailed,
    MissingSymbol,
    InvalidDescriptor,
    VersionMismatch,
    AbiMismatch,
    RegistrationClosed,
    CreationFailed,
    InitializationFailed,
};

struct LoadError {
    LoadErrorCode code;
    std::string message;
};

class PluginLoader;

class LoadedPlugin {
public:
    LoadedPlugin(LoadedPlugin&&) noexcept;
    LoadedPlugin& operator=(LoadedPlugin&&) noexcept;
    ~LoadedPlugin();

    LoadedPlugin(const LoadedPlugin&) = delete;
    LoadedPlugin& operator=(const LoadedPlugin&) = delete;

    // Must be serialized with loading/unloading and all other callbacks.
    void tick() noexcept;
    [[nodiscard]] std::string_view id() const noexcept;
    [[nodiscard]] PluginKind kind() const noexcept;

private:
    struct State;
    explicit LoadedPlugin(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
    friend class PluginLoader;
};

class PluginLoader {
public:
    // Native plugins are trusted process code, not a sandbox. Descriptor checks
    // cannot protect against malicious shared-library constructors or pointers.
    [[nodiscard]] static std::expected<LoadedPlugin, LoadError> load(
        const std::filesystem::path& library, IHost& host, LoadPhase phase);
};

} // namespace circuit::plugin
