// SPDX-License-Identifier: AGPL-3.0-only
#include "extensions.hpp"
#include "java_bridge.hpp"
#include "log.hpp"
#include <circuit/gameplay/rules.hpp>
#include <algorithm>
#include <circuit/plugin/loader.hpp>
#include <format>
#include <map>
#include <set>

namespace circuit::server {
using plugin::Permission;
using plugin::Permissions;
using plugin::Status;

std::optional<Permissions> parse_permissions(std::string_view list) {
    Permissions p = 0;
    while (!list.empty()) {
        const auto comma = list.find(',');
        auto name = list.substr(0, comma);
        while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
        while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
        if (name == "world.read") p |= static_cast<Permissions>(Permission::world_read);
        else if (name == "world.write") p |= static_cast<Permissions>(Permission::world_write);
        else if (name == "world.observe") p |= static_cast<Permissions>(Permission::world_observe);
        else if (name == "server.control") p |= static_cast<Permissions>(Permission::server_control);
        else if (name == "commands") p |= static_cast<Permissions>(Permission::commands);
        else if (!name.empty()) return std::nullopt;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return p;
}

namespace {
constexpr std::array<std::string_view, 9> builtin_commands{"help", "players", "tp", "gamemode", "forceload", "gamerule", "time", "weather", "tps"};

bool valid_command(std::string_view name) {
    return !name.empty() && name.size() <= 32 &&
           std::ranges::all_of(name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
}

plugin::ItemStack stack_of(const evidence::ContentMapping& mapping, contracts::ItemStack s) {
    if (s.empty()) return {};
    return {mapping.item_name(s.item), s.count};
}
} // namespace

// A registered command: a native handler or the Java bridge.
struct Command {
    std::string owner;
    std::string usage;
    plugin::ICommandHandler* handler{}; // null: the Java bridge
};

struct Extensions::Impl {
    GameServices& game;
    const ServerConfig& config;
    const contracts::VersionBundle& bundle;

    class ModuleHost;
    struct Module {
        std::unique_ptr<ModuleHost> host;
        std::optional<plugin::LoadedPlugin> loaded;
        bool subscribed{};
    };
    std::vector<std::unique_ptr<Module>> modules; // load order
    std::map<std::string, Command> commands;
    bool registration_open{true};
    std::unique_ptr<JavaBridge> bridge;

    // The services one module sees, limited by its permissions.
    class ModuleHost final : public plugin::IHost {
    public:
        ModuleHost(Impl& impl, std::string id, Permissions permissions, bool mod)
            : impl_(impl), id_(std::move(id)), permissions_(permissions), mod_(mod) {}
        Module* module{};
        std::set<std::uint64_t> admissions; // this tick's commands, which it may withdraw

        void log(plugin::LogLevel level, std::string_view message) noexcept override {
            const auto text = std::format("[{}] {}", id_, message.substr(0, 4000));
            switch (level) {
            case plugin::LogLevel::Info: log_info(text); break;
            case plugin::LogLevel::Warning: log_warn(text); break;
            case plugin::LogLevel::Error: log_error(text); break;
            }
        }
        plugin::TickMetrics tick_metrics() const noexcept override { return impl_.game.tick_metrics(); }
        Permissions permissions() const noexcept override { return permissions_; }

        plugin::Result<std::uint64_t> time() const noexcept override {
            if (!allows(Permission::world_read)) return std::unexpected(Status::denied);
            return impl_.game.simulation().time();
        }
        plugin::Result<std::string> block(plugin::Position p) const noexcept override {
            if (!allows(Permission::world_read)) return std::unexpected(Status::denied);
            const auto s = impl_.game.simulation().view().block(contracts::DimensionId::overworld, {p.x, p.y, p.z}); // the server's world
            if (!s) return std::unexpected(s.error() == contracts::ErrorCode::unloaded_chunk ? Status::unavailable : Status::invalid);
            try {
                return impl_.game.content_mapping().notation(*s);
            } catch (...) {
                return std::unexpected(Status::unavailable);
            }
        }
        plugin::Result<plugin::BlockEntityInfo> block_entity(plugin::Position p) const noexcept override {
            if (!allows(Permission::world_read)) return std::unexpected(Status::denied);
            const auto be = impl_.game.simulation().view().block_entity(contracts::DimensionId::overworld, {p.x, p.y, p.z});
            if (!be) return std::unexpected(be.error() == contracts::ErrorCode::unloaded_chunk ? Status::unavailable : Status::invalid);
            if (!*be) return std::unexpected(Status::invalid);
            try {
                const auto& mapping = impl_.game.content_mapping();
                plugin::BlockEntityInfo info;
                info.kind = protocol_name((*be)->kind);
                for (const auto& s : (*be)->slots) info.slots.push_back(stack_of(mapping, s));
                info.signal = (*be)->signal;
                return info;
            } catch (...) {
                return std::unexpected(Status::unavailable);
            }
        }
        plugin::Result<plugin::ChunkInfo> chunk(std::int32_t x, std::int32_t z) const noexcept override {
            if (!allows(Permission::world_read)) return std::unexpected(Status::denied);
            try {
                const auto c = impl_.game.simulation().column({contracts::DimensionId::overworld, {x, z}});
                if (!c) return plugin::ChunkInfo{};
                return plugin::ChunkInfo{true, gameplay::block_ticking(c->levels.simulation), c->block_entities.size(), c->entities.size()};
            } catch (...) {
                return std::unexpected(Status::unavailable);
            }
        }
        plugin::Result<std::vector<plugin::ItemEntity>> items(std::int32_t x, std::int32_t z) const noexcept override {
            if (!allows(Permission::world_read)) return std::unexpected(Status::denied);
            try {
                const auto c = impl_.game.simulation().column({contracts::DimensionId::overworld, {x, z}});
                std::vector<plugin::ItemEntity> out;
                if (!c) return out;
                for (const auto& e : c->entities) {
                    out.push_back({e.id.value, e.position.x, e.position.y, e.position.z, stack_of(impl_.game.content_mapping(), e.stack)});
                }
                return out;
            } catch (...) {
                return std::unexpected(Status::unavailable);
            }
        }

        plugin::Result<std::uint64_t> set_block(plugin::Position p, std::string_view state) noexcept override {
            if (!allows(Permission::world_write)) return std::unexpected(Status::denied);
            try {
                const auto s = impl_.game.content_mapping().block(state);
                if (!s || !gameplay::in_world(impl_.bundle, {p.x, p.y, p.z})) return std::unexpected(Status::invalid);
                return admit(contracts::SetBlock{{p.x, p.y, p.z}, s->raw(), std::nullopt});
            } catch (...) {
                return std::unexpected(Status::unavailable);
            }
        }
        plugin::Result<std::uint64_t> use_block(plugin::Position p) noexcept override {
            if (!allows(Permission::world_write)) return std::unexpected(Status::denied);
            if (!gameplay::in_world(impl_.bundle, {p.x, p.y, p.z})) return std::unexpected(Status::invalid);
            return admit(contracts::UseBlock{{p.x, p.y, p.z}});
        }
        plugin::Result<void> cancel(std::uint64_t admission) noexcept override {
            if (!allows(Permission::world_write)) return std::unexpected(Status::denied);
            if (!admissions.erase(admission) || !impl_.game.withdraw(admission)) return std::unexpected(Status::invalid);
            return {};
        }

        plugin::Result<void> subscribe() noexcept override {
            if (!allows(Permission::world_observe)) return std::unexpected(Status::denied);
            if (module) module->subscribed = true;
            return {};
        }

        plugin::Result<void> register_command(std::string_view name, std::string_view usage, plugin::ICommandHandler& handler) noexcept override {
            if (!allows(Permission::commands)) return std::unexpected(Status::denied);
            // Mods register at startup only; the registry is frozen for them afterwards.
            if (mod_ && !impl_.registration_open) return std::unexpected(Status::unavailable);
            try {
                return impl_.add_command(name, Command{id_, std::string(usage.substr(0, 200)), &handler});
            } catch (...) {
                return std::unexpected(Status::unavailable);
            }
        }
        plugin::Result<void> tell(std::string_view player, std::string_view message) noexcept override {
            if (!allows(Permission::commands)) return std::unexpected(Status::denied);
            try {
                if (!impl_.game.tell(player, message.substr(0, 1000))) return std::unexpected(Status::invalid);
            } catch (...) {
                return std::unexpected(Status::unavailable);
            }
            return {};
        }

        plugin::Result<void> freeze(bool frozen) noexcept override {
            if (!allows(Permission::server_control)) return std::unexpected(Status::denied);
            impl_.game.freeze(frozen);
            return {};
        }
        plugin::Result<void> step(std::uint32_t ticks) noexcept override {
            if (!allows(Permission::server_control)) return std::unexpected(Status::denied);
            if (ticks == 0 || ticks > 72000) return std::unexpected(Status::invalid);
            impl_.game.step(ticks);
            return {};
        }
        plugin::TickStatus tick_status() const noexcept override { return impl_.game.tick_status(); }

        [[nodiscard]] const std::string& id() const noexcept { return id_; }

    private:
        [[nodiscard]] bool allows(Permission p) const noexcept { return plugin::allows(permissions_, p); }
        plugin::Result<std::uint64_t> admit(contracts::CommandBody body) noexcept {
            try {
                const auto a = impl_.game.submit(std::move(body));
                if (!a) return std::unexpected(Status::unavailable);
                admissions.insert(*a);
                return *a;
            } catch (...) {
                return std::unexpected(Status::unavailable);
            }
        }
        static std::string protocol_name(gameplay::BlockType t) {
            switch (t) {
            case gameplay::BlockType::chest: return "minecraft:chest";
            case gameplay::BlockType::hopper: return "minecraft:hopper";
            case gameplay::BlockType::comparator: return "minecraft:comparator";
            case gameplay::BlockType::moving_piston: return "minecraft:piston";
            default: return "unknown";
            }
        }

        Impl& impl_;
        std::string id_;
        Permissions permissions_;
        bool mod_;
    };

    Impl(GameServices& g, const ServerConfig& c, const contracts::VersionBundle& b) : game(g), config(c), bundle(b) {}

    plugin::Result<void> add_command(std::string_view name, Command command) {
        if (!valid_command(name) || std::ranges::find(builtin_commands, name) != builtin_commands.end())
            return std::unexpected(Status::invalid);
        if (!commands.emplace(std::string(name), std::move(command)).second) return std::unexpected(Status::invalid);
        return {};
    }

    Permissions permissions_for(const std::string& id) const {
        const auto it = config.plugin_permissions.find(id);
        if (it == config.plugin_permissions.end())
            return Permission::world_read | Permission::world_observe;
        const auto p = parse_permissions(it->second);
        if (!p) throw std::runtime_error(std::format("plugin {}: unknown permission in `{}`", id, it->second));
        return *p;
    }

    // Starts modules in dependency order. `loaded` holds the ids already started.
    void start(std::vector<plugin::OpenedModule> opened, plugin::LoadPhase phase, std::vector<std::string>& loaded) {
        std::vector<plugin::ModuleInfo> infos;
        for (const auto& o : opened) infos.push_back(o.info());
        const auto order = plugin::load_order(infos, bundle.id, loaded);
        if (!order) throw std::runtime_error("plugins: " + order.error().message);
        for (const auto i : *order) {
            const auto& info = infos[i];
            auto module = std::make_unique<Module>();
            module->host = std::make_unique<ModuleHost>(*this, info.id, permissions_for(info.id), info.kind == plugin::PluginKind::Mod);
            module->host->module = module.get();
            auto started = plugin::PluginLoader::start(std::move(opened[i]), *module->host, phase);
            if (!started) {
                drop_commands(info.id);
                throw std::runtime_error(std::format("plugin {}: {}", info.id, started.error().message));
            }
            module->loaded.emplace(std::move(*started));
            log_info(std::format("loaded {} {}", info.kind == plugin::PluginKind::Mod ? "mod" : "plugin", info.id));
            loaded.push_back(info.id);
            modules.push_back(std::move(module));
        }
    }

    void load() {
        std::vector<plugin::OpenedModule> mods, plugins;
        for (const auto& path : config.plugins) {
            auto opened = plugin::PluginLoader::open(path);
            if (!opened) throw std::runtime_error(std::format("plugin {}: {}", path.string(), opened.error().message));
            (opened->info().kind == plugin::PluginKind::Mod ? mods : plugins).push_back(std::move(*opened));
        }
        std::vector<std::string> loaded;
        // Mods may depend only on mods; they all start before registration closes.
        start(std::move(mods), plugin::LoadPhase::Registration, loaded);
        registration_open = false;
        start(std::move(plugins), plugin::LoadPhase::Running, loaded);
        if (!config.java_bridge.empty()) {
            const auto permissions = parse_permissions(config.java_permissions);
            if (!permissions) throw std::runtime_error(std::format("java-permissions: unknown permission in `{}`", config.java_permissions));
            bridge = std::make_unique<JavaBridge>(game, config, bundle, *permissions,
                                                  [this](std::string_view name, std::string_view usage) {
                                                      return add_command(name, Command{"java", std::string(usage), nullptr});
                                                  });
            bridge->start();
        }
    }

    void drop_commands(std::string_view owner) {
        std::erase_if(commands, [&](const auto& c) { return c.second.owner == owner; });
    }

    // The changes of the tick that just ran, for subscribed modules.
    plugin::TickReport report() const {
        plugin::TickReport r;
        const auto& engine = game.simulation();
        const auto& mapping = game.content_mapping();
        r.tick = engine.time();
        const auto notation = [&](std::uint64_t raw) {
            const auto s = gameplay::BlockState::decode(static_cast<std::uint32_t>(raw));
            return s ? mapping.notation(*s) : std::string("unknown");
        };
        const auto stack = [&](std::uint64_t packed) {
            return stack_of(mapping, {static_cast<std::uint16_t>(packed >> 8), static_cast<std::uint8_t>(packed & 0xFF)});
        };
        for (const auto& t : engine.trace()) {
            if (t.dimension != contracts::DimensionId::overworld) continue; // extensions see the server's world
            const plugin::Position p{t.pos.x, t.pos.y, t.pos.z};
            if (t.kind == contracts::TraceKind::block_changed) r.blocks.push_back({p, notation(t.a), notation(t.b)});
            else if (t.kind == contracts::TraceKind::inventory_changed)
                r.slots.push_back({p, static_cast<std::int32_t>(t.a), stack(t.b), stack(t.c)});
        }
        return r;
    }

    void after_tick() {
        // Commands of the last tick are applied: they can no longer be withdrawn.
        for (auto& m : modules) m->host->admissions.clear();
        const bool wanted = std::ranges::any_of(modules, [](const auto& m) { return m->subscribed; }) || (bridge && bridge->subscribed());
        if (wanted) {
            const auto r = report();
            for (auto& m : modules)
                if (m->subscribed) m->loaded->tick_report(r);
            if (bridge) bridge->tick(r);
        } else if (bridge) {
            bridge->tick({game.simulation().time(), {}, {}});
        }
        for (auto& m : modules) m->loaded->tick();
    }

    void unload() {
        if (bridge) {
            bridge->stop();
            bridge.reset();
            drop_commands("java");
        }
        while (!modules.empty()) {
            auto& m = modules.back();
            const auto id = m->host->id();
            drop_commands(id); // no callbacks into the library after it closes
            m->loaded.reset();
            modules.pop_back();
            log_info(std::format("unloaded {}", id));
        }
    }
};

Extensions::Extensions(GameServices& game, const ServerConfig& config, const contracts::VersionBundle& bundle)
    : impl_(std::make_unique<Impl>(game, config, bundle)) {}
Extensions::~Extensions() { impl_->unload(); }

void Extensions::load() {
    try {
        impl_->load();
    } catch (...) {
        impl_->unload();
        throw;
    }
}

void Extensions::before_tick() {
    if (impl_->bridge) {
        impl_->bridge->serve();
        if (impl_->bridge->failed()) {
            log_error(std::format("java bridge stopped: {}", impl_->bridge->failure()));
            impl_->bridge->stop();
            impl_->bridge.reset();
            impl_->drop_commands("java");
        }
    }
}

void Extensions::after_tick() { impl_->after_tick(); }

std::optional<std::string> Extensions::command(std::string_view player, std::string_view name, std::span<const std::string> arguments) {
    const auto it = impl_->commands.find(std::string(name));
    if (it == impl_->commands.end()) return std::nullopt;
    if (!it->second.handler) {
        if (!impl_->bridge) return "The Java bridge is not running";
        impl_->bridge->command(player, name, arguments);
        return std::string();
    }
    return it->second.handler->run(player, arguments);
}

std::vector<std::pair<std::string, std::string>> Extensions::commands() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [name, c] : impl_->commands) out.emplace_back(name, c.usage);
    return out;
}

void Extensions::unload() { impl_->unload(); }

} // namespace circuit::server
