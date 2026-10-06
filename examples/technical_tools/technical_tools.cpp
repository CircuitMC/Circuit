// SPDX-License-Identifier: AGPL-3.0-only
// Technical tools as a native plugin, built only on the SDK's controlled
// services:
//   /tick freeze | unfreeze | step [n] | query   tick control and timing
//   /inspect block <x> <y> <z> | chunk <cx> <cz>  state of the published world
//   /trace redstone on | off                     redstone changes, per tick
//   /counter add|remove <x> <y> <z> | list | reset   items entering containers
// It needs the permissions commands, world.read, world.observe and
// server.control; without one, the matching commands are not offered.
#include "circuit/plugin/api.hpp"

#include <charconv>
#include <cstdio>
#include <format>
#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace circuit::plugin;

std::optional<std::int32_t> integer(const std::string& s) {
    std::int32_t v{};
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (error != std::errc{} || end != s.data() + s.size()) return std::nullopt;
    return v;
}
std::optional<Position> position(std::span<const std::string> a) {
    if (a.size() != 3) return std::nullopt;
    const auto x = integer(a[0]), y = integer(a[1]), z = integer(a[2]);
    if (!x || !y || !z) return std::nullopt;
    return Position{*x, *y, *z};
}
std::string describe(Status s) {
    switch (s) {
    case Status::denied: return "not permitted";
    case Status::invalid: return "invalid";
    case Status::unavailable: return "unavailable";
    }
    return "failed";
}
bool redstone(const std::string& notation) {
    for (const auto* name : {"redstone_wire", "repeater", "comparator", "redstone_lamp", "observer", "lever", "piston"})
        if (notation.find(name) != std::string::npos) return true;
    return false;
}

class TechnicalTools;

// One chat command, forwarding to a member function.
struct Command final : ICommandHandler {
    TechnicalTools* tools{};
    std::string (TechnicalTools::*run_fn)(std::string_view, std::span<const std::string>){};
    std::string run(std::string_view player, std::span<const std::string> arguments) noexcept override;
};

class TechnicalTools final : public IPlugin {
public:
    bool on_load(IHost& host) noexcept override {
        host_ = &host;
        const auto offer = [&](Command& c, std::string_view name, std::string_view usage,
                                std::string (TechnicalTools::*fn)(std::string_view, std::span<const std::string>)) {
            c.tools = this;
            c.run_fn = fn;
            if (const auto ok = host_->register_command(name, usage, c); !ok)
                host_->log(LogLevel::Warning, std::format("technical-tools: /{} not offered: {}", name, describe(ok.error())));
        };
        offer(tick_, "tick", "freeze|unfreeze|step [n]|query", &TechnicalTools::tick_command);
        offer(inspect_, "inspect", "block <x> <y> <z>|chunk <cx> <cz>", &TechnicalTools::inspect_command);
        offer(trace_, "trace", "redstone on|off", &TechnicalTools::trace_command);
        offer(counter_, "counter", "add|remove <x> <y> <z>|list|reset", &TechnicalTools::counter_command);
        if (const auto ok = host_->subscribe(); !ok)
            host_->log(LogLevel::Warning, "technical-tools: no tick reports: " + describe(ok.error()));
        host_->log(LogLevel::Info, "technical-tools: loaded");
        return true;
    }

    void on_tick_report(const TickReport& report) noexcept override {
        try {
            for (const auto& s : report.slots) {
                if (!counted_.contains(s.pos)) continue;
                const bool same = s.before.item == s.after.item;
                const auto gained = same ? s.after.count - s.before.count : s.after.count;
                if (gained > 0) counts_[s.after.item] += gained;
            }
            if (!tracers_.empty()) {
                std::vector<std::string> lines;
                for (const auto& b : report.blocks)
                    if (redstone(b.after) || redstone(b.before))
                        lines.push_back(std::format("{} {} {} {} -> {}", b.pos.x, b.pos.y, b.pos.z, b.before, b.after));
                if (!lines.empty()) {
                    for (const auto& player : tracers_) {
                        (void)host_->tell(player, std::format("tick {}: {} redstone changes", report.tick, lines.size()));
                        for (std::size_t i = 0; i < lines.size() && i < 8; ++i) (void)host_->tell(player, lines[i]);
                    }
                }
            }
        } catch (...) {
            // Out of memory: skip this report.
        }
    }

    void on_tick() noexcept override {
        const auto metrics = host_->tick_metrics();
        // One sample per minute of completed ticks.
        if (metrics.completed_ticks == 0 || metrics.completed_ticks % 1200 != 0 || metrics.completed_ticks == last_reported_tick_) return;
        last_reported_tick_ = metrics.completed_ticks;
        char message[192];
        std::snprintf(message, sizeof(message), "technical-tools: tick=%llu last=%.3fms mean=%.3fms",
                      static_cast<unsigned long long>(metrics.completed_ticks), static_cast<double>(metrics.last_tick_ns) / 1'000'000.0,
                      static_cast<double>(metrics.mean_tick_ns) / 1'000'000.0);
        host_->log(LogLevel::Info, message);
    }

    void on_unload() noexcept override {
        if (host_) {
            host_->log(LogLevel::Info, "technical-tools: unloaded");
            host_ = nullptr;
        }
    }

    std::string tick_command(std::string_view, std::span<const std::string> a) {
        if (a.size() == 1 && a[0] == "freeze") return result(host_->freeze(true), "Ticking frozen");
        if (a.size() == 1 && a[0] == "unfreeze") return result(host_->freeze(false), "Ticking resumed");
        if (!a.empty() && a[0] == "step") {
            const auto n = a.size() == 2 ? integer(a[1]) : std::optional<std::int32_t>(1);
            if (!n || *n < 1) return "Usage: /tick step [n]";
            return result(host_->step(static_cast<std::uint32_t>(*n)), std::format("Stepping {} ticks", *n));
        }
        if (a.size() == 1 && a[0] == "query") {
            const auto s = host_->tick_status();
            return std::format("tick {}{}; recent ticks: mean {:.2f} ms, p50 {:.2f}, p95 {:.2f}, p99 {:.2f}, max {:.2f}", s.tick,
                               s.frozen ? std::format(", frozen ({} steps pending)", s.pending_steps) : "", s.mean_ms, s.p50_ms, s.p95_ms,
                               s.p99_ms, s.max_ms);
        }
        return "Usage: /tick freeze|unfreeze|step [n]|query";
    }

    std::string inspect_command(std::string_view, std::span<const std::string> a) {
        if (!a.empty() && a[0] == "block") {
            const auto p = position(a.subspan(1));
            if (!p) return "Usage: /inspect block <x> <y> <z>";
            const auto b = host_->block(*p);
            if (!b) return "Block: " + describe(b.error());
            std::string text = std::format("{} {} {}: {}", p->x, p->y, p->z, *b);
            if (const auto be = host_->block_entity(*p)) {
                text += std::format(" [{}", be->kind);
                if (be->kind == "minecraft:comparator") text += std::format(", signal {}", be->signal);
                int stacks = 0, items = 0;
                for (const auto& s : be->slots)
                    if (s.count > 0) ++stacks, items += s.count;
                if (!be->slots.empty()) text += std::format(", {} items in {} of {} slots", items, stacks, be->slots.size());
                text += "]";
            }
            return text;
        }
        if (a.size() == 3 && a[0] == "chunk") {
            const auto x = integer(a[1]), z = integer(a[2]);
            if (!x || !z) return "Usage: /inspect chunk <cx> <cz>";
            const auto c = host_->chunk(*x, *z);
            if (!c) return "Chunk: " + describe(c.error());
            if (!c->loaded) return std::format("chunk {} {}: not loaded", *x, *z);
            return std::format("chunk {} {}: {}, {} block entities, {} entities", *x, *z, c->ticking ? "ticking" : "border",
                               c->block_entities, c->entities);
        }
        return "Usage: /inspect block <x> <y> <z>|chunk <cx> <cz>";
    }

    std::string trace_command(std::string_view player, std::span<const std::string> a) {
        if (a.size() == 2 && a[0] == "redstone" && (a[1] == "on" || a[1] == "off")) {
            if (a[1] == "on") tracers_.insert(std::string(player));
            else tracers_.erase(std::string(player));
            return std::format("Redstone trace {}", a[1]);
        }
        return "Usage: /trace redstone on|off";
    }

    std::string counter_command(std::string_view, std::span<const std::string> a) {
        if (a.size() == 4 && (a[0] == "add" || a[0] == "remove")) {
            const auto p = position(a.subspan(1));
            if (!p) return "Usage: /counter add|remove <x> <y> <z>";
            if (a[0] == "remove") return counted_.erase(*p) ? "Counter removed" : "No counter there";
            const auto be = host_->block_entity(*p);
            if (!be || be->slots.empty()) return "Not a container";
            counted_.insert(*p);
            return std::format("Counting items entering {} {} {}", p->x, p->y, p->z);
        }
        if (a.size() == 1 && a[0] == "reset") {
            counts_.clear();
            since_ = host_->time().value_or(0);
            return "Counters reset";
        }
        if (a.size() == 1 && a[0] == "list") {
            const auto now = host_->time().value_or(since_);
            const auto ticks = now > since_ ? now - since_ : 0;
            if (counts_.empty()) return std::format("No items counted in {} ticks ({} containers)", ticks, counted_.size());
            std::string text = std::format("In {} ticks:", ticks);
            for (const auto& [item, n] : counts_)
                text += std::format(" {} x{} ({:.0f}/h)", item, n, ticks ? static_cast<double>(n) * 72000.0 / static_cast<double>(ticks) : 0.0);
            return text;
        }
        return "Usage: /counter add|remove <x> <y> <z>|list|reset";
    }

private:
    static std::string result(const Result<void>& r, std::string_view ok) { return r ? std::string(ok) : "Refused: " + describe(r.error()); }

    IHost* host_{};
    Command tick_, inspect_, trace_, counter_;
    std::set<std::string> tracers_;
    std::set<Position> counted_;
    std::map<std::string, std::int64_t> counts_;
    std::uint64_t since_{};
    std::uint64_t last_reported_tick_{};
};

std::string Command::run(std::string_view player, std::span<const std::string> arguments) noexcept {
    try {
        return (tools->*run_fn)(player, arguments);
    } catch (...) {
        return "The command failed";
    }
}

constexpr circuit::plugin::PluginDescriptor descriptor{
    sizeof(circuit::plugin::PluginDescriptor),
    circuit::plugin::api_version,
    circuit::plugin::native_abi_tag,
    "circuit.technical-tools",
    circuit::plugin::PluginKind::Plugin,
    nullptr,
    nullptr,
};

} // namespace

CIRCUIT_PLUGIN_EXPORT const circuit::plugin::PluginDescriptor* circuit_plugin_descriptor() noexcept { return &descriptor; }

CIRCUIT_PLUGIN_EXPORT circuit::plugin::IPlugin* circuit_plugin_create() noexcept { return new (std::nothrow) TechnicalTools; }

CIRCUIT_PLUGIN_EXPORT void circuit_plugin_destroy(circuit::plugin::IPlugin* plugin) noexcept { delete static_cast<TechnicalTools*>(plugin); }
