// SPDX-License-Identifier: AGPL-3.0-only
#include "circuit/plugin/loader.hpp"

#include <cstring>
#include <dlfcn.h>
#include <utility>

namespace circuit::plugin {

struct LoadedPlugin::State {
    void* library{};
    IPlugin* instance{};
    DestroyFunction destroy{};
    const PluginDescriptor* descriptor{};
    bool initialization_attempted{};

    ~State() {
        if (instance) {
            if (initialization_attempted) {
                instance->on_unload();
            }
            destroy(instance); // Deallocate inside the module that allocated it.
        }
        if (library) {
            dlclose(library);
        }
    }
};

LoadedPlugin::LoadedPlugin(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}
LoadedPlugin::LoadedPlugin(LoadedPlugin&&) noexcept = default;
LoadedPlugin& LoadedPlugin::operator=(LoadedPlugin&&) noexcept = default;
LoadedPlugin::~LoadedPlugin() = default;

void LoadedPlugin::tick() noexcept {
    if (state_) {
        state_->instance->on_tick();
    }
}

std::string_view LoadedPlugin::id() const noexcept {
    return state_ ? state_->descriptor->id : std::string_view{};
}

PluginKind LoadedPlugin::kind() const noexcept {
    return state_ ? state_->descriptor->kind : PluginKind::Plugin;
}

std::expected<LoadedPlugin, LoadError> PluginLoader::load(
    const std::filesystem::path& library, IHost& host, LoadPhase phase) {
    auto state = std::make_unique<LoadedPlugin::State>();
    state->library = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!state->library) {
        const char* error = dlerror();
        return std::unexpected(LoadError{LoadErrorCode::OpenFailed,
            error ? error : "dlopen failed"});
    }

    const auto descriptor_function = reinterpret_cast<DescriptorFunction>(
        dlsym(state->library, descriptor_symbol));
    const auto create = reinterpret_cast<CreateFunction>(
        dlsym(state->library, create_symbol));
    state->destroy = reinterpret_cast<DestroyFunction>(
        dlsym(state->library, destroy_symbol));
    if (!descriptor_function || !create || !state->destroy) {
        return std::unexpected(LoadError{LoadErrorCode::MissingSymbol,
            "module must export circuit_plugin_descriptor/create/destroy"});
    }

    state->descriptor = descriptor_function();
    const auto* descriptor = state->descriptor;
    if (!descriptor || descriptor->structure_size < sizeof(PluginDescriptor)) {
        return std::unexpected(LoadError{LoadErrorCode::InvalidDescriptor,
            "missing or undersized module descriptor"});
    }
    if (descriptor->version != api_version) {
        return std::unexpected(LoadError{LoadErrorCode::VersionMismatch,
            "unsupported module API version"});
    }
    if (!descriptor->abi_tag || std::strcmp(descriptor->abi_tag, native_abi_tag) != 0) {
        return std::unexpected(LoadError{LoadErrorCode::AbiMismatch,
            "module and host native ABI tags differ"});
    }
    if (!descriptor->id || descriptor->id[0] == '\0'
        || (descriptor->kind != PluginKind::Plugin && descriptor->kind != PluginKind::Mod)) {
        return std::unexpected(LoadError{LoadErrorCode::InvalidDescriptor,
            "module ID or kind is invalid"});
    }
    if (descriptor->kind == PluginKind::Mod && phase != LoadPhase::Registration) {
        return std::unexpected(LoadError{LoadErrorCode::RegistrationClosed,
            "mods must load during startup registration"});
    }

    state->instance = create();
    if (!state->instance) {
        return std::unexpected(LoadError{LoadErrorCode::CreationFailed,
            "module factory returned null"});
    }
    state->initialization_attempted = true;
    if (!state->instance->on_load(host)) {
        return std::unexpected(LoadError{LoadErrorCode::InitializationFailed,
            "module rejected initialization"});
    }
    return LoadedPlugin(std::move(state));
}

} // namespace circuit::plugin
