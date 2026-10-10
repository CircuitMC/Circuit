// SPDX-License-Identifier: AGPL-3.0-only
// Storage measurements for docs/STORAGE_EVALUATION.md: checkpoint sizes and
// costs under different compressors, engine dump and restore times, recovery
// time, and how much of a running world changes between checkpoints.
//
//   storage_bench [--anvil <java-world>] [--chunks N]
//
// zstd and LZ4 are measured when the system's libzstd.so.1 and liblz4.so.1
// can be loaded at run time; Circuit does not build against them.
#include "../../tests/sim/scenarios.hpp"
#include <chrono>
#include <circuit/gameplay/rng.hpp>
#include <circuit/protocol/framing.hpp>
#include <circuit/runtime/engine.hpp>
#include <circuit/server/anvil.hpp>
#include <circuit/storage/world_directory.hpp>
#include <dlfcn.h>
#include <filesystem>
#include <format>
#include <iostream>
#include <set>
#include <unistd.h>

using namespace circuit::test;
using circuit::runtime::Engine;
using circuit::storage::Checkpoint;
using Clock = std::chrono::steady_clock;

namespace {
const VersionBundle& bundle() { return **BundleRegistry::select_for_simulation("java-1.21.8"); }

template <class F>
double best_ms(F&& f, int runs = 3) {
    double best = 1e300;
    for (int i = 0; i < runs; ++i) {
        const auto t = Clock::now();
        f();
        best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - t).count());
    }
    return best;
}

// Optional compressors, loaded from the system libraries if present.
struct Zstd {
    using Bound = std::size_t (*)(std::size_t);
    using Compress = std::size_t (*)(void*, std::size_t, const void*, std::size_t, int);
    using Decompress = std::size_t (*)(void*, std::size_t, const void*, std::size_t);
    using IsError = unsigned (*)(std::size_t);
    Bound bound{};
    Compress compress{};
    Decompress decompress{};
    IsError error{};
    explicit Zstd(void* h)
        : bound(reinterpret_cast<Bound>(dlsym(h, "ZSTD_compressBound"))), compress(reinterpret_cast<Compress>(dlsym(h, "ZSTD_compress"))),
          decompress(reinterpret_cast<Decompress>(dlsym(h, "ZSTD_decompress"))), error(reinterpret_cast<IsError>(dlsym(h, "ZSTD_isError"))) {}
    [[nodiscard]] bool ok() const { return bound && compress && decompress && error; }
};
struct Lz4 {
    using Bound = int (*)(int);
    using Compress = int (*)(const char*, char*, int, int);
    using Decompress = int (*)(const char*, char*, int, int);
    Bound bound{};
    Compress compress{};
    Decompress decompress{};
    explicit Lz4(void* h)
        : bound(reinterpret_cast<Bound>(dlsym(h, "LZ4_compressBound"))), compress(reinterpret_cast<Compress>(dlsym(h, "LZ4_compress_default"))),
          decompress(reinterpret_cast<Decompress>(dlsym(h, "LZ4_decompress_safe"))) {}
    [[nodiscard]] bool ok() const { return bound && compress && decompress; }
};

struct World {
    std::string name;
    WorldDump state;
};

// A creative redstone server: a flat floor four layers deep in every column,
// a lamp line with repeaters every fourth column and a stocked chest every
// eighth.
World synthetic(std::int32_t side) {
    WorldDump world;
    world.time = 100000;
    auto& d = world.level(circuit::contracts::DimensionId::overworld);
    for (std::int32_t cx = 0; cx < side; ++cx)
        for (std::int32_t cz = 0; cz < side; ++cz) {
            d.chunks.push_back({{cx - side / 2, cz - side / 2}, 0, ChunkLevel::unloaded});
            const auto ox = (cx - side / 2) * 16, oz = (cz - side / 2) * 16;
            for (std::int32_t y = -64; y < -60; ++y)
                for (std::int32_t x = 0; x < 16; ++x)
                    for (std::int32_t z = 0; z < 16; ++z) d.blocks.push_back({{ox + x, y, oz + z}, blocks::stone().raw()});
            if ((cx + cz) % 4 == 0)
                for (std::int32_t x = 0; x < 16; ++x) {
                    const auto s = x % 5 == 4 ? blocks::repeater(Direction::east, 2) : x == 15 ? blocks::lamp() : blocks::wire(15 - x);
                    d.blocks.push_back({{ox + x, -60, oz + 8}, s.raw()});
                }
            if ((cx * 3 + cz) % 8 == 0) {
                const BlockPos p{ox + 3, -60, oz + 3};
                d.blocks.push_back({p, blocks::chest(Direction::north, blocks::ChestType::single).raw()});
                BlockEntity be{BlockType::chest, d.next_block_entity_order++, std::vector<ItemStack>(27), 0};
                for (std::size_t s = 0; s < 27; s += 3) be.slots[s] = {items::redstone, 64};
                d.block_entities.push_back({p, be});
            }
        }
    canonicalize(world);
    return {std::format("synthetic redstone server ({} columns)", side * side), std::move(world)};
}

// Worst case for palettes: random supported blocks filling 64 columns.
World dense() {
    WorldDump world;
    auto& d = world.level(circuit::contracts::DimensionId::overworld);
    circuit::gameplay::Rng rng(7);
    const std::array<BlockState, 6> pool{blocks::stone(), blocks::glass(), blocks::obsidian(), blocks::redstone_block(), blocks::slime(), blocks::lamp()};
    for (std::int32_t cx = 0; cx < 8; ++cx)
        for (std::int32_t cz = 0; cz < 8; ++cz) {
            d.chunks.push_back({{cx, cz}, 0, ChunkLevel::unloaded});
            for (std::int32_t y = -64; y < 0; ++y)
                for (std::int32_t x = 0; x < 16; ++x)
                    for (std::int32_t z = 0; z < 16; ++z)
                        d.blocks.push_back({{cx * 16 + x, y, cz * 16 + z}, pool[rng.bounded(pool.size())].raw()});
        }
    canonicalize(world);
    return {"dense random blocks (64 columns, 64 layers)", std::move(world)};
}

World scenario(const Scenario& s) {
    Engine engine(bundle(), s.seed);
    Admissions admit;
    for (Tick t = 1; t <= s.ticks; ++t) engine.advance(admit(s.inputs(t)));
    return {"scenario " + s.name, engine.dump()};
}

void measure(const World& w, const Zstd* zstd, const Lz4* lz4) {
    const Checkpoint c{"java-1.21.8", std::string(bundle().rules->id), 1, w.state, {}};
    const auto body = circuit::storage::checkpoint_body(c);
    std::size_t sections = 0;
    {
        std::set<std::pair<ChunkPos, std::int32_t>> s;
        for (const auto& [p, raw] : w.state.overworld().blocks) s.insert({circuit::contracts::chunk_of(p), p.y >> 4});
        sections = s.size();
    }
    std::cout << std::format("\n## {}\n\n{} columns, {} non-empty sections, {} blocks, {} block entities; body {:.1f} KiB "
                             "({:.1f} bytes per non-empty section)\n\n",
                             w.name, w.state.overworld().chunks.size(), sections, w.state.overworld().blocks.size(), w.state.overworld().block_entities.size(),
                             static_cast<double>(body.size()) / 1024.0, sections ? static_cast<double>(body.size()) / static_cast<double>(sections) : 0.0);
    std::cout << "| Codec | Size (KiB) | Ratio | Compress (ms) | Decompress (ms) |\n| --- | --- | --- | --- | --- |\n";
    const auto row = [&](std::string_view name, std::size_t size, double c_ms, double d_ms) {
        std::cout << std::format("| {} | {:.1f} | {:.2f} | {:.2f} | {:.2f} |\n", name, static_cast<double>(size) / 1024.0,
                                 static_cast<double>(body.size()) / static_cast<double>(std::max<std::size_t>(size, 1)), c_ms, d_ms);
    };
    for (const int level : {0, 1, 3, 6, 9}) {
        std::vector<std::byte> z;
        const auto c_ms = best_ms([&] { z = circuit::protocol::deflate_bytes(body, level); });
        const auto d_ms = best_ms([&] { (void)circuit::protocol::inflate_exact(z, body.size()); });
        row(std::format("zlib {}", level), z.size(), c_ms, d_ms);
    }
    if (zstd)
        for (const int level : {1, 3, 9}) {
            std::vector<std::byte> z(zstd->bound(body.size()));
            std::size_t n = 0;
            const auto c_ms = best_ms([&] { n = zstd->compress(z.data(), z.size(), body.data(), body.size(), level); });
            std::vector<std::byte> back(body.size());
            const auto d_ms = best_ms([&] { (void)zstd->decompress(back.data(), back.size(), z.data(), n); });
            row(std::format("zstd {}", level), n, c_ms, d_ms);
        }
    if (lz4) {
        std::vector<char> z(static_cast<std::size_t>(lz4->bound(static_cast<int>(body.size()))));
        int n = 0;
        const auto c_ms = best_ms([&] {
            n = lz4->compress(reinterpret_cast<const char*>(body.data()), z.data(), static_cast<int>(body.size()), static_cast<int>(z.size()));
        });
        std::vector<char> back(body.size());
        const auto d_ms = best_ms([&] { (void)lz4->decompress(z.data(), back.data(), n, static_cast<int>(back.size())); });
        row("lz4", static_cast<std::size_t>(n), c_ms, d_ms);
    }

    // Whole checkpoint costs at the default level, and the engine side.
    std::vector<std::byte> encoded;
    const auto encode_ms = best_ms([&] { encoded = circuit::storage::encode_checkpoint(c); });
    std::optional<Checkpoint> decoded;
    const auto decode_ms = best_ms([&] { decoded = circuit::storage::decode_checkpoint(encoded).value(); });
    std::unique_ptr<Engine> engine;
    const auto restore_ms = best_ms([&] { engine = std::make_unique<Engine>(bundle(), 1, decoded->state); });
    WorldDump dumped;
    const auto dump_ms = best_ms([&] { dumped = engine->dump(); });
    // Background checkpoints: the game thread only takes a snapshot; the writer
    // encodes it, reusing the encoding of unchanged sections.
    circuit::runtime::WorldSnapshot snap;
    const auto snapshot_ms = best_ms([&] { snap = engine->snapshot(); });
    Checkpoint from_snapshot{c.bundle_id, c.rules_id, c.seed, snap.state, {}};
    circuit::storage::SectionCache cache;
    const auto cold_ms = best_ms([&] {
        circuit::storage::SectionCache fresh;
        (void)circuit::storage::encode_checkpoint(from_snapshot, snap.sections, bundle(), &fresh);
    });
    (void)circuit::storage::encode_checkpoint(from_snapshot, snap.sections, bundle(), &cache);
    const auto warm_ms = best_ms([&] { (void)circuit::storage::encode_checkpoint(from_snapshot, snap.sections, bundle(), &cache); });
    // Recovery through a world directory on the host file system.
    std::string templ = (std::filesystem::temp_directory_path() / "circuit-bench-XXXXXX").string();
    if (!::mkdtemp(templ.data())) throw std::runtime_error("temporary directory");
    circuit::storage::PosixFilesystem fs;
    double write_ms = 0, recover_ms = 0;
    {
        circuit::storage::WorldDirectory dir(fs, std::filesystem::path(templ) / "w", "java-1.21.8", std::string(bundle().rules->id));
        const auto t = Clock::now();
        if (!dir.checkpoint(c)) throw std::runtime_error("checkpoint failed");
        write_ms = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    }
    recover_ms = best_ms([&] {
        circuit::storage::WorldDirectory dir(fs, std::filesystem::path(templ) / "w", "java-1.21.8", std::string(bundle().rules->id));
        const auto r = dir.recover().value();
        Engine restored(bundle(), r->checkpoint.seed, r->checkpoint.state);
    });
    std::filesystem::remove_all(templ);
    std::cout << std::format("\nDefault checkpoint (zlib {}): {:.1f} KiB; encode {:.2f} ms, decode {:.2f} ms. Engine restore {:.2f} ms, "
                             "engine dump {:.2f} ms. Directory checkpoint with fsync {:.2f} ms; recovery to a running engine {:.2f} ms.\n",
                             circuit::storage::CheckpointOptions{}.compression_level, static_cast<double>(encoded.size()) / 1024.0, encode_ms,
                             decode_ms, restore_ms, dump_ms, write_ms, recover_ms);
    std::cout << std::format("Snapshot {:.3f} ms; encoding a snapshot {:.2f} ms with an empty section cache, {:.2f} ms when no section "
                             "changed.\n",
                             snapshot_ms, cold_ms, warm_ms);
}

// Fraction of columns whose blocks or block entities changed between
// checkpoints of a running world.
void changes(const Scenario& s, Tick interval, Tick total) {
    Engine engine(bundle(), s.seed);
    Admissions admit;
    WorldDump last;
    std::size_t intervals = 0;
    double changed_sum = 0, columns_sum = 0;
    for (Tick t = 1; t <= total; ++t) {
        engine.advance(admit(t <= s.ticks ? s.inputs(t) : std::vector<CommandBody>{}));
        if (t % interval != 0) continue;
        auto d = engine.dump();
        if (t > interval) {
            std::map<ChunkPos, std::size_t> before, after;
            const auto key = [](const auto& b) { return std::pair{b.first, b.second}; };
            std::set<std::pair<BlockPos, std::uint32_t>> a, b;
            for (const auto& x : last.overworld().blocks) a.insert(key(x));
            for (const auto& x : d.overworld().blocks) b.insert(key(x));
            std::set<ChunkPos> changed;
            for (const auto& x : a)
                if (!b.contains(x)) changed.insert(circuit::contracts::chunk_of(x.first));
            for (const auto& x : b)
                if (!a.contains(x)) changed.insert(circuit::contracts::chunk_of(x.first));
            for (const auto& [p, be] : d.overworld().block_entities) {
                const auto it = std::ranges::find(last.overworld().block_entities, p, [](const auto& e) { return e.first; });
                if (it == last.overworld().block_entities.end() || !(it->second == be)) changed.insert(circuit::contracts::chunk_of(p));
            }
            changed_sum += static_cast<double>(changed.size());
            columns_sum += static_cast<double>(d.overworld().chunks.size());
            ++intervals;
        }
        last = std::move(d);
    }
    std::cout << std::format("| {} | {} | {} | {:.1f} % |\n", s.name, interval, intervals,
                             columns_sum > 0 ? 100.0 * changed_sum / columns_sum : 0.0);
}
} // namespace

int main(int argc, char** argv) {
    std::string anvil;
    std::int32_t side = 32;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--anvil" && i + 1 < argc) anvil = argv[++i];
        else if (a == "--chunks" && i + 1 < argc) side = std::stoi(argv[++i]);
        else {
            std::cerr << "usage: storage_bench [--anvil <java-world>] [--chunks N]\n";
            return 2;
        }
    }
    void* zh = dlopen("libzstd.so.1", RTLD_NOW | RTLD_LOCAL);
    void* lh = dlopen("liblz4.so.1", RTLD_NOW | RTLD_LOCAL);
    const auto zstd = zh ? std::optional<Zstd>(Zstd(zh)) : std::nullopt;
    const auto lz4 = lh ? std::optional<Lz4>(Lz4(lh)) : std::nullopt;
    std::cout << std::format("# Storage measurements\n\nzstd: {}; LZ4: {}\n", zstd && zstd->ok() ? "measured" : "not available",
                             lz4 && lz4->ok() ? "measured" : "not available");

    std::vector<World> worlds;
    worlds.push_back(scenario(many_machines_scenario()));
    worlds.push_back(scenario(drops_scenario()));
    worlds.push_back(synthetic(side));
    worlds.push_back(dense());
    if (!anvil.empty()) {
        const auto imported = circuit::server::anvil::import_world(anvil, bundle(), {.unsupported = circuit::server::anvil::Unsupported::substitute});
        if (!imported) {
            std::cerr << "anvil: " << imported.error() << '\n';
            return 1;
        }
        worlds.push_back({std::format("imported vanilla world ({})", anvil), imported->state});
    }
    for (const auto& w : worlds) measure(w, zstd && zstd->ok() ? &*zstd : nullptr, lz4 && lz4->ok() ? &*lz4 : nullptr);

    std::cout << "\n## Columns changed between checkpoints\n\n| Workload | Interval (ticks) | Intervals | Columns changed |\n| --- | --- | --- | --- |\n";
    changes(many_machines_scenario(), 20, 400);
    changes(clock_scenario(), 20, 400);
    changes(drops_scenario(), 20, 400);
    changes(many_machines_scenario(), 200, 2000);
    return 0;
}
