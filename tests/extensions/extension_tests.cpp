// SPDX-License-Identifier: AGPL-3.0-only
// Native modules and the Java bridge on a real server: load order and
// registration, permissions, controlled world access, tick reports, tick
// control, the technical tools, and Java plugins in a real JVM, including
// a JVM that crashes, stalls or floods the server.
#include "../server/test_client.hpp"
#include "../support/check.hpp"
#include <circuit/protocol/content.hpp>
#include <circuit/server/server.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace circuit;
using namespace circuit::test;
using protocol::Bound;
using protocol::State;
using protocol::Writer;
using server::ServerConfig;

namespace {
ServerConfig config_with(std::vector<std::filesystem::path> plugins) {
    ServerConfig c;
    c.bind_address = "127.0.0.1";
    c.port = 0;
    c.online_mode = false;
    c.workers = 2;
    c.network_threads = 2;
    c.view_distance = 3;
    c.simulation_distance = 2;
    c.plugins = std::move(plugins);
    return c;
}
const protocol::ContentTables& content() { return *protocol::ContentTables::for_protocol(772); }

std::string text_of(const TestClient::Packet& p) {
    auto r = p.reader();
    auto tag = protocol::nbt::read_network(r);
    if (!tag) return {};
    if (tag->type() == protocol::nbt::TagType::string) return tag->text();
    if (const auto* t = tag->find("text")) return t->text();
    return {};
}

struct Player {
    explicit Player(std::uint16_t port, std::string_view name = "Tester") : client(port) {
        client.join(name);
        auto pos = client.until(State::play, "player_position");
        require(pos.has_value(), "spawn");
        client.send(State::play, "accept_teleportation", Writer().varint(pos->reader().varint().value()));
        expect("joined the game");
    }
    // Sends a command and returns the first system message containing `want`
    // (or any message when `want` is empty).
    std::string command(std::string_view text, std::string_view want = {}, std::chrono::milliseconds timeout = std::chrono::milliseconds(8000)) {
        client.send(State::play, "chat_command", Writer().string(text));
        return expect(want, timeout);
    }
    std::string expect(std::string_view want, std::chrono::milliseconds timeout = std::chrono::milliseconds(8000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto p = client.until(State::play, "system_chat",
                                  std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
            if (!p) break;
            const auto t = text_of(*p);
            if (want.empty() || t.find(want) != std::string::npos) return t;
        }
        return "<no message>";
    }
    std::optional<std::int32_t> block_update(protocol::WirePosition pos, std::chrono::milliseconds timeout = std::chrono::milliseconds(8000)) {
        const auto want = client.id(State::play, Bound::clientbound, "block_update");
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto p = client.next(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
            if (!p) return std::nullopt;
            if (p->id != want) continue;
            auto r = p->reader();
            if (r.position().value() == pos) return r.varint().value();
        }
        return std::nullopt;
    }
    TestClient client;
};

std::uint64_t tick_of(const std::string& query) {
    // "tick N..." from /tick query or /probe time
    const auto at = query.find_first_of("0123456789");
    return at == std::string::npos ? 0 : std::stoull(query.substr(at));
}
} // namespace

int main() {
    Suite suite("extension_tests");
    const std::filesystem::path tools = CIRCUIT_TECHNICAL_TOOLS, probe = CIRCUIT_PROBE, probe_mod = CIRCUIT_PROBE_MOD;

    suite.run("modules load after their dependencies and the registry freezes", [&] {
        auto c = config_with({probe, probe_mod}); // the plugin is listed first; the mod must load first
        c.plugin_permissions["test.probe"] = "world.read,world.write,world.observe,commands";
        c.plugin_permissions["test.probe-mod"] = "commands";
        server::Server s(c);
        s.start();
        Player p(s.port());
        require(p.command("probe-mod late", "startup") == "startup ok, late error unavailable", "mods register at startup only");
        const auto help = p.command("help", "Commands");
        require(help.find("/probe") != std::string::npos && help.find("/probe-mod") != std::string::npos, "module commands listed: " + help);
        s.stop();
        // A plugin cannot replace a built-in command or another module's command.
        bool refused = false;
        try {
            auto missing = config_with({probe}); // without its mod
            server::Server bad(missing);
        } catch (const std::runtime_error& e) {
            refused = std::string_view(e.what()).find("test.probe-mod") != std::string_view::npos;
        }
        require(refused, "a missing dependency stops the server from starting");
    });

    suite.run("world access through commands, queries and tick reports", [&] {
        auto c = config_with({probe_mod, probe});
        c.plugin_permissions["test.probe"] = "world.read,world.write,world.observe,commands";
        c.plugin_permissions["test.probe-mod"] = "commands";
        server::Server s(c);
        s.start();
        Player p(s.port());
        require(p.command("probe block 0 -61 0", "stone") == "minecraft:stone", "query reads the floor");
        require(p.command("probe block 0 5000 0", "error") == "error invalid", "outside the world");
        const auto admission = p.command("probe set 1 -60 1 minecraft:redstone_lamp[lit=false]");
        require(admission.find("error") == std::string::npos, "set admitted: " + admission);
        require(p.block_update({1, -60, 1}) == content().state_id("minecraft:redstone_lamp[lit=false]"), "the command took effect");
        require(p.command("probe set 1 -60 2 minecraft:dirt", "error") == "error invalid", "unknown states are refused");
        const auto withdrawn = p.command("probe setcancel 2 -60 2 minecraft:stone", "cancel");
        require(withdrawn == "cancel ok, again error invalid", "withdrawn once: " + withdrawn);
        require(p.command("probe block 2 -60 2") == "minecraft:air", "the withdrawn command never ran");
        require(p.command("probe use 0 -61 0").find("error") == std::string::npos, "use admitted");
        const auto reports = p.command("probe reports", "subscribe");
        require(reports.find("subscribe ok") != std::string::npos && reports.find("redstone_lamp") != std::string::npos,
                "reports include the lamp: " + reports);
        require(p.command("probe entity 1 -60 1", "error") == "error invalid", "no block entity on a lamp");
        require(p.command("probe items 0 0", "items") == "0 items", "no item entities");
        s.stop();
    });

    suite.run("permissions are enforced per module", [&] {
        auto c = config_with({probe_mod, probe});
        c.plugin_permissions["test.probe"] = "world.read,commands";
        c.plugin_permissions["test.probe-mod"] = "commands";
        server::Server s(c);
        s.start();
        Player p(s.port());
        require(p.command("probe set 1 -60 1 minecraft:stone", "error") == "error denied", "writing needs world.write");
        require(p.command("probe reports", "subscribe").find("subscribe error denied") != std::string::npos, "reports need world.observe");
        require(p.command("probe block 0 -61 0") == "minecraft:stone", "reading is granted");
        s.stop();
        auto quiet = config_with({tools}); // default permissions: no commands
        server::Server t(quiet);
        t.start();
        Player q(t.port());
        require(q.command("tick query", "Unknown") == "Unknown command: /tick", "commands need the commands permission");
        t.stop();
    });

    suite.run("technical tools: tick control, inspection, traces and counters", [&] {
        auto c = config_with({tools});
        c.plugin_permissions["circuit.technical-tools"] = "world.read,world.observe,server.control,commands";
        server::Server s(c);
        s.start();
        Player p(s.port());
        require(p.command("tick freeze", "frozen") == "Ticking frozen", "freeze");
        const auto first = tick_of(p.command("tick query", "tick"));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto frozen = p.command("tick query", "tick");
        require(tick_of(frozen) == first && frozen.find("frozen") != std::string::npos, "no ticks while frozen: " + frozen);
        require(p.command("tick step 5", "Stepping") == "Stepping 5 ticks", "step");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        const auto stepped = p.command("tick query", "tick");
        require(tick_of(stepped) == first + 5, "five ticks stepped: " + stepped);
        require(p.command("tick unfreeze", "resumed") == "Ticking resumed", "unfreeze");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        require(tick_of(p.command("tick query", "tick")) > first + 5, "ticking again");
        require(p.command("inspect block 0 -61 0", "stone") == "0 -61 0: minecraft:stone", "inspect a block");
        require(p.command("inspect chunk 0 0", "chunk").find("ticking") != std::string::npos, "inspect a chunk");
        // A redstone trace: place a lamp and a lever, then flip the lever.
        require(p.command("trace redstone on", "trace") == "Redstone trace on", "trace on");
        const auto place = [&](std::string_view item, protocol::WirePosition on, std::int32_t seq) {
            p.client.send(State::play, "set_creative_mode_slot",
                          Writer().i16(36).varint(1).varint(content().item_id(item).value()).varint(0).varint(0));
            p.client.send(State::play, "use_item_on",
                          Writer().varint(0).position(on).varint(1).f32(0.5f).f32(1.0f).f32(0.5f).boolean(false).boolean(false).varint(seq));
        };
        place("minecraft:redstone_lamp", {2, -61, 1}, 1);
        require(p.expect("redstone changes").find("1 redstone changes") != std::string::npos, "the lamp placement is traced");
        require(p.command("trace redstone off", "trace") == "Redstone trace off", "trace off");
        require(p.command("counter add 2 -60 1", "container") == "Not a container", "counters need a container");
        require(p.command("counter list", "counted").find("0 containers") != std::string::npos, "no counters");
        s.stop();
    });

    suite.run("java plugins run in a separate JVM", [&] {
        const std::string sources = CIRCUIT_SOURCE_DIR;
        if (std::system("javac -version > /dev/null 2>&1") != 0 || std::system("java -version > /dev/null 2>&1") != 0) {
            std::cout << "  SKIPPED: java and javac are required for the bridge tests\n";
            return;
        }
        std::string templ = (std::filesystem::temp_directory_path() / "circuit-java-XXXXXX").string();
        require(::mkdtemp(templ.data()) != nullptr, "temporary directory");
        const std::filesystem::path classes = templ;
        const auto compile = std::format("javac --release 17 -d '{}' $(find '{}/modules/java-bridge' '{}/tests/extensions/java' -name '*.java') 2>&1",
                                         classes.string(), sources, sources);
        require(std::system(compile.c_str()) == 0, "compile the bridge and test plugins");

        auto c = config_with({});
        c.java_bridge = classes.string();
        c.java_plugin_classes = {"org.circuitmc.example.ExamplePlugin", "org.circuitmc.test.BridgeTestPlugin"};
        c.java_permissions = "world.read,world.write,world.observe,commands";
        c.java_max_lag_ticks = 40;
        {
            server::Server s(c);
            s.start();
            Player p(s.port());
            // Commands arrive once the JVM registered them.
            std::string reply;
            for (int i = 0; i < 50 && (reply = p.command("block 0 -61 0", "", std::chrono::milliseconds(2000))).find("Unknown") != std::string::npos; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            require(reply == "minecraft:stone", "a Java query: " + reply);
            require(p.command("pulse 2 -60 2 10", "Pulse") == "Pulse at 2 -60 2 for 10 ticks", "a Java command");
            require(p.command("block 2 -60 2", "minecraft:") == "minecraft:redstone_block", "the Java plugin placed a block");
            std::string removed;
            for (int i = 0; i < 20 && (removed = p.command("block 2 -60 2", "minecraft:")) != "minecraft:air"; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            require(removed == "minecraft:air", "and removed it after its pulse");
            // A stalled plugin thread: the bridge falls behind and is stopped; the server keeps ticking.
            require(p.command("jstall", "stalling") == "stalling", "stall");
            std::string after;
            for (int i = 0; i < 40 && (after = p.command("pulse 3 -60 3", "", std::chrono::milliseconds(1000))).find("Unknown") == std::string::npos; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            require(after == "Unknown command: /pulse", "the stalled bridge was stopped: " + after);
            const auto before = s.ticks();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            require(s.ticks() > before && s.running(), "the server keeps ticking");
            s.stop();
        }
        {
            server::Server s(c);
            s.start();
            Player p(s.port());
            std::string reply;
            for (int i = 0; i < 50 && (reply = p.command("jcrash", "", std::chrono::milliseconds(1000))).find("Unknown") != std::string::npos; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::string after;
            for (int i = 0; i < 40 && (after = p.command("block 0 -61 0", "", std::chrono::milliseconds(1000))).find("Unknown") == std::string::npos; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            require(after == "Unknown command: /block", "a crashed JVM is removed: " + after);
            require(s.running(), "the server survives the JVM");
            s.stop();
        }
        {
            // A JVM that floods the server with requests is stopped.
            server::Server s(c);
            s.start();
            Player p(s.port());
            std::string reply;
            for (int i = 0; i < 50 && (reply = p.command("jflood", "", std::chrono::milliseconds(1000))).find("Unknown") != std::string::npos; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::string after;
            for (int i = 0; i < 40 && (after = p.command("block 0 -61 0", "", std::chrono::milliseconds(1000))).find("Unknown") == std::string::npos; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            require(after == "Unknown command: /block", "a flooding JVM is removed: " + after);
            require(s.running(), "the server survives the flood");
            s.stop();
        }
        {
            auto limited = c;
            limited.java_permissions = "world.read,commands";
            server::Server s(limited);
            s.start();
            Player p(s.port());
            std::string reply;
            for (int i = 0; i < 50 && (reply = p.command("pulse 2 -60 2", "", std::chrono::milliseconds(2000))).find("Unknown") != std::string::npos; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            require(reply.find("failed") != std::string::npos && reply.find("world.write") != std::string::npos, "writing denied: " + reply);
            s.stop();
        }
        {
            // A JVM that presents the wrong token is refused at startup.
            const auto fake = classes / "fake-java.sh";
            std::ofstream(fake) << "#!/bin/sh\nCIRCUIT_BRIDGE_TOKEN=wrong exec java \"$@\"\n";
            std::filesystem::permissions(fake, std::filesystem::perms::owner_all);
            auto wrong = c;
            wrong.java = fake.string();
            bool refused = false;
            try {
                server::Server s(wrong);
            } catch (const std::runtime_error& e) {
                refused = std::string_view(e.what()).find("wrong token") != std::string_view::npos;
            }
            require(refused, "wrong token refused");
        }
        std::filesystem::remove_all(classes);
    });

    return suite.finish();
}
