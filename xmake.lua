set_project("Circuit")
set_version("0.0.1")
set_xmakever("3.0.0")
set_languages("c++23")
add_rules("mode.debug", "mode.release", "mode.asan", "mode.tsan")
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

target("circuit_runtime")
    set_kind("static")
    add_files("src/runtime/*.cpp")
target_end()

target("circuit_plugins")
    set_kind("static")
    add_files("src/plugin/*.cpp")
    if is_plat("linux") then add_syslinks("dl", {public = true}) end
target_end()

target("circuit-server")
    set_kind("binary")
    add_files("app/main.cpp")
    add_deps("circuit_runtime", "circuit_plugins")
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
