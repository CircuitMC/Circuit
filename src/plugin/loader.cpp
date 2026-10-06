// SPDX-License-Identifier: AGPL-3.0-only
#include "circuit/plugin/loader.hpp"

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <set>
#include <utility>

namespace circuit::plugin {

namespace {
struct Library {
    void* handle{};
    ~Library() {
        if (handle) dlclose(handle);
    }
};

bool valid_id(std::string_view id) {
    return !id.empty() && id.size() <= 64 &&
           std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'; });
}

std::vector<std::string> split(const char* list) {
    std::vector<std::string> out;
    if (!list) return out;
    std::string_view rest(list);
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        auto part = rest.substr(0, comma);
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
        if (!part.empty()) out.emplace_back(part);
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
    }
    return out;
}
} // namespace

struct OpenedModule::State {
    Library library;
    CreateFunction create{};
    DestroyFunction destroy{};
    ModuleInfo info;
};

OpenedModule::OpenedModule(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
OpenedModule::OpenedModule(OpenedModule&&) noexcept = default;
OpenedModule& OpenedModule::operator=(OpenedModule&&) noexcept = default;
OpenedModule::~OpenedModule() = default;
const ModuleInfo& OpenedModule::info() const noexcept { return state_->info; }

struct LoadedPlugin::State {
    std::unique_ptr<OpenedModule::State> module;
    IPlugin* instance{};
    bool initialization_attempted{};

    ~State() {
        if (instance) {
            if (initialization_attempted) instance->on_unload();
            module->destroy(instance); // Deallocate inside the module that allocated it.
        }
        // The library closes when `module` is destroyed, after the instance.
    }
};

LoadedPlugin::LoadedPlugin(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
LoadedPlugin::LoadedPlugin(LoadedPlugin&&) noexcept = default;
LoadedPlugin& LoadedPlugin::operator=(LoadedPlugin&&) noexcept = default;
LoadedPlugin::~LoadedPlugin() = default;

void LoadedPlugin::tick() noexcept {
    if (state_) state_->instance->on_tick();
}

void LoadedPlugin::tick_report(const TickReport& report) noexcept {
    if (state_) state_->instance->on_tick_report(report);
}

std::string_view LoadedPlugin::id() const noexcept { return state_ ? std::string_view(state_->module->info.id) : std::string_view{}; }

PluginKind LoadedPlugin::kind() const noexcept { return state_ ? state_->module->info.kind : PluginKind::Plugin; }

std::expected<OpenedModule, LoadError> PluginLoader::open(const std::filesystem::path& library) {
    auto state = std::make_unique<OpenedModule::State>();
    state->library.handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!state->library.handle) {
        const char* error = dlerror();
        return std::unexpected(LoadError{LoadErrorCode::OpenFailed, error ? error : "dlopen failed"});
    }
    const auto descriptor_function = reinterpret_cast<DescriptorFunction>(dlsym(state->library.handle, descriptor_symbol));
    state->create = reinterpret_cast<CreateFunction>(dlsym(state->library.handle, create_symbol));
    state->destroy = reinterpret_cast<DestroyFunction>(dlsym(state->library.handle, destroy_symbol));
    if (!descriptor_function || !state->create || !state->destroy)
        return std::unexpected(LoadError{LoadErrorCode::MissingSymbol, "module must export circuit_plugin_descriptor/create/destroy"});

    const auto* descriptor = descriptor_function();
    if (!descriptor || descriptor->structure_size < sizeof(PluginDescriptor))
        return std::unexpected(LoadError{LoadErrorCode::InvalidDescriptor, "missing or undersized module descriptor"});
    if (descriptor->version != api_version)
        return std::unexpected(LoadError{LoadErrorCode::VersionMismatch, "unsupported module API version"});
    if (!descriptor->abi_tag || std::strcmp(descriptor->abi_tag, native_abi_tag) != 0)
        return std::unexpected(LoadError{LoadErrorCode::AbiMismatch, "module and host native ABI tags differ"});
    if (!descriptor->id || !valid_id(descriptor->id) ||
        (descriptor->kind != PluginKind::Plugin && descriptor->kind != PluginKind::Mod))
        return std::unexpected(LoadError{LoadErrorCode::InvalidDescriptor, "module ID or kind is invalid"});
    ModuleInfo info{descriptor->id, descriptor->kind, {}, split(descriptor->game_versions)};
    for (auto d = descriptor->dependencies; d && *d; ++d) {
        if (!valid_id(*d) || info.dependencies.size() >= 64)
            return std::unexpected(LoadError{LoadErrorCode::InvalidDescriptor, "invalid dependency list"});
        info.dependencies.emplace_back(*d);
    }
    state->info = std::move(info);
    return OpenedModule(std::move(state));
}

std::expected<LoadedPlugin, LoadError> PluginLoader::start(OpenedModule module, IHost& host, LoadPhase phase) {
    auto state = std::make_unique<LoadedPlugin::State>();
    state->module = std::move(module.state_);
    if (state->module->info.kind == PluginKind::Mod && phase != LoadPhase::Registration)
        return std::unexpected(LoadError{LoadErrorCode::RegistrationClosed, "mods must load during startup registration"});
    state->instance = state->module->create();
    if (!state->instance) return std::unexpected(LoadError{LoadErrorCode::CreationFailed, "module factory returned null"});
    state->initialization_attempted = true;
    if (!state->instance->on_load(host))
        return std::unexpected(LoadError{LoadErrorCode::InitializationFailed, "module rejected initialization"});
    return LoadedPlugin(std::move(state));
}

std::expected<LoadedPlugin, LoadError> PluginLoader::load(const std::filesystem::path& library, IHost& host, LoadPhase phase) {
    auto opened = open(library);
    if (!opened) return std::unexpected(opened.error());
    return start(std::move(*opened), host, phase);
}

std::expected<std::vector<std::size_t>, LoadError> load_order(std::span<const ModuleInfo> modules, std::string_view bundle_id,
                                                              std::span<const std::string> loaded) {
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < modules.size(); ++i) {
        const auto& m = modules[i];
        if (!index.emplace(m.id, i).second || std::ranges::find(loaded, m.id) != loaded.end())
            return std::unexpected(LoadError{LoadErrorCode::DuplicateId, "module id " + m.id + " is used twice"});
        if (!m.game_versions.empty() && std::ranges::find(m.game_versions, bundle_id) == m.game_versions.end())
            return std::unexpected(LoadError{LoadErrorCode::GameVersionMismatch, "module " + m.id + " does not support " + std::string(bundle_id)});
    }
    // Kahn's algorithm; among ready modules, the smallest id first.
    std::vector<std::size_t> waiting(modules.size());
    std::vector<std::vector<std::size_t>> dependents(modules.size());
    for (std::size_t i = 0; i < modules.size(); ++i)
        for (const auto& d : modules[i].dependencies) {
            if (std::ranges::find(loaded, d) != loaded.end()) continue;
            const auto it = index.find(d);
            if (it == index.end())
                return std::unexpected(LoadError{LoadErrorCode::MissingDependency, "module " + modules[i].id + " needs " + d});
            ++waiting[i];
            dependents[it->second].push_back(i);
        }
    std::set<std::pair<std::string, std::size_t>> ready;
    for (std::size_t i = 0; i < modules.size(); ++i)
        if (waiting[i] == 0) ready.insert({modules[i].id, i});
    std::vector<std::size_t> order;
    while (!ready.empty()) {
        const auto [id, i] = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(i);
        for (const auto d : dependents[i])
            if (--waiting[d] == 0) ready.insert({modules[d].id, d});
    }
    if (order.size() != modules.size()) {
        std::string stuck;
        for (std::size_t i = 0; i < modules.size(); ++i)
            if (waiting[i] > 0) stuck += (stuck.empty() ? "" : ", ") + modules[i].id;
        return std::unexpected(LoadError{LoadErrorCode::DependencyCycle, "dependency cycle among " + stuck});
    }
    return order;
}

} // namespace circuit::plugin
