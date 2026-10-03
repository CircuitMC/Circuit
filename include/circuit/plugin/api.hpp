// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>
#include <string_view>

// Native C++ interfaces require the same toolchain, standard library and ABI
// configuration. This tag catches common mismatches; it is not an ABI promise.
#define CIRCUIT_DETAIL_STRINGIFY_IMPL(value) #value
#define CIRCUIT_DETAIL_STRINGIFY(value) CIRCUIT_DETAIL_STRINGIFY_IMPL(value)
#if defined(__clang__)
#define CIRCUIT_DETAIL_COMPILER "clang-" __clang_version__
#elif defined(__GNUC__)
#define CIRCUIT_DETAIL_COMPILER "gcc-" __VERSION__
#else
#error "The initial Circuit native plugin ABI supports GCC and Clang on Linux."
#endif
#if defined(_LIBCPP_VERSION)
#define CIRCUIT_DETAIL_STDLIB "libc++-" CIRCUIT_DETAIL_STRINGIFY(_LIBCPP_VERSION)
#elif defined(__GLIBCXX__)
#define CIRCUIT_DETAIL_STDLIB "libstdc++-" CIRCUIT_DETAIL_STRINGIFY(__GLIBCXX__) \
    "-cxx11abi-" CIRCUIT_DETAIL_STRINGIFY(_GLIBCXX_USE_CXX11_ABI)
#else
#error "Unsupported C++ standard library for the initial native plugin ABI."
#endif
#if defined(_GLIBCXX_DEBUG)
#define CIRCUIT_DETAIL_DEBUG_ABI "-debug"
#else
#define CIRCUIT_DETAIL_DEBUG_ABI "-normal"
#endif

#define CIRCUIT_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))

namespace circuit::plugin {

inline constexpr std::uint32_t api_version = 1;
inline constexpr char native_abi_tag[] =
    "circuit-v1;" CIRCUIT_DETAIL_COMPILER ";" CIRCUIT_DETAIL_STDLIB
    CIRCUIT_DETAIL_DEBUG_ABI ";cxx-" CIRCUIT_DETAIL_STRINGIFY(__cplusplus)
    ";ptr-" CIRCUIT_DETAIL_STRINGIFY(__SIZEOF_POINTER__);

enum class PluginKind : std::uint32_t { Plugin = 1, Mod = 2 };
enum class LogLevel : std::uint32_t { Info, Warning, Error };

struct TickMetrics {
    std::uint64_t completed_ticks{};
    std::uint64_t last_tick_ns{};
    std::uint64_t mean_tick_ns{};
};

// A plugin receives values and explicit capabilities, never mutable world
// pointers. Callbacks run on the control thread. The host outlives its plugins.
class IHost {
public:
    virtual void log(LogLevel level, std::string_view message) noexcept = 0;
    [[nodiscard]] virtual TickMetrics tick_metrics() const noexcept = 0;

protected:
    virtual ~IHost() = default;
};

class IPlugin {
public:
    // Returning false rejects loading. on_unload is still called for cleanup.
    virtual bool on_load(IHost& host) noexcept = 0;
    virtual void on_tick() noexcept = 0;
    // Stop and join any owned workers before returning from on_unload.
    virtual void on_unload() noexcept = 0;

protected:
    virtual ~IPlugin() = default;
};

// C linkage locates these entry points, while the implementation deliberately
// uses a toolchain-specific C++ inheritance ABI behind them.
struct PluginDescriptor {
    std::uint32_t structure_size;
    std::uint32_t version;
    const char* abi_tag;
    const char* id;
    PluginKind kind;
};

using DescriptorFunction = const PluginDescriptor* (*)() noexcept;
using CreateFunction = IPlugin* (*)() noexcept;
using DestroyFunction = void (*)(IPlugin*) noexcept;

inline constexpr char descriptor_symbol[] = "circuit_plugin_descriptor";
inline constexpr char create_symbol[] = "circuit_plugin_create";
inline constexpr char destroy_symbol[] = "circuit_plugin_destroy";

} // namespace circuit::plugin

#undef CIRCUIT_DETAIL_COMPILER
#undef CIRCUIT_DETAIL_STDLIB
#undef CIRCUIT_DETAIL_DEBUG_ABI
#undef CIRCUIT_DETAIL_STRINGIFY
#undef CIRCUIT_DETAIL_STRINGIFY_IMPL
