// SPDX-License-Identifier: AGPL-3.0-only
// Cost of encoding chunk packets on the game thread, for docs/PERFORMANCE.md.
//   chunk_bench [radius]
#include "../../src/server/registries.hpp"
#include "../../src/server/world_codec.hpp"
#include "../../src/server/world_meta.hpp"
#include <atomic>
#include <chrono>
#include <circuit/contracts/version.hpp>
#include <format>
#include <iostream>
#include <thread>

using namespace circuit;

int main(int argc, char** argv) {
    const std::uint8_t radius = static_cast<std::uint8_t>(argc > 1 ? std::stoi(argv[1]) : 8);
    const auto& bundle = **contracts::BundleRegistry::select_for_simulation("java-1.21.8");
    const auto* tables = protocol::ContentTables::for_protocol(772);
    const auto* mapping = evidence::ContentMapping::for_game_version("1.21.8");
    const server::BlockPalette palette(*tables, *mapping);
    runtime::Engine engine(bundle, 1, server::world_engine_options(bundle, -61, 4));
    engine.advance({{1, contracts::AddTicket{1, {0, 0}, radius}}});
    // Some redstone in every column, so that sections are not single-valued.
    std::vector<contracts::Command> build;
    std::uint64_t admission = 2;
    for (std::int32_t cx = -radius; cx <= radius; ++cx)
        for (std::int32_t cz = -radius; cz <= radius; ++cz)
            for (std::int32_t x = 0; x < 16; ++x)
                build.push_back({admission++, contracts::SetBlock{{cx * 16 + x, -60, cz * 16 + 8}, gameplay::blocks::wire(0).raw(), std::nullopt}});
    engine.advance(build);
    const auto view = engine.view();
    const server::NeighborLookup at = [&](contracts::BlockPos p) {
        const auto s = view.block(p);
        return s ? *s : gameplay::blocks::air();
    };
    std::vector<runtime::ColumnSnapshot> columns;
    for (const auto& [c, level] : engine.loaded_columns())
        if (auto s = engine.column(c)) columns.push_back(std::move(*s));
    const server::WorldShape shape{bundle.world.min_y, bundle.world.height};
    std::size_t bytes = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const auto& c : columns) bytes += server::encode_chunk(c, palette, at, shape, 0, 772).size();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << std::format("{} columns: {:.1f} ms ({:.3f} ms per column), {:.1f} KiB\n", columns.size(), ms, ms / static_cast<double>(columns.size()),
                             static_cast<double>(bytes) / 1024.0);
    // The same work spread over threads, as the server does when sending chunks.
    for (const unsigned threads : {2u, 4u, 8u}) {
        std::atomic<std::size_t> next{0};
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> pool;
        for (unsigned i = 0; i < threads; ++i)
            pool.emplace_back([&] {
                for (std::size_t k; (k = next.fetch_add(1)) < columns.size();) (void)server::encode_chunk(columns[k], palette, at, shape, 0, 772);
            });
        for (auto& t : pool) t.join();
        const auto pms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::cout << std::format("  {} threads: {:.1f} ms\n", threads, pms);
    }
    return 0;
}
