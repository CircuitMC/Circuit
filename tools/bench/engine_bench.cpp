// SPDX-License-Identifier: AGPL-3.0-only
// Tick time of the simulation engine on a workload, for docs/PERFORMANCE.md.
//   engine_bench <file.ctrace|file.wl> [--ticks N] [--workers W] [--task-times]
//                [--layout adaptive|single] [--inline N] [--label text]
// A .ctrace workload runs as `circuit-server --profile-fixture` runs it: the
// fixture's setup, its settle time, then N timed ticks. A .wl workload is the
// text format of tools/bench/README.md, for worlds larger than a fixture's
// area. The program prints one line of key=value fields that starts with RUN;
// tools/bench/table.py turns such lines into a table. Engine only: no
// networking, no chunk sending and no persistence.
#include <algorithm>
#include <chrono>
#include <circuit/contracts/commands.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/evidence/content.hpp>
#include <circuit/evidence/fixture_runner.hpp>
#include <circuit/evidence/trace_format.hpp>
#include <circuit/gameplay/block.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <circuit/runtime/engine.hpp>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace circuit;
using contracts::Command;
using contracts::Tick;

namespace {
std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Workload {
    contracts::VersionBundle bundle;
    std::map<Tick, std::vector<Command>> by_tick;
    // Admitted at every tick t with t % period == phase.
    struct Every {
        Tick period{}, phase{};
        contracts::CommandBody body;
    };
    std::vector<Every> every;
    Tick settle{};                    // ticks run before the timed ones
    std::optional<std::int32_t> floor; // y of a generated one-block floor
    gameplay::BlockState floor_state{};
};

Workload load_ctrace(const std::string& path) {
    const auto parsed = evidence::parse_fixture(read_text(path));
    if (!parsed) throw std::runtime_error(std::format("{}:{}: {}", path, parsed.error().line, parsed.error().message));
    const auto inputs = evidence::fixture_inputs(*parsed);
    if (!inputs) throw std::runtime_error(std::format("cannot run {}: {}", path, inputs.error()));
    return {inputs->bundle, inputs->by_tick, {}, parsed->ticks + 2, std::nullopt, {}};
}

Workload load_wl(const std::string& path) {
    std::istringstream in(read_text(path));
    const auto selected = contracts::BundleRegistry::select_for_simulation("java-1.21.8");
    if (!selected) throw std::runtime_error("java-1.21.8 has no executable rules");
    Workload w{**selected, {}, {}, 40, std::nullopt, {}};
    const auto* mapping = evidence::ContentMapping::for_game_version(w.bundle.schema.content_release);
    if (!mapping) throw std::runtime_error("no content mapping for the bundle");
    std::map<std::string, gameplay::BlockState> states;
    const auto state = [&](const std::string& notation) {
        if (const auto it = states.find(notation); it != states.end()) return it->second;
        const auto b = mapping->block(notation);
        if (!b) throw std::runtime_error("block " + notation + ": " + b.error());
        return states[notation] = *b;
    };
    std::uint64_t admission = 1;
    // The fixture runner's game rules unless the file says `norules`, so that
    // .wl and .ctrace workloads run under the same conditions.
    bool fixture_rules = true;
    std::vector<std::pair<std::string, std::int32_t>> rules;
    std::string line;
    std::size_t number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        const auto required = [&] {
            if (ls.fail()) throw std::runtime_error(std::format("{}:{}: cannot read: {}", path, number, line));
        };
        std::string word;
        ls >> word;
        if (word == "settle") {
            ls >> w.settle;
        } else if (word == "floor") {
            std::int32_t y{};
            std::string notation = "minecraft:stone";
            ls >> y;
            required();
            ls >> notation; // optional
            ls.clear();
            w.floor = y;
            w.floor_state = state(notation);
        } else if (word == "norules") {
            fixture_rules = false;
        } else if (word == "rule") {
            std::string name;
            std::int32_t value{};
            ls >> name >> value;
            rules.push_back({name, value});
        } else if (word == "ticket") {
            std::uint32_t id{};
            std::int32_t cx{}, cz{};
            int radius{};
            std::string type = "forced";
            ls >> id >> cx >> cz >> radius;
            required();
            ls >> type; // optional
            ls.clear();
            if (radius < 0 || radius > 255) throw std::runtime_error(std::format("{}:{}: a ticket radius outside 0 to 255", path, number));
            contracts::AddTicket ticket{id, {cx, cz}, static_cast<std::uint8_t>(radius)};
            if (type == "player") {
                // The two tickets a server gives a player: one that loads the
                // view distance, and one that simulates the simulation
                // distance, here the smaller of the radius and 10.
                ticket.type = contracts::TicketType::player_loading;
                w.by_tick[1].push_back({admission++, ticket});
                w.by_tick[1].push_back({admission++, contracts::AddTicket{id + 1000000, {cx, cz}, 0, contracts::TicketType::player_simulation,
                                                                          static_cast<std::uint8_t>(31 - std::min(radius, 10)), 0}});
            } else if (type == "forced") {
                w.by_tick[1].push_back({admission++, ticket});
            } else {
                throw std::runtime_error(std::format("{}:{}: unknown ticket type {}", path, number, type));
            }
        } else if (word == "presence") {
            std::uint64_t id{};
            double x{}, y{}, z{};
            ls >> id >> x >> y >> z;
            contracts::PlayerPresence presence{};
            presence.player = contracts::PlayerKey{id, 0};
            presence.position = {x, y, z};
            w.by_tick[1].push_back({admission++, presence});
        } else if (word == "at" || word == "every") {
            Tick tick = 0, period = 0, phase = 0;
            if (word == "at") ls >> tick;
            else ls >> period >> phase;
            std::string kind;
            ls >> kind;
            contracts::CommandBody body;
            contracts::BlockPos p;
            if (kind == "set") {
                std::string notation;
                ls >> p.x >> p.y >> p.z >> notation;
                body = contracts::SetBlock{p, state(notation).raw(), std::nullopt};
            } else if (kind == "use") {
                ls >> p.x >> p.y >> p.z;
                body = contracts::UseBlock{p};
            } else if (kind == "insert") {
                int slot{}, count{};
                std::string item;
                ls >> p.x >> p.y >> p.z >> slot >> item >> count;
                const auto id = mapping->item(item);
                if (!id) throw std::runtime_error("item " + item + ": " + id.error());
                body = contracts::InsertItem{p, static_cast<std::uint8_t>(slot), {*id, static_cast<std::uint8_t>(count)}};
            } else {
                throw std::runtime_error(std::format("{}:{}: unknown action {}", path, number, kind));
            }
            if (word == "every" && period == 0) throw std::runtime_error(std::format("{}:{}: a period of 0", path, number));
            // As in a fixture: action tick 0 is admitted at engine tick 2,
            // after the tick that loads the columns.
            if (word == "at") w.by_tick[tick + 2].push_back({admission++, std::move(body)});
            else w.every.push_back({period, phase, std::move(body)});
        } else {
            throw std::runtime_error(std::format("{}:{}: unknown line: {}", path, number, line));
        }
        required();
    }
    auto& first = w.by_tick[1];
    if (fixture_rules) {
        first.push_back({admission++, contracts::SetGameRule{"randomTickSpeed", 0}});
        first.push_back({admission++, contracts::SetGameRule{"doDaylightCycle", 0}});
        first.push_back({admission++, contracts::SetGameRule{"doWeatherCycle", 0}});
    }
    // From tick 2: on the tick that loads the first columns the level has no
    // light yet, which a random tick may read.
    for (const auto& [name, value] : rules) w.by_tick[2].push_back({admission++, contracts::SetGameRule{name, value}});
    return w;
}

int usage() {
    std::cerr << "usage: engine_bench <file.ctrace|file.wl> [--ticks N] [--workers W] [--task-times]\n"
                 "                    [--layout adaptive|single] [--inline N] [--label text]\n"
                 "  --ticks N      timed ticks after the workload settled (600)\n"
                 "  --workers W    engine workers, 1 to 64 (1)\n"
                 "  --task-times   also time every task step: fills work= and critical=, and adds\n"
                 "                 two clock reads per root to the tick time\n"
                 "  --layout       region layout policy (adaptive)\n"
                 "  --inline N     roots a phase attempt runs on the calling thread (engine default)\n"
                 "  --label text   copied into the output line\n";
    return 2;
}
} // namespace

int main(int argc, char** argv) try {
    std::string path, layout = "adaptive", label = "-";
    unsigned ticks = 600, workers = 1;
    std::optional<std::size_t> inline_roots;
    bool task_times = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(arg + " needs a value");
            return argv[++i];
        };
        if (arg == "--ticks") ticks = static_cast<unsigned>(std::stoul(value()));
        else if (arg == "--workers") workers = static_cast<unsigned>(std::stoul(value()));
        else if (arg == "--inline") inline_roots = std::stoul(value());
        else if (arg == "--layout") layout = value();
        else if (arg == "--task-times") task_times = true;
        else if (arg == "--label") label = value();
        else if (arg.starts_with("--") || !path.empty()) return usage();
        else path = arg;
    }
    if (path.empty() || ticks == 0 || workers == 0 || workers > 64 || (layout != "adaptive" && layout != "single")
        || label.empty() || label.find_first_of(" \t\n") != std::string::npos)
        return usage();

    const Workload w = path.ends_with(".wl") ? load_wl(path) : load_ctrace(path);
    runtime::EngineOptions options;
    options.workers = workers;
    options.record_trace = false;
    options.measure_tasks = task_times;
    if (inline_roots) options.inline_roots = *inline_roots;
    if (layout == "single") options.layout = runtime::LayoutPolicy::single_region;
    if (w.floor) {
        const auto min_y = w.bundle.world.min_y;
        if (*w.floor < min_y || *w.floor >= min_y + w.bundle.world.height) throw std::runtime_error("the floor is outside the world");
        const auto section = static_cast<std::size_t>((*w.floor - min_y) >> 4);
        const auto layer = static_cast<std::size_t>((*w.floor - min_y) & 15);
        options.generator = [section, layer, fill = w.floor_state](contracts::ChunkKey key, const auto& blocks) {
            if (key.dimension != contracts::DimensionId::overworld) return;
            auto& states = blocks(section);
            for (std::size_t i = 0; i < 256; ++i) states[layer * 256 + i] = fill;
        };
    }
    runtime::Engine engine(w.bundle, 0, options);
    const std::vector<Command> none;
    std::vector<Command> scratch;
    std::uint64_t admission = 1ull << 40; // above every admission number of the file
    const auto step = [&] {
        const auto t = engine.time() + 1;
        const auto it = w.by_tick.find(t);
        if (w.every.empty() || t <= 2) {
            engine.advance(it == w.by_tick.end() ? none : it->second);
            return;
        }
        scratch.clear();
        if (it != w.by_tick.end()) scratch = it->second;
        for (const auto& e : w.every)
            if (t % e.period == e.phase) scratch.push_back({admission++, e.body});
        engine.advance(scratch);
    };
    while (engine.time() < w.settle) step(); // Load, setup and settle.

    const auto before = engine.stats();
    std::vector<double> durations;
    durations.reserve(ticks);
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < ticks; ++i) {
        const auto t = std::chrono::steady_clock::now();
        step();
        durations.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
    }
    const auto total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const auto after = engine.stats();

    std::ranges::sort(durations);
    const auto at = [&](double q) {
        return durations[std::min(durations.size() - 1, static_cast<std::size_t>(q * static_cast<double>(durations.size())))];
    };
    const auto ms = [](std::uint64_t ns) { return static_cast<double>(ns) / 1e6; };
    // Phase times are totals over the timed ticks, in milliseconds.
    double phases = 0;
    std::string phase_text;
    for (std::size_t p = 0; p < contracts::phase_order.size(); ++p) {
        const auto v = ms(after.phase_nanoseconds[p] - before.phase_nanoseconds[p]);
        if (p > 0) phases += v;
        phase_text += std::format(" ph_{}={:.2f}", contracts::phase_name(contracts::phase_order[p]), v);
    }
    const auto commands = ms(after.phase_nanoseconds[0] - before.phase_nanoseconds[0]);
    const auto runner = ms(after.runner_nanoseconds - before.runner_nanoseconds);
    const auto dump = engine.dump();
    std::size_t blocks = 0, block_entities = 0, entities = 0, pending = 0;
    for (const auto& level : dump.levels) {
        blocks += level.blocks.size();
        block_entities += level.block_entities.size();
        entities += level.entities.size();
        pending += level.ticks.size();
    }
    const auto name = path.substr(path.find_last_of('/') + 1);
    std::cout << std::format(
        "RUN wl={} label={} workers={} ticks={} task_times={} total_ms={:.2f} mean={:.4f} p50={:.4f} p95={:.4f} p99={:.4f} max={:.4f}{} "
        "work={:.2f} critical={:.2f} runner={:.2f} commit={:.2f} light={:.2f} outside_runner={:.2f} layout_rest={:.2f} regions={} loaded={} "
        "max_tasks={} phases={} attempts={} tasks={} conflicts={} merges={} splits={} digest={:#018x} blocks={} block_entities={} entities={} "
        "pending={}\n",
        name, label, workers, ticks, task_times ? 1 : 0, total, total / ticks, at(0.50), at(0.95), at(0.99), durations.back(), phase_text,
        ms(after.work_nanoseconds - before.work_nanoseconds), ms(after.critical_nanoseconds - before.critical_nanoseconds), runner,
        ms(after.commit_nanoseconds - before.commit_nanoseconds), ms(after.light_nanoseconds - before.light_nanoseconds), phases - runner,
        total - phases - commands, engine.region_count(), engine.loaded_chunks(), after.max_tasks_in_phase, after.phases - before.phases,
        after.attempts - before.attempts, after.tasks - before.tasks, after.conflict_aborts - before.conflict_aborts,
        after.merges - before.merges, after.splits - before.splits, gameplay::digest(dump), blocks, block_entities, entities, pending);
    return 0;
} catch (const std::exception& e) {
    std::cerr << "engine_bench: " << e.what() << "\n";
    return 1;
}
