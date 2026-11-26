set_project("Circuit")
set_version("0.1.0")
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

-- The engine facade (include/circuit/runtime/engine.hpp) and, as it grows,
-- the strict single-threaded runtime behind it.
target("circuit_runtime")
    set_kind("static")
    add_files("src/runtime/*.cpp")
    add_deps("circuit_gameplay")
target_end()

-- Java Edition wire protocol: data types, NBT, framing and packet ids.
target("circuit_protocol")
    set_kind("static")
    add_files("src/protocol/*.cpp")
    add_deps("circuit_contracts")
    add_syslinks("z", {public = true})
target_end()

-- Login cryptography, offline identities and the JSON reader. Session
-- verification returns with the network work on PACE.
target("circuit_auth")
    set_kind("static")
    add_files("src/auth/*.cpp")
    add_syslinks("ssl", "crypto", {public = true})
target_end()

-- Data packs: the official jar's built-in pack, pack directories and archives.
target("circuit_data")
    set_kind("static")
    add_files("src/data/*.cpp")
    add_deps("circuit_auth", "circuit_contracts")
    add_syslinks("z", "crypto", {public = true})
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

-- A test program is every source file of its directory: a new file adds
-- cases with CIRCUIT_TEST (tests/support/check.hpp) and no other file changes.
-- CIRCUIT_SOURCE_DIR locates tests/fixtures from any of them.
target("contract_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/contracts/*.cpp")
    add_deps("circuit_contracts")
    on_config(function (target)
        target:add("defines", format('CIRCUIT_SOURCE_DIR="%s"', os.projectdir()))
    end)
target_end()

-- Independent sequential reference executor in the vanilla order; the
-- specification of the strict single-threaded runtime. Never linked into a server.
target("circuit_oracle")
    set_kind("static")
    set_default(false)
    add_files("tests/oracle/*.cpp")
    add_deps("circuit_runtime")
target_end()

target("rules_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/rules/*.cpp")
    add_deps("circuit_oracle")
    on_config(function (target)
        target:add("defines", format('CIRCUIT_SOURCE_DIR="%s"', os.projectdir()))
    end)
target_end()

target("evidence_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/evidence/*.cpp")
    add_deps("circuit_evidence", "circuit_oracle")
    on_config(function (target)
        target:add("defines", format('CIRCUIT_SOURCE_DIR="%s"', os.projectdir()))
    end)
    on_run(function (target)
        os.execv(target:targetfile(), {path.join(os.projectdir(), "tests", "fixtures")})
    end)
target_end()

-- What the restructuring keeps of the old server: the data compilers that turn
-- the official jar and data packs into GameData, the synchronized registries,
-- the packet layouts (wire), the block palette and section encoding, and Anvil
-- import and export. The lanes split it by owner.
target("circuit_server")
    set_kind("static")
    add_files("src/server/*.cpp")
    add_deps("circuit_protocol", "circuit_auth", "circuit_evidence", "circuit_storage", "circuit_data")
target_end()

target("anvil_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/anvil/*.cpp")
    add_deps("circuit_server", "circuit_oracle")
    on_load(function (target)
        target:add("defines", format('CIRCUIT_SOURCE_DIR="%s"', os.projectdir()))
    end)
target_end()

target("protocol_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/protocol/*.cpp")
    add_deps("circuit_protocol")
    on_config(function (target)
        target:add("defines", format('CIRCUIT_SOURCE_DIR="%s"', os.projectdir()))
    end)
target_end()

target("data_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/data/*.cpp")
    add_deps("circuit_data")
    on_config(function (target)
        target:add("defines", format('CIRCUIT_SOURCE_DIR="%s"', os.projectdir()))
    end)
target_end()

target("storage_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/storage/*.cpp")
    add_deps("circuit_storage", "circuit_oracle")
    on_config(function (target)
        target:add("defines", format('CIRCUIT_SOURCE_DIR="%s"', os.projectdir()))
    end)
target_end()

target("content_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/content/*.cpp")
    add_deps("circuit_server", "circuit_oracle")
    on_config(function (target)
        target:add("defines", format('CIRCUIT_SOURCE_DIR="%s"', os.projectdir()))
    end)
target_end()

-- Records, checks and compares circuit-trace fixtures on the oracle
-- (tools/scenario.py, re-recording tests/fixtures/reference).
target("circuit-fixture")
    set_kind("binary")
    add_files("tools/fixture/*.cpp")
    add_deps("circuit_server", "circuit_oracle")
target_end()
