set_project("Circuit")
set_version("0.0.1")
set_xmakever("3.0.0")
set_languages("c++23")
add_rules("mode.debug", "mode.release", "mode.asan", "mode.tsan", "mode.ubsan")
-- UndefinedBehaviorSanitizer stops at its first diagnostic, so a completed run
-- has no report.
if is_mode("ubsan") then
    add_cxflags("-fno-sanitize-recover=all")
    add_ldflags("-fno-sanitize-recover=all")
end
set_warnings("allextra")
set_policy("build.fence", true)

option("asio_include")
    set_default("")
    set_showmenu(true)
    set_description("Standalone Asio include directory for offline builds; empty uses pinned xmake package")
option_end()

if get_config("asio_include") ~= "" then
    add_includedirs(get_config("asio_include"), {system = true})
else
    add_requires("asio 1.34.2")
    add_packages("asio")
end
add_defines("ASIO_STANDALONE", "ASIO_NO_DEPRECATED")
add_includedirs("include")
if is_plat("linux") then add_syslinks("pthread") end

target("circuit_contracts")
    set_kind("static")
    add_files("src/contracts/*.cpp")
target_end()

-- Reference rules; executors supply storage and ordering through RuleContext.
target("circuit_gameplay")
    set_kind("static")
    add_files("src/gameplay/*.cpp")
    add_deps("circuit_contracts")
target_end()

target("circuit_runtime")
    set_kind("static")
    add_files("src/runtime/*.cpp")
    add_deps("circuit_gameplay")
target_end()

target("circuit_plugins")
    set_kind("static")
    add_files("src/plugin/*.cpp")
    if is_plat("linux") then add_syslinks("dl", {public = true}) end
target_end()

-- Java Edition wire protocol: data types, NBT, framing and packet ids.
target("circuit_protocol")
    set_kind("static")
    add_files("src/protocol/*.cpp")
    add_syslinks("z", {public = true})
target_end()

-- Identity providers: offline identities now, session verification later.
target("circuit_auth")
    set_kind("static")
    add_files("src/auth/*.cpp")
    add_syslinks("ssl", "crypto", {public = true})
target_end()

-- Durable storage: checkpoints, the write-ahead log and the world directory.
target("circuit_storage")
    set_kind("static")
    add_files("src/storage/*.cpp")
    add_deps("circuit_protocol", "circuit_gameplay")
    add_syslinks("crypto", {public = true})
target_end()

-- circuit-trace fixtures: format, content mapping, evaluation and recording.
target("circuit_evidence")
    set_kind("static")
    add_files("src/evidence/*.cpp")
    add_deps("circuit_runtime")
target_end()

target("circuit-server")
    set_kind("binary")
    add_files("app/*.cpp")
    add_deps("circuit_runtime", "circuit_plugins", "circuit_evidence", "circuit_server")
target_end()

target("technical_tools")
    set_kind("shared")
    add_files("examples/technical_tools/*.cpp")
target_end()

target("plugin_fixture")
    set_kind("shared")
    set_default(false)
    add_defines("CIRCUIT_PLUGIN_TEST_FIXTURE")
    add_files("tests/plugin_tests.cpp")
target_end()

target("plugin_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/plugin_tests.cpp")
    add_deps("circuit_plugins", "technical_tools", "plugin_fixture")
    on_run(function (target)
        os.execv(target:targetfile(), {target:dep("technical_tools"):targetfile(), target:dep("plugin_fixture"):targetfile()})
    end)
target_end()

target("runtime_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/runtime/*.cpp")
    add_deps("circuit_runtime")
target_end()

target("interface_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/interface_tests.cpp")
target_end()

target("validation_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/validation_tests.cpp")
target_end()

target("contract_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/contracts/*.cpp")
    add_deps("circuit_contracts")
target_end()

-- Independent sequential reference executor; never linked into the server.
target("circuit_oracle")
    set_kind("static")
    set_default(false)
    add_files("tests/oracle/*.cpp")
    add_deps("circuit_gameplay")
target_end()

target("rules_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/rules/*.cpp")
    add_deps("circuit_oracle")
target_end()

target("simulation_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/sim/*.cpp")
    add_deps("circuit_oracle", "circuit_runtime")
target_end()

target("evidence_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/evidence/*.cpp")
    add_deps("circuit_evidence", "circuit_oracle")
    on_run(function (target)
        os.execv(target:targetfile(), {path.join(os.projectdir(), "tests", "fixtures")})
    end)
target_end()

-- Network server: sessions, version adapter and the authoritative game loop.
target("circuit_server")
    set_kind("static")
    add_files("src/server/*.cpp")
    add_deps("circuit_protocol", "circuit_auth", "circuit_evidence", "circuit_storage", "circuit_plugins")
target_end()

target("server_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/server/*.cpp")
    add_deps("circuit_server", "circuit-server")
    -- The crash test runs the server binary as a separate process.
    on_config(function (target)
        target:add("defines", format('CIRCUIT_SERVER_BINARY="%s"', path.absolute(target:dep("circuit-server"):targetfile())))
    end)
target_end()

target("anvil_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/anvil/*.cpp")
    add_deps("circuit_server", "circuit_runtime")
target_end()

target("sdk_probe")
    set_kind("shared")
    set_default(false)
    add_files("tests/extensions/sdk_probe.cpp")
target_end()

target("sdk_probe_mod")
    set_kind("shared")
    set_default(false)
    add_defines("CIRCUIT_PROBE_MOD")
    add_files("tests/extensions/sdk_probe.cpp")
target_end()

target("extension_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/extensions/extension_tests.cpp")
    add_deps("circuit_server", "technical_tools", "sdk_probe", "sdk_probe_mod")
    on_config(function (target)
        local function quoted(name, value) target:add("defines", format('%s="%s"', name, value)) end
        quoted("CIRCUIT_TECHNICAL_TOOLS", path.absolute(target:dep("technical_tools"):targetfile()))
        quoted("CIRCUIT_PROBE", path.absolute(target:dep("sdk_probe"):targetfile()))
        quoted("CIRCUIT_PROBE_MOD", path.absolute(target:dep("sdk_probe_mod"):targetfile()))
        quoted("CIRCUIT_SOURCE_DIR", os.projectdir())
    end)
target_end()

target("protocol_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/protocol/*.cpp")
    add_deps("circuit_protocol")
target_end()

target("storage_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/storage/*.cpp")
    add_deps("circuit_storage", "circuit_runtime")
target_end()

target("control_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/control/*.cpp")
    add_deps("circuit_runtime")
target_end()

-- Exploration code is isolated from the server and does not define game rules.
target("circuit_models")
    set_kind("static")
    set_default(false)
    add_files("src/exploration/*.cpp")
target_end()

target("parallel_models")
    set_kind("binary")
    set_default(false)
    add_files("tests/exploration/*.cpp")
    add_deps("circuit_models", "circuit_runtime")
target_end()
