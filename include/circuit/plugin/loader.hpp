// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "circuit/plugin/api.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

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
    DuplicateId,
    MissingDependency,
    DependencyCycle,
    GameVersionMismatch,
};

struct LoadError {
    LoadErrorCode code;
    std::string message;
};

// What a module's descriptor declares.
struct ModuleInfo {
    std::string id;
    PluginKind kind{PluginKind::Plugin};
    std::vector<std::string> dependencies;
    std::vector<std::string> game_versions; // empty: any
};

class PluginLoader;

// A library that is open and whose descriptor was checked, but whose module
// was not created yet.
class OpenedModule {
public:
    OpenedModule(OpenedModule&&) noexcept;
    OpenedModule& operator=(OpenedModule&&) noexcept;
    ~OpenedModule();
    OpenedModule(const OpenedModule&) = delete;
    OpenedModule& operator=(const OpenedModule&) = delete;

    [[nodiscard]] const ModuleInfo& info() const noexcept;

private:
    struct State;
    explicit OpenedModule(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
    friend class PluginLoader;
    friend class LoadedPlugin;
};

class LoadedPlugin {
public:
    LoadedPlugin(LoadedPlugin&&) noexcept;
    LoadedPlugin& operator=(LoadedPlugin&&) noexcept;
    ~LoadedPlugin();

    LoadedPlugin(const LoadedPlugin&) = delete;
    LoadedPlugin& operator=(const LoadedPlugin&) = delete;

    // Must be serialized with loading/unloading and all other callbacks.
    void tick() noexcept;
    void tick_report(const TickReport& report) noexcept;
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
    [[nodiscard]] static std::expected<OpenedModule, LoadError> open(const std::filesystem::path& library);
    // Creates and initializes an opened module. Mods start only in the
    // registration phase.
    [[nodiscard]] static std::expected<LoadedPlugin, LoadError> start(OpenedModule module, IHost& host, LoadPhase phase);
    [[nodiscard]] static std::expected<LoadedPlugin, LoadError> load(const std::filesystem::path& library, IHost& host,
                                                                     LoadPhase phase);
};

// An order in which every module comes after its dependencies (ties by id).
// Dependencies may also name modules in `loaded`. Fails for a duplicate id, a
// missing dependency, a cycle, or a module that does not support `bundle_id`.
[[nodiscard]] std::expected<std::vector<std::size_t>, LoadError> load_order(std::span<const ModuleInfo> modules,
                                                                              std::string_view bundle_id,
                                                                              std::span<const std::string> loaded = {});

} // namespace circuit::plugin
