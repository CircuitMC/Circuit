// SPDX-License-Identifier: AGPL-3.0-only
// Test modules for the SDK services. Built twice: as the plugin
// "test.probe" (which depends on the mod) and, with CIRCUIT_PROBE_MOD, as the
// mod "test.probe-mod". Commands:
//   /probe set|use|setcancel <x> <y> <z> [state]   world commands
//   /probe block|entity <x> <y> <z>, /probe items <cx> <cz>, /probe time
//   /probe reports                                 what the tick reports showed
//   /probe-mod late                                a registration after startup
#include "circuit/plugin/api.hpp"

#include <charconv>
#include <format>
#include <new>
#include <string>

namespace {
using namespace circuit::plugin;

std::string status(Status s) {
    return s == Status::denied ? "denied" : s == Status::invalid ? "invalid" : "unavailable";
}
template <class T>
std::string show(const Result<T>& r) {
    if (!r) return "error " + status(r.error());
    if constexpr (std::is_void_v<T>) return "ok";
    else if constexpr (std::is_same_v<T, std::string>) return *r;
    else return std::format("{}", *r);
}
[[maybe_unused]] std::int32_t number(const std::string& s) {
    std::int32_t v{};
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

#ifdef CIRCUIT_PROBE_MOD
class ProbeMod final : public IPlugin, ICommandHandler {
public:
    bool on_load(IHost& host) noexcept override {
        host_ = &host;
        registered_ = show(host.register_command("probe-mod", "late", *this));
        return true;
    }
    void on_tick() noexcept override {}
    void on_unload() noexcept override {}
    std::string run(std::string_view, std::span<const std::string> a) noexcept override {
        if (a.size() == 1 && a[0] == "late") return "startup " + registered_ + ", late " + show(host_->register_command("probe-late", "", *this));
        return "probe-mod";
    }

private:
    IHost* host_{};
    std::string registered_;
};
using Module = ProbeMod;
constexpr const char* module_id = "test.probe-mod";
constexpr PluginKind module_kind = PluginKind::Mod;
constexpr const char* const* module_dependencies = nullptr;
#else
class Probe final : public IPlugin, ICommandHandler {
public:
    bool on_load(IHost& host) noexcept override {
        host_ = &host;
        (void)host.register_command("probe", "...", *this);
        subscribed_ = show(host.subscribe());
        return true;
    }
    void on_tick() noexcept override { ++ticks_; }
    void on_tick_report(const TickReport& r) noexcept override {
        ++reports_;
        blocks_ += r.blocks.size();
        if (!r.blocks.empty()) last_block_ = std::format("{} {} {} {} -> {}", r.blocks.back().pos.x, r.blocks.back().pos.y, r.blocks.back().pos.z,
                                                         r.blocks.back().before, r.blocks.back().after);
        last_tick_ = r.tick;
    }
    void on_unload() noexcept override {}

    std::string run(std::string_view, std::span<const std::string> a) noexcept override {
        try {
            const auto pos = [&](std::size_t i) { return Position{number(a[i]), number(a[i + 1]), number(a[i + 2])}; };
            if (a.size() >= 5 && a[0] == "set") return show(host_->set_block(pos(1), a[4]));
            if (a.size() >= 4 && a[0] == "use") return show(host_->use_block(pos(1)));
            if (a.size() >= 5 && a[0] == "setcancel") {
                const auto admission = host_->set_block(pos(1), a[4]);
                if (!admission) return show(admission);
                const auto first = show(host_->cancel(*admission));
                const auto second = show(host_->cancel(*admission));
                return "cancel " + first + ", again " + second;
            }
            if (a.size() >= 4 && a[0] == "block") return show(host_->block(pos(1)));
            if (a.size() >= 4 && a[0] == "entity") {
                const auto be = host_->block_entity(pos(1));
                if (!be) return "error " + status(be.error());
                int items = 0;
                for (const auto& s : be->slots) items += s.count;
                return std::format("{} slots {} items {} signal {}", be->kind, be->slots.size(), items, be->signal);
            }
            if (a.size() >= 3 && a[0] == "items") {
                const auto items = host_->items(number(a[1]), number(a[2]));
                if (!items) return "error " + status(items.error());
                std::string text = std::format("{} items", items->size());
                for (const auto& i : *items) text += std::format(" {}x{}", i.stack.item, i.stack.count);
                return text;
            }
            if (a.size() == 1 && a[0] == "time") return show(host_->time());
            if (a.size() == 1 && a[0] == "reports")
                return std::format("subscribe {}, ticks {}, reports {}, last tick {}, block changes {}, last {}", subscribed_, ticks_, reports_,
                                   last_tick_, blocks_, last_block_);
            return "unknown";
        } catch (...) {
            return "failed";
        }
    }

private:
    IHost* host_{};
    std::string subscribed_, last_block_;
    std::uint64_t ticks_{}, reports_{}, last_tick_{}, blocks_{};
};
using Module = Probe;
constexpr const char* module_id = "test.probe";
constexpr PluginKind module_kind = PluginKind::Plugin;
constexpr const char* dependency_list[]{"test.probe-mod", nullptr};
constexpr const char* const* module_dependencies = dependency_list;
#endif

constexpr PluginDescriptor descriptor{sizeof(PluginDescriptor), api_version, native_abi_tag, module_id, module_kind,
                                      module_dependencies, "java-1.21.8"};
} // namespace

CIRCUIT_PLUGIN_EXPORT const PluginDescriptor* circuit_plugin_descriptor() noexcept { return &descriptor; }
CIRCUIT_PLUGIN_EXPORT IPlugin* circuit_plugin_create() noexcept { return new (std::nothrow) Module; }
CIRCUIT_PLUGIN_EXPORT void circuit_plugin_destroy(IPlugin* p) noexcept { delete static_cast<Module*>(p); }
