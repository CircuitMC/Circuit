// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

inline constexpr std::uint32_t api_version = 2;
inline constexpr char native_abi_tag[] =
    "circuit-v2;" CIRCUIT_DETAIL_COMPILER ";" CIRCUIT_DETAIL_STDLIB
    CIRCUIT_DETAIL_DEBUG_ABI ";cxx-" CIRCUIT_DETAIL_STRINGIFY(__cplusplus)
    ";ptr-" CIRCUIT_DETAIL_STRINGIFY(__SIZEOF_POINTER__);

enum class PluginKind : std::uint32_t { Plugin = 1, Mod = 2 };
enum class LogLevel : std::uint32_t { Info, Warning, Error };

// What a module may do beyond logging. The operator grants permissions per
// module; everything else is refused with Status::denied.
enum class Permission : std::uint32_t {
    world_read = 1,     // queries of the published world
    world_write = 2,    // commands, applied at the start of a later tick
    world_observe = 4,  // tick reports with the committed changes
    server_control = 8, // freeze, unfreeze and step
    commands = 16,      // chat commands and messages to players
};
using Permissions = std::uint32_t;
constexpr Permissions operator|(Permission a, Permission b) noexcept {
    return static_cast<Permissions>(a) | static_cast<Permissions>(b);
}
constexpr Permissions operator|(Permissions a, Permission b) noexcept { return a | static_cast<Permissions>(b); }
constexpr bool allows(Permissions granted, Permission p) noexcept { return (granted & static_cast<Permissions>(p)) != 0; }

enum class Status : std::uint32_t {
    denied = 1,      // the module lacks the permission
    invalid = 2,     // malformed arguments, unknown block state, out of range
    unavailable = 3, // not in this host or not now (for example, an unloaded chunk)
};
template <class T>
using Result = std::expected<T, Status>;

struct TickMetrics {
    std::uint64_t completed_ticks{};
    std::uint64_t last_tick_ns{};
    std::uint64_t mean_tick_ns{};
};

struct Position {
    std::int32_t x{}, y{}, z{};
    auto operator<=>(const Position&) const = default;
};

struct ItemStack {
    std::string item; // e.g. "minecraft:redstone"; empty for an empty slot
    std::int32_t count{};
    bool operator==(const ItemStack&) const = default;
};

struct ItemEntity {
    std::uint64_t id{};
    double x{}, y{}, z{};
    ItemStack stack;
};

struct ChunkInfo {
    bool loaded{};
    bool ticking{}; // its scheduled ticks run (simulation level 32 or lower)
    std::size_t block_entities{}, entities{};
};

struct BlockEntityInfo {
    std::string kind;              // e.g. "minecraft:hopper"
    std::vector<ItemStack> slots;  // containers
    std::int32_t signal{};         // comparator output
};

// The changes of one committed tick, in order.
struct BlockChange {
    Position pos;
    std::string before, after; // block-state notation
};
struct SlotChange {
    Position pos; // the container
    std::int32_t slot{};
    ItemStack before, after;
};
struct TickReport {
    std::uint64_t tick{};
    std::vector<BlockChange> blocks;
    std::vector<SlotChange> slots;
};

struct TickStatus {
    std::uint64_t tick{};
    bool frozen{};
    std::uint64_t pending_steps{};
    double mean_ms{}, p50_ms{}, p95_ms{}, p99_ms{}, max_ms{}; // recent ticks
};

// A chat command registered by a module. Runs on the control thread.
class ICommandHandler {
public:
    // Returns the reply sent to the player.
    virtual std::string run(std::string_view player, std::span<const std::string> arguments) noexcept = 0;

protected:
    virtual ~ICommandHandler() = default;
};

// The host's services. A module receives values and explicit capabilities,
// never mutable world pointers. Every call is made on the control thread,
// from a callback, and returns before the next tick starts. Queries read the
// world as published after the last tick; commands are admitted like player
// input and take effect at the start of a later tick. The host outlives its
// modules.
class IHost {
public:
    virtual void log(LogLevel level, std::string_view message) noexcept = 0;
    [[nodiscard]] virtual TickMetrics tick_metrics() const noexcept = 0;
    [[nodiscard]] virtual Permissions permissions() const noexcept = 0;

    // world_read
    [[nodiscard]] virtual Result<std::uint64_t> time() const noexcept = 0;
    [[nodiscard]] virtual Result<std::string> block(Position pos) const noexcept = 0;
    [[nodiscard]] virtual Result<BlockEntityInfo> block_entity(Position pos) const noexcept = 0;
    [[nodiscard]] virtual Result<ChunkInfo> chunk(std::int32_t chunk_x, std::int32_t chunk_z) const noexcept = 0;
    [[nodiscard]] virtual Result<std::vector<ItemEntity>> items(std::int32_t chunk_x, std::int32_t chunk_z) const noexcept = 0;

    // world_write: returns the command's admission number.
    virtual Result<std::uint64_t> set_block(Position pos, std::string_view state) noexcept = 0;
    virtual Result<std::uint64_t> use_block(Position pos) noexcept = 0;
    // Withdraws a command of this module that has not been applied yet.
    virtual Result<void> cancel(std::uint64_t admission) noexcept = 0;

    // world_observe: on_tick_report receives each committed tick from now on.
    virtual Result<void> subscribe() noexcept = 0;

    // commands
    virtual Result<void> register_command(std::string_view name, std::string_view usage, ICommandHandler& handler) noexcept = 0;
    virtual Result<void> tell(std::string_view player, std::string_view message) noexcept = 0;

    // server_control. While frozen, ticks do not run; inputs wait for the
    // next tick that runs. step(n) runs n ticks while frozen.
    virtual Result<void> freeze(bool frozen) noexcept = 0;
    virtual Result<void> step(std::uint32_t ticks) noexcept = 0;
    [[nodiscard]] virtual TickStatus tick_status() const noexcept = 0;

protected:
    virtual ~IHost() = default;
};

class IPlugin {
public:
    // Returning false rejects loading. on_unload is still called for cleanup.
    virtual bool on_load(IHost& host) noexcept = 0;
    // After every committed tick.
    virtual void on_tick() noexcept = 0;
    // After every committed tick, before on_tick, once subscribed.
    virtual void on_tick_report(const TickReport& report) noexcept { (void)report; }
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
    const char* id;             // [a-z0-9._-]+, unique per server
    PluginKind kind;
    const char* const* dependencies; // ids loaded first; null-terminated, or null for none
    const char* game_versions;       // comma-separated bundle ids (e.g. "java-1.21.8"), or null for any
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
