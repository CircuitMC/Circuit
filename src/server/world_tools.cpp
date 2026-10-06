// SPDX-License-Identifier: AGPL-3.0-only
// Conversion between Anvil worlds and Circuit world directories.
#include "world_meta.hpp"
#include <circuit/server/anvil.hpp>
#include <circuit/server/server.hpp>
#include <circuit/storage/world_directory.hpp>
#include <format>

namespace circuit::server {
namespace {
std::expected<const contracts::VersionBundle*, std::string> bundle_for(const ServerConfig& config) {
    const auto b = contracts::BundleRegistry::select_for_simulation(config.game_version);
    if (!b) return std::unexpected(std::format("{} has no executable rules", config.game_version));
    return *b;
}
} // namespace

std::expected<anvil::ImportReport, std::string> import_anvil_world(const std::filesystem::path& source, const ServerConfig& config,
                                                                   const anvil::ImportOptions& options) {
    if (config.world_directory.empty()) return std::unexpected("no world directory configured");
    const auto bundle = bundle_for(config);
    if (!bundle) return std::unexpected(bundle.error());
    storage::PosixFilesystem fs;
    storage::WorldDirectory world(fs, config.world_directory, std::string((*bundle)->id), std::string((*bundle)->rules->id));
    if (fs.exists(config.world_directory / "manifest"))
        return std::unexpected(std::format("{} already holds a world", config.world_directory.string()));
    auto imported = anvil::import_world(source, **bundle, options);
    if (!imported) return std::unexpected(imported.error());
    storage::Checkpoint c{std::string((*bundle)->id), std::string((*bundle)->rules->id), imported->report.seed,
                          std::move(imported->state), {}};
    auto meta = fresh_meta(config.floor_y);
    meta.spawn = imported->report.spawn;
    c.extras[std::string(meta_extra)] = encode_meta(meta);
    // The engine must accept the state before it becomes a world.
    try {
        runtime::Engine check(**bundle, c.seed, c.state);
    } catch (const std::invalid_argument& e) {
        return std::unexpected(std::format("the imported state is not valid: {}", e.what()));
    }
    if (auto ok = world.checkpoint(c); !ok) return std::unexpected(ok.error());
    return std::move(imported->report);
}

std::expected<anvil::ExportReport, std::string> export_anvil_world(const ServerConfig& config, const std::filesystem::path& target) {
    if (config.world_directory.empty()) return std::unexpected("no world directory configured");
    const auto bundle = bundle_for(config);
    if (!bundle) return std::unexpected(bundle.error());
    storage::PosixFilesystem fs;
    storage::WorldDirectory world(fs, config.world_directory, std::string((*bundle)->id), std::string((*bundle)->rules->id));
    auto recovered = world.recover();
    if (!recovered) return std::unexpected(recovered.error());
    if (!*recovered) return std::unexpected(std::format("{} holds no saved world", config.world_directory.string()));
    auto& r = **recovered;
    auto meta = fresh_meta(config.floor_y);
    if (const auto it = r.checkpoint.extras.find(std::string(meta_extra)); it != r.checkpoint.extras.end()) {
        const auto loaded = decode_meta(it->second);
        if (!loaded) return std::unexpected("invalid server data in the world");
        meta = *loaded;
    }
    gameplay::WorldDump state;
    try {
        runtime::Engine engine(**bundle, r.checkpoint.seed, r.checkpoint.state, world_engine_options(**bundle, meta.floor_y, 1));
        for (const auto& input : r.inputs) engine.advance(input.commands);
        state = engine.dump();
    } catch (const std::exception& e) {
        return std::unexpected(std::format("the saved world cannot be replayed: {}", e.what()));
    }
    anvil::ExportOptions options;
    options.level_name = config.world_directory.filename().string();
    options.seed = r.checkpoint.seed;
    options.floor_y = meta.floor_y;
    options.spawn = meta.spawn;
    auto report = anvil::export_world(state, **bundle, target, options);
    if (report)
        for (const auto& note : r.notes) report->notes.push_back("world directory: " + note);
    return report;
}

std::expected<std::uint64_t, std::string> migrate_world(const std::filesystem::path& source, const std::filesystem::path& target,
                                                        std::string_view bundle_id) {
    storage::PosixFilesystem fs;
    const auto identity = storage::WorldDirectory::identity(fs, source);
    if (!identity) return std::unexpected(identity.error());
    if (!*identity) return std::unexpected(std::format("{} holds no saved world", source.string()));
    const auto to = contracts::BundleRegistry::select_for_simulation(bundle_id);
    if (!to) return std::unexpected(std::format("{} has no executable rules", bundle_id));
    if ((*to)->rules->id != (*identity)->rules_id)
        return std::unexpected(std::format("the world runs {} and {} runs {}: the state cannot be migrated", (*identity)->rules_id, bundle_id,
                                           (*to)->rules->id));
    if (fs.exists(target / "manifest")) return std::unexpected(std::format("{} already holds a world", target.string()));
    storage::WorldDirectory from(fs, source, (*identity)->bundle_id, (*identity)->rules_id);
    auto recovered = from.recover();
    if (!recovered) return std::unexpected(recovered.error());
    auto& r = **recovered;
    // Replay the logged inputs so the copy starts from the latest durable tick.
    const auto source_bundle = contracts::BundleRegistry::find((*identity)->bundle_id);
    if (!source_bundle) return std::unexpected(std::format("unknown bundle {}", (*identity)->bundle_id));
    auto meta = fresh_meta(-61);
    if (const auto it = r.checkpoint.extras.find(std::string(meta_extra)); it != r.checkpoint.extras.end()) {
        const auto loaded = decode_meta(it->second);
        if (!loaded) return std::unexpected("invalid server data in the world");
        meta = *loaded;
    }
    storage::Checkpoint c{std::string(bundle_id), (*identity)->rules_id, r.checkpoint.seed, {}, r.checkpoint.extras};
    try {
        runtime::Engine engine(**source_bundle, r.checkpoint.seed, r.checkpoint.state, world_engine_options(**source_bundle, meta.floor_y, 1));
        for (const auto& input : r.inputs) {
            engine.advance(input.commands);
            if (!input.commands.empty()) meta.next_admission = std::max(meta.next_admission, input.commands.back().admission + 1);
        }
        c.state = engine.dump();
    } catch (const std::exception& e) {
        return std::unexpected(std::format("the saved world cannot be replayed: {}", e.what()));
    }
    c.extras[std::string(meta_extra)] = encode_meta(meta);
    storage::WorldDirectory to_dir(fs, target, std::string(bundle_id), (*identity)->rules_id);
    if (auto ok = to_dir.checkpoint(c); !ok) return std::unexpected(ok.error());
    return c.state.time;
}

} // namespace circuit::server
