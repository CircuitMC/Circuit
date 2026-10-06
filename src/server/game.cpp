// SPDX-License-Identifier: AGPL-3.0-only
#include "game.hpp"
#include "extensions.hpp"
#include "log.hpp"
#include "world_meta.hpp"
#include <circuit/gameplay/rules.hpp>
#include <circuit/storage/world_directory.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace circuit::server {
using contracts::BlockPos;
using contracts::ChunkPos;
using contracts::Direction;
using contracts::EntityId;
using gameplay::BlockType;
using gameplay::ChunkLevel;
namespace blocks = gameplay::blocks;
namespace nbt = protocol::nbt;

namespace {
constexpr double max_move_squared = 100.0; // per accepted movement packet
constexpr double reach = 6.0; // creative interaction range 5 plus 1, to the nearest point of the block
constexpr std::size_t chunks_per_tick = 32; // per player
constexpr std::uint32_t spawn_ticket = 1;
constexpr std::uint8_t spawn_radius = 2;
constexpr std::size_t inventory_slots = 46;
constexpr std::int32_t hotbar_first = 36;
constexpr std::uint8_t creative = 1, spectator = 3;

struct Player {
    std::shared_ptr<Peer> peer;
    Uuid uuid;
    std::string name;
    std::vector<ProfileProperty> properties;
    bool verified{};
    std::int32_t entity_id{};
    Vec3 pos;
    float yaw{}, pitch{};
    bool on_ground{}, sneaking{}, flying{};
    std::uint8_t game_mode{creative};
    std::int32_t view_distance{};
    ChunkPos center{};
    std::set<ChunkPos> sent;
    std::uint32_t ticket{};
    std::int32_t next_teleport{1};
    std::optional<std::int32_t> awaiting_teleport;
    std::array<std::pair<std::int32_t, std::int32_t>, inventory_slots> inventory{}; // wire item id, count
    std::int32_t held{};
    std::int32_t inventory_state{};
    std::vector<std::int32_t> acks;
    bool moved{};
    std::set<std::uint64_t> seen_by; // peers whose clients show this player's entity
};

// What the world keeps of a player between sessions. Items are stored by
// name, so saved inventories do not depend on a protocol's numbering.
struct SavedPlayer {
    Vec3 pos;
    float yaw{}, pitch{};
    std::uint8_t game_mode{creative};
    std::array<std::pair<std::string, std::int32_t>, inventory_slots> inventory{};
    std::int32_t held{};
};

std::vector<std::byte> encode_players(const std::map<Uuid, SavedPlayer>& players) {
    protocol::Writer w;
    w.varint(1).varint(static_cast<std::int32_t>(players.size()));
    for (const auto& [uuid, p] : players) {
        w.uuid(uuid).f64(p.pos.x).f64(p.pos.y).f64(p.pos.z).f32(p.yaw).f32(p.pitch).u8(p.game_mode).varint(p.held);
        for (const auto& [name, count] : p.inventory) w.string(name, 256).varint(count);
    }
    return w.take();
}
std::optional<std::map<Uuid, SavedPlayer>> decode_players(std::span<const std::byte> bytes) {
    protocol::Reader r(bytes);
    const auto version = r.varint();
    const auto count = r.length(1u << 20, 30);
    if (!version || *version != 1 || !count) return std::nullopt;
    std::map<Uuid, SavedPlayer> out;
    for (std::size_t i = 0; i < *count; ++i) {
        SavedPlayer p;
        const auto uuid = r.uuid();
        const auto x = r.f64(), y = r.f64(), z = r.f64();
        const auto yaw = r.f32(), pitch = r.f32();
        const auto mode = r.u8();
        const auto held = r.varint();
        if (!uuid || !x || !y || !z || !yaw || !pitch || !mode || !held) return std::nullopt;
        p.pos = {*x, *y, *z};
        p.yaw = *yaw;
        p.pitch = *pitch;
        p.game_mode = *mode;
        p.held = *held;
        for (auto& [name, n] : p.inventory) {
            const auto item = r.string(256);
            const auto c = r.varint();
            if (!item || !c) return std::nullopt;
            name = *item;
            n = *c;
        }
        out[*uuid] = std::move(p);
    }
    if (r.remaining() != 0) return std::nullopt;
    return out;
}

struct Tracked {
    std::int32_t protocol_id{};
    Vec3 last;
    std::set<std::uint64_t> viewers; // peer ids
};

struct Message {
    enum class Kind : std::uint8_t { join, intent, leave } kind;
    std::uint64_t peer{};
    std::optional<JoinRequest> join;
    std::optional<Intent> intent;
};

bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
ChunkPos chunk_at(Vec3 v) {
    return {static_cast<std::int32_t>(std::floor(v.x)) >> 4, static_cast<std::int32_t>(std::floor(v.z)) >> 4};
}
WirePosition wire_pos(BlockPos p) { return {p.x, p.y, p.z}; }
BlockPos block_pos(WirePosition p) { return {p.x, p.y, p.z}; }
Vec3 entity_vec(contracts::FixedVec v) {
    return {static_cast<double>(v.x) / contracts::fixed_one, static_cast<double>(v.y) / contracts::fixed_one,
            static_cast<double>(v.z) / contracts::fixed_one};
}
Uuid entity_uuid(std::uint64_t id) { return Uuid{0x43697263756974ULL /* "Circuit" */, id}; }
std::int32_t chebyshev(ChunkPos a, ChunkPos b) { return std::max(std::abs(a.x - b.x), std::abs(a.z - b.z)); }

// Horizontal direction the player looks at: yaw 0 is south, 90 west, 180 north, 270 east.
// Player Abilities flags: invulnerable, flying, may fly, instant build.
std::uint8_t abilities(std::uint8_t game_mode) { return game_mode == spectator ? 0x01 | 0x02 | 0x04 : 0x01 | 0x04 | 0x08; }

Direction horizontal_look(float yaw) {
    const auto quarter = static_cast<int>(std::floor(static_cast<double>(yaw) / 90.0 + 0.5)) & 3;
    constexpr std::array<Direction, 4> by_quarter{Direction::south, Direction::west, Direction::north, Direction::east};
    return by_quarter[static_cast<std::size_t>(quarter)];
}
Direction look(float yaw, float pitch) {
    if (pitch < -45.0f) return Direction::up;
    if (pitch > 45.0f) return Direction::down;
    return horizontal_look(yaw);
}
} // namespace

struct Game::Impl final : GameServices {
    const ServerConfig& config;
    const contracts::VersionBundle& bundle;
    const Wire& wire;
    const protocol::ContentTables& content;
    const evidence::ContentMapping& mapping;
    const std::vector<RegistryEntries>& registries;
    BlockPalette palette;
    std::unique_ptr<runtime::Engine> engine;
    contracts::CommandQueue queue;
    // Persistence; `world` is empty for a world kept in memory only.
    std::unique_ptr<storage::Filesystem> fs;
    std::unique_ptr<storage::WorldDirectory> world;
    std::uint64_t seed{};
    WorldMeta meta;
    std::map<Uuid, SavedPlayer> saved;
    bool healthy{true}; // false after a tick failed: the state is not saved again

    std::mutex inbox_mutex;
    std::vector<Message> inbox;
    std::atomic<std::size_t> player_count{0};
    std::atomic<std::uint64_t> tick_count{0};

    std::map<std::uint64_t, Player> players;
    std::uint32_t next_ticket{1000};
    std::int32_t next_entity_id{1};
    std::unordered_map<std::uint64_t, Tracked> tracked;
    std::int32_t dimension_type{};
    std::int32_t biome{};
    std::int32_t item_entity_type{}, player_entity_type{};
    std::unordered_map<std::uint16_t, std::int32_t> item_wire; // reference item -> wire item id
    bool started{false};

    // Tick control (freeze and step) and timing of recent ticks.
    bool frozen{false};
    std::uint64_t pending_steps{};
    std::array<double, 1200> durations{}; // milliseconds, ring buffer
    std::size_t durations_seen{};
    std::uint64_t last_tick_ns{}, total_tick_ns{};

    Impl(const ServerConfig& c, const contracts::VersionBundle& b, const Wire& w, const protocol::ContentTables& t,
         const evidence::ContentMapping& m, const std::vector<RegistryEntries>& r)
        : config(c), bundle(b), wire(w), content(t), mapping(m), registries(r), palette(t, m) {
        dimension_type = registry_index(registries, "minecraft:dimension_type", "minecraft:overworld").value();
        biome = registry_index(registries, "minecraft:worldgen/biome", "minecraft:plains").value();
        item_entity_type = content.entity_type("minecraft:item").value();
        player_entity_type = content.entity_type("minecraft:player").value();
        for (std::uint16_t i = 1; i <= gameplay::items::max_id; ++i)
            if (const auto id = content.item_id(mapping.item_name(i))) item_wire[i] = *id;
        open_world();
        extensions = std::make_unique<Extensions>(*this, config, bundle);
        extensions->load();
    }

    runtime::EngineOptions engine_options(std::int32_t floor_y) const { return world_engine_options(bundle, floor_y, config.workers); }

    // ---- persistence ------------------------------------------------------

    // Creates the engine: from the world directory's latest durable state
    // (a checkpoint plus the logged inputs after it), or a new world.
    void open_world() {
        meta = fresh_meta(config.floor_y);
        seed = config.seed;
        if (config.world_directory.empty()) {
            engine = std::make_unique<runtime::Engine>(bundle, seed, engine_options(meta.floor_y));
            log_warn("no world directory configured: the world is not saved");
            return;
        }
        const auto where = config.world_directory.string();
        fs = std::make_unique<storage::PosixFilesystem>();
        world = std::make_unique<storage::WorldDirectory>(*fs, config.world_directory, std::string(bundle.id),
                                                          std::string(bundle.rules->id));
        auto recovered = world->recover();
        if (!recovered) throw std::runtime_error(std::format("world {}: {}", where, recovered.error()));
        if (!*recovered) {
            engine = std::make_unique<runtime::Engine>(bundle, seed, engine_options(meta.floor_y));
            log_info(std::format("creating a new world in {}", where));
        } else {
            auto& r = **recovered;
            for (const auto& note : r.notes) log_warn(std::format("world {}: {}", where, note));
            // An imported world has no server data yet: it keeps the defaults.
            if (const auto it = r.checkpoint.extras.find(std::string(meta_extra)); it != r.checkpoint.extras.end()) {
                const auto loaded = decode_meta(it->second);
                if (!loaded) throw std::runtime_error(std::format("world {}: invalid server data", where));
                meta = *loaded;
            }
            if (const auto it = r.checkpoint.extras.find(std::string(players_extra)); it != r.checkpoint.extras.end()) {
                auto players_data = decode_players(it->second);
                if (!players_data) throw std::runtime_error(std::format("world {}: invalid player data", where));
                saved = std::move(*players_data);
            }
            seed = r.checkpoint.seed;
            if (seed != config.seed || meta.floor_y != config.floor_y)
                log_info(std::format("the world keeps its own seed {} and floor height {}", seed, meta.floor_y));
            const auto restore = [&] {
                return std::make_unique<runtime::Engine>(bundle, seed, r.checkpoint.state, engine_options(meta.floor_y));
            };
            try {
                engine = restore();
            } catch (const std::invalid_argument& e) {
                throw std::runtime_error(std::format("world {}: {}", where, e.what()));
            }
            // Replay the logged inputs. An input the engine cannot apply ends
            // the history before it: the world continues from the last good tick.
            std::size_t applied = 0;
            try {
                for (const auto& input : r.inputs) {
                    engine->advance(input.commands);
                    ++applied;
                    if (!input.commands.empty()) meta.next_admission = std::max(meta.next_admission, input.commands.back().admission + 1);
                }
            } catch (const std::exception& e) {
                log_error(std::format("world {}: tick {} of the log cannot be replayed ({}); continuing from tick {}", where,
                                      r.inputs[applied].tick, e.what(), r.inputs[applied].tick - 1));
                engine = restore();
                for (std::size_t i = 0; i < applied; ++i) engine->advance(r.inputs[i].commands);
            }
            log_info(std::format("world {} recovered at tick {} (checkpoint at tick {} and {} logged ticks)", where, engine->time(),
                                 r.checkpoint.state.time, applied));
        }
        // Players are not connected after a restart: their chunk tickets go.
        queue = contracts::CommandQueue(65536, meta.next_admission);
        for (const auto& t : engine->dump().tickets) {
            if (t.id == spawn_ticket) started = true;
            else admit(contracts::RemoveTicket{t.id});
        }
        next_ticket = meta.next_ticket;
        // A checkpoint of the opened state starts a fresh log segment.
        if (auto ok = save(); !ok) throw std::runtime_error(std::format("world {}: cannot save: {}", where, ok.error()));
    }

    SavedPlayer snapshot(const Player& p) const {
        SavedPlayer s{p.pos, p.yaw, p.pitch, p.game_mode, {}, p.held};
        for (std::size_t i = 0; i < inventory_slots; ++i)
            if (const auto [item, count] = p.inventory[i]; count > 0)
                if (const auto name = content.item_name(item)) s.inventory[i] = {std::string(*name), count};
        return s;
    }

    // Writes a checkpoint of the current state, with the players' data.
    storage::Status save() {
        if (!world) return {};
        const auto started_at = std::chrono::steady_clock::now();
        for (const auto& [id, p] : players) saved[p.uuid] = snapshot(p);
        meta.next_admission = queue.next_admission();
        meta.next_ticket = next_ticket;
        storage::Checkpoint c{std::string(bundle.id), std::string(bundle.rules->id), seed, engine->dump(), {}};
        c.extras[std::string(meta_extra)] = encode_meta(meta);
        c.extras[std::string(players_extra)] = encode_players(saved);
        auto ok = world->checkpoint(c);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_at).count();
        if (ok) log_info(std::format("saved the world at tick {} in {:.1f} ms", engine->time(), ms));
        return ok;
    }

    BlockState at(BlockPos p) const {
        const auto v = engine->view().block(p);
        return v ? *v : blocks::air();
    }
    NeighborLookup lookup() const {
        return [this](BlockPos p) { return at(p); };
    }

    void admit(contracts::CommandBody body) {
        if (!queue.admit(std::move(body))) log_warn("game: command queue full; input dropped");
    }

    void broadcast(const Bytes& packet, std::uint64_t except = 0) {
        for (auto& [id, p] : players)
            if (id != except) p.peer->send(packet);
    }
    void message(Player& p, std::string text, std::string_view color = "gray") {
        p.peer->send(wire.system_chat(nbt::colored_text(std::move(text), color)));
    }

    // ---- joining and leaving ------------------------------------------------

    void on_join(JoinRequest r) {
        for (auto it = players.begin(); it != players.end(); ++it) {
            if (it->second.uuid == r.uuid) {
                it->second.peer->kick("You logged in from another location");
                on_leave(it->first);
                break;
            }
        }
        if (static_cast<std::int32_t>(players.size()) >= config.max_players) {
            r.peer->kick("The server is full");
            return;
        }
        const auto peer_id = r.peer->id();
        Player p;
        p.peer = r.peer;
        p.uuid = r.uuid;
        p.name = r.name;
        p.properties = r.properties;
        p.verified = r.verified;
        p.entity_id = next_entity_id++;
        p.pos = {meta.spawn.x + 0.5, static_cast<double>(meta.spawn.y) + 1.0, meta.spawn.z + 0.5};
        if (const auto it = saved.find(p.uuid); it != saved.end()) restore(p, it->second);
        p.view_distance = std::clamp(r.view_distance, 2, config.view_distance);
        p.center = chunk_at(p.pos);
        p.ticket = next_ticket++;
        admit(contracts::AddTicket{p.ticket, p.center, static_cast<std::uint8_t>(config.simulation_distance)});

        Wire::JoinGame j{p.entity_id, config.max_players, p.view_distance, config.simulation_distance, dimension_type,
                         p.game_mode};
        auto& peer = *p.peer;
        peer.send(wire.join_game(j));
        peer.send(wire.player_abilities(abilities(p.game_mode), 0.05f, 0.1f));
        peer.send(wire.set_held_slot(p.held));
        peer.send(wire.set_health(20.0f, 20, 5.0f));
        peer.send(wire.empty_commands());
        peer.send(wire.spawn_position(wire_pos({meta.spawn.x, meta.spawn.y + 1, meta.spawn.z}), 0.0f));
        peer.send(wire.set_time(static_cast<std::int64_t>(engine->time()), 6000, false));
        // Everyone sees the newcomer; the newcomer sees everyone.
        std::vector<PlayerListEntry> all;
        for (const auto& [id, other] : players) all.push_back(entry(other));
        all.push_back(entry(p));
        peer.send(wire.player_info_add(all));
        broadcast(wire.player_info_add({entry(p)}));
        send_inventory(p);
        peer.send(wire.game_event(13, 0.0f)); // start waiting for level chunks
        peer.send(wire.chunk_center(p.center.x, p.center.z));
        teleport(p, p.pos);
        log_info(std::format("{} joined ({}, {})", p.name, p.uuid.text(), p.verified ? "verified" : "offline"));
        players.emplace(peer_id, std::move(p));
        player_count = players.size();
        auto& joined = players.at(peer_id);
        for (const auto& [id, other] : players) message(players.at(id), std::format("{} joined the game", joined.name), "yellow");
    }

    // Applies saved data; values the movement rules would refuse are ignored.
    void restore(Player& p, const SavedPlayer& s) const {
        if (finite(s.pos) && std::abs(s.pos.x) < 3.0e7 && std::abs(s.pos.z) < 3.0e7 && std::abs(s.pos.y) <= 4096.0) p.pos = s.pos;
        if (std::isfinite(s.yaw) && std::isfinite(s.pitch)) {
            p.yaw = s.yaw;
            p.pitch = std::clamp(s.pitch, -90.0f, 90.0f);
        }
        if (s.game_mode == creative || s.game_mode == spectator) p.game_mode = s.game_mode;
        if (s.held >= 0 && s.held < 9) p.held = s.held;
        for (std::size_t i = 0; i < inventory_slots; ++i) {
            const auto& [name, count] = s.inventory[i];
            if (name.empty() || count < 1 || count > 99) continue;
            if (const auto id = content.item_id(name)) p.inventory[i] = {*id, count};
        }
    }

    PlayerListEntry entry(const Player& p) const {
        return {p.uuid, p.name, p.properties, p.game_mode, true, 0};
    }

    void on_leave(std::uint64_t peer_id) {
        const auto it = players.find(peer_id);
        if (it == players.end()) return;
        auto& p = it->second;
        admit(contracts::RemoveTicket{p.ticket});
        saved[p.uuid] = snapshot(p);
        const auto info = wire.player_info_remove({p.uuid});
        const auto remove = wire.remove_entities({p.entity_id});
        const auto name = p.name;
        for (auto& [tid, t] : tracked) t.viewers.erase(peer_id);
        for (auto& [oid, o] : players) o.seen_by.erase(peer_id);
        players.erase(it);
        player_count = players.size();
        broadcast(info);
        broadcast(remove);
        for (auto& [id, other] : players) message(other, std::format("{} left the game", name), "yellow");
        log_info(std::format("{} left", name));
    }

    void teleport(Player& p, Vec3 to) {
        p.pos = to;
        const auto id = p.next_teleport++;
        p.awaiting_teleport = id;
        p.peer->send(wire.player_position(id, to, p.yaw, p.pitch));
    }

    void send_inventory(Player& p) {
        std::vector<std::pair<std::int32_t, std::int32_t>> slots(p.inventory.begin(), p.inventory.end());
        p.peer->send(wire.inventory_content(++p.inventory_state, slots));
    }

    // ---- intents ----------------------------------------------------------

    void on_intent(std::uint64_t peer_id, Intent intent) {
        const auto it = players.find(peer_id);
        if (it == players.end()) return; // the player left or was never admitted
        auto& p = it->second;
        std::visit([&](auto& i) { handle(p, i); }, intent);
    }

    void handle(Player& p, intent::ConfirmTeleport& t) {
        if (p.awaiting_teleport == t.id) p.awaiting_teleport.reset();
    }

    void handle(Player& p, intent::Move& m) {
        if (m.rotation && std::isfinite(m.rotation->first) && std::isfinite(m.rotation->second)) {
            p.yaw = m.rotation->first;
            p.pitch = std::clamp(m.rotation->second, -90.0f, 90.0f);
            p.moved = true;
        }
        p.on_ground = m.on_ground;
        if (!m.position) return;
        if (p.awaiting_teleport) return; // moves before the teleport is confirmed are stale
        const auto to = *m.position;
        if (!finite(to) || std::abs(to.x) >= 3.0e7 || std::abs(to.z) >= 3.0e7 || std::abs(to.y) > 4096.0) {
            p.peer->kick("Invalid movement");
            return;
        }
        const double dx = to.x - p.pos.x, dy = to.y - p.pos.y, dz = to.z - p.pos.z;
        if (dx * dx + dy * dy + dz * dz > max_move_squared) {
            log_warn(std::format("{} moved too quickly ({:.1f} blocks); corrected", p.name, std::sqrt(dx * dx + dy * dy + dz * dz)));
            teleport(p, p.pos);
            return;
        }
        // Positions outside loaded chunks are refused, so players cannot move
        // into terrain the server has not delivered.
        const auto chunk = chunk_at(to);
        if (!engine->view().block({static_cast<std::int32_t>(std::floor(to.x)), bundle.world.min_y,
                                  static_cast<std::int32_t>(std::floor(to.z))})) {
            teleport(p, p.pos);
            return;
        }
        p.pos = to;
        p.moved = true;
        if (chunk.x != p.center.x || chunk.z != p.center.z) {
            p.center = chunk;
            p.peer->send(wire.chunk_center(chunk.x, chunk.z));
            admit(contracts::RemoveTicket{p.ticket});
            admit(contracts::AddTicket{p.ticket, chunk, static_cast<std::uint8_t>(config.simulation_distance)});
        }
    }

    bool in_reach(const Player& p, BlockPos b) const {
        const double eye = p.pos.y + (p.sneaking ? 1.27 : 1.62);
        const auto axis = [](double v, double low) { return v < low ? low - v : (v > low + 1.0 ? v - low - 1.0 : 0.0); };
        const double dx = axis(p.pos.x, b.x), dy = axis(eye, b.y), dz = axis(p.pos.z, b.z);
        return dx * dx + dy * dy + dz * dz <= reach * reach;
    }

    bool placeable_at(BlockPos b) const {
        if (!gameplay::in_world(bundle, b)) return false;
        const auto s = at(b);
        return s.is_air() || s.is(BlockType::water);
    }

    // A block would intersect a player standing or flying in the world.
    bool occupied_by_player(BlockPos b) const {
        for (const auto& [id, o] : players) {
            if (o.game_mode == spectator) continue;
            const bool x = b.x + 1 > o.pos.x - 0.3 && b.x < o.pos.x + 0.3;
            const bool z = b.z + 1 > o.pos.z - 0.3 && b.z < o.pos.z + 0.3;
            const bool y = b.y + 1 > o.pos.y && b.y < o.pos.y + 1.8;
            if (x && y && z) return true;
        }
        return false;
    }

    void correct(Player& p, BlockPos b) {
        if (gameplay::in_world(bundle, b))
            p.peer->send(wire.block_update(wire_pos(b), display_state(palette, lookup(), b, at(b))));
    }

    void handle(Player& p, intent::Dig& d) {
        const auto b = block_pos(d.pos);
        p.acks.push_back(d.sequence);
        if (d.status != 0 && d.status != 2) return; // creative players break blocks when they start digging
        if (p.game_mode != creative || !in_reach(p, b) || !gameplay::in_world(bundle, b)) {
            correct(p, b);
            return;
        }
        const auto s = at(b);
        if (s.is_air() || s.is(BlockType::piston_head) || s.is(BlockType::moving_piston)) {
            correct(p, b);
            return;
        }
        admit(contracts::SetBlock{b, blocks::air().raw(), std::nullopt});
    }

    // State placed by the held item, oriented by the player and the clicked face.
    std::optional<BlockState> placement(const Player& p, std::string_view item, Direction face) const {
        const auto h = horizontal_look(p.yaw);
        const auto l = look(p.yaw, p.pitch);
        if (item == "minecraft:stone") return blocks::stone();
        if (item == "minecraft:glass") return blocks::glass();
        if (item == "minecraft:obsidian") return blocks::obsidian();
        if (item == "minecraft:redstone_block") return blocks::redstone_block();
        if (item == "minecraft:slime_block") return blocks::slime();
        if (item == "minecraft:redstone_lamp") return blocks::lamp();
        if (item == "minecraft:redstone") return blocks::wire();
        if (item == "minecraft:lever") {
            // Mounted on the clicked face: floor and ceiling levers face the player's
            // look, wall levers point away from the wall.
            if (face == Direction::up) return blocks::lever(false, h, blocks::LeverFace::floor);
            if (face == Direction::down) return blocks::lever(false, h, blocks::LeverFace::ceiling);
            return blocks::lever(false, face, blocks::LeverFace::wall);
        }
        if (item == "minecraft:repeater") return blocks::repeater(h);
        if (item == "minecraft:comparator") return blocks::comparator(h);
        if (item == "minecraft:observer") return blocks::observer(contracts::opposite(l));
        if (item == "minecraft:piston") return blocks::piston(contracts::opposite(l));
        if (item == "minecraft:sticky_piston") return blocks::piston(contracts::opposite(l), true);
        if (item == "minecraft:hopper")
            return blocks::hopper(face == Direction::up || face == Direction::down ? Direction::down : contracts::opposite(face));
        if (item == "minecraft:chest") return blocks::chest(contracts::opposite(h));
        if (item == "minecraft:water_bucket") return blocks::water_source();
        if (item == "minecraft:wheat_seeds") return blocks::wheat();
        return std::nullopt;
    }

    // Blocks that need a solid block below them.
    static bool needs_floor(BlockState s) {
        return s.is(BlockType::redstone_wire) || s.is(BlockType::repeater) || s.is(BlockType::comparator);
    }

    void handle(Player& p, intent::UseOn& u) {
        p.acks.push_back(u.sequence);
        const auto b = block_pos(u.pos);
        if (u.face < 0 || u.face > 5 || p.game_mode == spectator || !in_reach(p, b) || !gameplay::in_world(bundle, b)) {
            // The client predicted a change at the clicked block or next to it.
            correct(p, b);
            if (u.face >= 0 && u.face <= 5) correct(p, b.relative(static_cast<Direction>(u.face)));
            return;
        }
        const auto face = static_cast<Direction>(u.face);
        const auto clicked = at(b);
        const bool usable = clicked.is(BlockType::lever) || clicked.is(BlockType::repeater) || clicked.is(BlockType::comparator);
        if (usable && !p.sneaking) {
            admit(contracts::UseBlock{b});
            return;
        }
        if (u.hand != 0) return; // the off hand holds nothing in this server
        const auto [item, count] = p.inventory[static_cast<std::size_t>(hotbar_first + p.held)];
        const auto target = placeable_at(b) ? b : b.relative(face);
        const auto name = count > 0 ? content.item_name(item) : std::nullopt;
        const auto state = name ? placement(p, *name, face) : std::nullopt;
        if (!state || !placeable_at(target) || !in_reach(p, target)
            || (needs_floor(*state) && !gameplay::is_solid(at(target.relative(Direction::down)).type()))
            || (state->is(BlockType::lever) && (target == b || !gameplay::is_solid(clicked.type())))
            || occupied_by_player(target)) {
            correct(p, b);
            correct(p, target);
            return;
        }
        admit(contracts::SetBlock{target, state->raw(), std::nullopt});
    }

    void handle(Player& p, intent::Use& u) { p.acks.push_back(u.sequence); }

    void handle(Player& p, intent::HeldSlot& h) {
        if (h.slot < 0 || h.slot > 8) {
            p.peer->kick("Invalid hotbar slot");
            return;
        }
        p.held = h.slot;
    }

    void handle(Player& p, intent::CreativeSlot& c) {
        if (p.game_mode != creative) return;
        if (c.slot < 0 || c.slot >= static_cast<std::int16_t>(inventory_slots)) return; // dropping is not supported
        if (c.count < 0 || c.count > 99 || (c.count > 0 && !content.item_name(c.item))) {
            send_inventory(p);
            return;
        }
        p.inventory[static_cast<std::size_t>(c.slot)] = c.count == 0 ? std::pair{0, 0} : std::pair{c.item, c.count};
    }

    void handle(Player& p, intent::Sneak& s) { p.sneaking = s.sneaking; }
    void handle(Player& p, intent::Flying& f) { p.flying = f.flying && (p.game_mode == creative || p.game_mode == spectator); }
    void handle(Player&, intent::Loaded&) {}

    void handle(Player& p, intent::Chat& c) {
        std::string text;
        for (const char ch : c.text)
            if (static_cast<unsigned char>(ch) >= 0x20 && ch != 0x7F) text += ch;
        if (text.empty()) return;
        log_info(std::format("<{}> {}", p.name, text));
        broadcast(wire.system_chat(nbt::text(std::format("<{}> {}", p.name, text))));
    }

    void handle(Player& p, intent::Command& c) {
        std::vector<std::string> args;
        for (std::size_t i = 0; i < c.text.size();) {
            const auto end = c.text.find(' ', i);
            if (end != i) args.emplace_back(c.text.substr(i, end == std::string::npos ? std::string::npos : end - i));
            if (end == std::string::npos) break;
            i = end + 1;
        }
        if (args.empty()) return;
        if (args[0] == "help") {
            std::string extra;
            for (const auto& [name, usage] : extensions->commands()) extra += std::format(", /{}{}{}", name, usage.empty() ? "" : " ", usage);
            message(p, "Commands: /help, /players, /tp <x> <y> <z>, /gamemode <creative|spectator>, /tps" + extra);
        } else if (args[0] == "players") {
            std::string names;
            for (const auto& [id, o] : players) names += (names.empty() ? "" : ", ") + o.name;
            message(p, std::format("{} online: {}", players.size(), names));
        } else if (args[0] == "tp" && args.size() == 4) {
            Vec3 to;
            try {
                to = {std::stod(args[1]), std::stod(args[2]), std::stod(args[3])};
            } catch (const std::exception&) {
                message(p, "Usage: /tp <x> <y> <z>", "red");
                return;
            }
            if (!finite(to) || std::abs(to.x) >= 3.0e7 || std::abs(to.z) >= 3.0e7 || std::abs(to.y) > 4096.0) {
                message(p, "Destination out of range", "red");
                return;
            }
            const auto chunk = chunk_at(to);
            teleport(p, to);
            p.center = chunk;
            p.peer->send(wire.chunk_center(chunk.x, chunk.z));
            admit(contracts::RemoveTicket{p.ticket});
            admit(contracts::AddTicket{p.ticket, chunk, static_cast<std::uint8_t>(config.simulation_distance)});
        } else if (args[0] == "gamemode" && args.size() == 2 && (args[1] == "creative" || args[1] == "spectator")) {
            p.game_mode = args[1] == "creative" ? creative : spectator;
            p.peer->send(wire.game_event(3, static_cast<float>(p.game_mode)));
            // Spectators fly permanently; creative players may fly and build instantly.
            p.peer->send(wire.player_abilities(abilities(p.game_mode), 0.05f, 0.1f));
            broadcast(wire.player_info_add({entry(p)}));
        } else if (args[0] == "tps") {
            message(p, std::format("tick {}", engine->time()));
        } else if (const auto reply = extensions->command(p.name, args[0], std::span(args).subspan(1))) {
            if (!reply->empty()) message(p, *reply, "white");
        } else {
            message(p, std::format("Unknown command: /{}", args[0]), "red");
        }
    }

    // ---- publishing ---------------------------------------------------------

    void publish_blocks() {
        std::set<BlockPos> changed;
        std::set<ChunkPos> unloaded;
        for (const auto& r : engine->trace()) {
            if (r.kind == contracts::TraceKind::block_changed) changed.insert(r.pos);
            if (r.kind == contracts::TraceKind::chunk_unloaded) unloaded.insert(contracts::chunk_of(r.pos));
        }
        // Dust next to a change may change shape on screen.
        std::set<BlockPos> shown = changed;
        for (const auto b : changed)
            for (const auto d : contracts::horizontal_directions)
                for (const int dy : {-1, 0, 1}) {
                    const auto n = b.relative(d).relative(Direction::up, dy);
                    if (gameplay::in_world(bundle, n) && at(n).is(BlockType::redstone_wire)) shown.insert(n);
                }
        const auto look_up = lookup();
        for (const auto b : shown) {
            const auto c = contracts::chunk_of(b);
            std::optional<Bytes> packet;
            for (auto& [id, p] : players) {
                if (!p.sent.contains(c)) continue;
                if (!packet) packet = wire.block_update(wire_pos(b), display_state(palette, look_up, b, at(b)));
                p.peer->send(*packet);
            }
        }
        for (auto& [id, p] : players)
            for (const auto c : unloaded)
                if (p.sent.erase(c)) forget(p, c);
    }

    void forget(Player& p, ChunkPos c) {
        p.peer->send(wire.forget_chunk(c.x, c.z));
        std::vector<std::int32_t> removed;
        for (auto& [eid, t] : tracked) {
            if (contracts::chunk_of(contracts::BlockPos{static_cast<std::int32_t>(std::floor(t.last.x)), 0,
                                                       static_cast<std::int32_t>(std::floor(t.last.z))})
                    == c
                && t.viewers.erase(p.peer->id()))
                removed.push_back(t.protocol_id);
        }
        if (!removed.empty()) p.peer->send(wire.remove_entities(removed));
    }

    void show_entity(Player& p, std::uint64_t engine_id, const gameplay::Entity& e) {
        auto [it, fresh] = tracked.try_emplace(engine_id);
        auto& t = it->second;
        if (fresh) {
            t.protocol_id = next_entity_id++;
            t.last = entity_vec(e.position);
        }
        if (!t.viewers.insert(p.peer->id()).second) return;
        const auto wire_item = item_wire.contains(e.stack.item) ? item_wire.at(e.stack.item) : 0;
        p.peer->send(wire.spawn_entity(t.protocol_id, entity_uuid(engine_id), item_entity_type, t.last, 0, 0, 0, 0));
        p.peer->send(wire.item_metadata(t.protocol_id, wire_item, e.stack.count));
    }

    void publish_entities() {
        const auto view = engine->view();
        std::set<std::uint64_t> touched;
        for (const auto& r : engine->trace()) {
            switch (r.kind) {
            case contracts::TraceKind::entity_spawned: case contracts::TraceKind::entity_changed_chunk:
            case contracts::TraceKind::entity_stack_changed:
                touched.insert(r.a);
                break;
            case contracts::TraceKind::entity_removed:
                if (const auto it = tracked.find(r.a); it != tracked.end()) {
                    const auto packet = wire.remove_entities({it->second.protocol_id});
                    for (const auto viewer : it->second.viewers)
                        if (const auto p = players.find(viewer); p != players.end()) p->second.peer->send(packet);
                    tracked.erase(it);
                }
                break;
            default: break;
            }
        }
        for (const auto id : touched) {
            const auto e = view.entity(EntityId{id});
            if (!e || !*e) continue;
            const auto c = contracts::chunk_of(contracts::block_of((*e)->position));
            for (auto& [pid, p] : players)
                if (p.sent.contains(c)) show_entity(p, id, **e);
            if (const auto it = tracked.find(id); it != tracked.end()) {
                // Stack changes are sent again to everyone who sees the entity.
                const auto wire_item = item_wire.contains((*e)->stack.item) ? item_wire.at((*e)->stack.item) : 0;
                const auto meta = wire.item_metadata(it->second.protocol_id, wire_item, (*e)->stack.count);
                for (const auto viewer : it->second.viewers)
                    if (const auto p = players.find(viewer); p != players.end()) p->second.peer->send(meta);
            }
        }
        // Position sync for moving entities.
        for (auto& [id, t] : tracked) {
            const auto e = view.entity(EntityId{id});
            if (!e || !*e) continue;
            const auto now = entity_vec((*e)->position);
            if (now.x == t.last.x && now.y == t.last.y && now.z == t.last.z) continue;
            t.last = now;
            const auto packet = wire.entity_position(t.protocol_id, now, 0, 0, (*e)->on_ground);
            for (const auto viewer : t.viewers)
                if (const auto p = players.find(viewer); p != players.end()) p->second.peer->send(packet);
        }
    }

    void publish_chunks() {
        const auto shape = WorldShape{bundle.world.min_y, bundle.world.height};
        std::map<ChunkPos, ChunkLevel> loaded;
        for (const auto& [c, level] : engine->loaded_columns()) loaded.emplace(c, level);
        const auto look_up = lookup();
        for (auto& [id, p] : players) {
            // Forget chunks out of range.
            std::vector<ChunkPos> stale;
            for (const auto c : p.sent)
                if (chebyshev(c, p.center) > p.view_distance + 1 || !loaded.contains(c)) stale.push_back(c);
            for (const auto c : stale) {
                p.sent.erase(c);
                forget(p, c);
            }
            // Send the nearest missing chunks, a bounded number per tick.
            std::vector<ChunkPos> wanted;
            for (std::int32_t dx = -p.view_distance; dx <= p.view_distance; ++dx)
                for (std::int32_t dz = -p.view_distance; dz <= p.view_distance; ++dz) {
                    const ChunkPos c{p.center.x + dx, p.center.z + dz};
                    if (!p.sent.contains(c) && loaded.contains(c)) wanted.push_back(c);
                }
            if (wanted.empty()) continue;
            std::ranges::sort(wanted, {}, [&](ChunkPos c) {
                return std::pair{chebyshev(c, p.center), std::abs(c.x - p.center.x) + std::abs(c.z - p.center.z)};
            });
            if (wanted.size() > chunks_per_tick) wanted.resize(chunks_per_tick);
            p.peer->send(wire.chunk_batch_start());
            for (const auto c : wanted) {
                const auto column = engine->column(c);
                if (!column) continue;
                p.peer->send(wire.chunk(encode_chunk(*column, palette, look_up, shape, biome)));
                p.sent.insert(c);
                for (const auto& e : column->entities) show_entity(p, e.id.value, e);
            }
            p.peer->send(wire.chunk_batch_finished(static_cast<std::int32_t>(wanted.size())));
        }
    }

    void publish_players() {
        for (auto& [id, p] : players) {
            if (!p.moved) continue;
            p.moved = false;
            const auto pos = wire.entity_position(p.entity_id, p.pos, p.yaw, p.pitch, p.on_ground);
            const auto head = wire.head_rotation(p.entity_id, p.yaw);
            const auto c = chunk_at(p.pos);
            for (auto& [oid, o] : players) {
                if (oid == id || !o.sent.contains(c)) continue;
                o.peer->send(pos);
                o.peer->send(head);
            }
        }
        // Players appear to others while their chunk is visible to them.
        for (auto& [id, p] : players)
            for (auto& [oid, o] : players) {
                if (oid == id) continue;
                const bool visible = o.sent.contains(chunk_at(p.pos));
                if (visible && p.seen_by.insert(oid).second) {
                    o.peer->send(wire.spawn_entity(p.entity_id, p.uuid, player_entity_type, p.pos, p.pitch, p.yaw, p.yaw, 0));
                } else if (!visible && p.seen_by.erase(oid)) {
                    o.peer->send(wire.remove_entities({p.entity_id}));
                }
            }
    }

    // ---- services for modules (game thread) ------------------------------

    const runtime::Engine& simulation() const override { return *engine; }
    const evidence::ContentMapping& content_mapping() const override { return mapping; }
    std::optional<std::uint64_t> submit(contracts::CommandBody body) override {
        const auto a = queue.admit(std::move(body));
        return a ? std::optional<std::uint64_t>(*a) : std::nullopt;
    }
    bool withdraw(std::uint64_t admission) override { return queue.cancel(admission); }
    bool tell(std::string_view player, std::string_view text) override {
        for (auto& [id, p] : players)
            if (p.name == player) {
                message(p, std::string(text), "white");
                return true;
            }
        return false;
    }
    void freeze(bool value) override {
        frozen = value;
        if (!value) pending_steps = 0;
    }
    void step(std::uint32_t ticks) override {
        frozen = true;
        pending_steps += ticks;
    }
    plugin::TickStatus tick_status() const override {
        plugin::TickStatus s{engine->time(), frozen, pending_steps, 0, 0, 0, 0, 0};
        const auto n = std::min(durations_seen, durations.size());
        if (n == 0) return s;
        std::vector<double> d(durations.begin(), durations.begin() + static_cast<std::ptrdiff_t>(n));
        std::ranges::sort(d);
        double sum = 0;
        for (const auto v : d) sum += v;
        const auto at = [&](double q) { return d[std::min(n - 1, static_cast<std::size_t>(q * static_cast<double>(n)))]; };
        s.mean_ms = sum / static_cast<double>(n);
        s.p50_ms = at(0.50);
        s.p95_ms = at(0.95);
        s.p99_ms = at(0.99);
        s.max_ms = d.back();
        return s;
    }
    plugin::TickMetrics tick_metrics() const override {
        const auto t = engine->time();
        return {t, last_tick_ns, durations_seen == 0 ? 0 : total_tick_ns / durations_seen};
    }

    void tick() {
        healthy = false; // until the tick completes
        if (!started) {
            admit(contracts::AddTicket{spawn_ticket, contracts::chunk_of(meta.spawn), spawn_radius});
            started = true;
        }
        std::vector<Message> batch;
        {
            std::lock_guard lock(inbox_mutex);
            batch.swap(inbox);
        }
        for (auto& m : batch) {
            switch (m.kind) {
            case Message::Kind::join: on_join(std::move(*m.join)); break;
            case Message::Kind::intent: on_intent(m.peer, std::move(*m.intent)); break;
            case Message::Kind::leave: on_leave(m.peer); break;
            }
        }
        if (extensions) extensions->before_tick();
        // While frozen, players move and chunks are sent, but the world waits.
        const bool run = !frozen || pending_steps > 0;
        if (run) advance();
        publish_chunks();
        publish_players();
        for (auto& [id, p] : players) {
            for (const auto seq : p.acks) p.peer->send(wire.block_changed_ack(seq));
            p.acks.clear();
        }
        ++tick_count;
        healthy = true;
        if (!run) return;
        if (extensions) extensions->after_tick();
        if (world && engine->time() % config.autosave_ticks == 0)
            if (auto ok = save(); !ok) {
                log_error(std::format("cannot save the world: {}", ok.error()));
                healthy = false;
                throw std::runtime_error("the world could not be saved; stopping");
            }
    }

    // Runs one engine tick with the admitted commands and publishes its changes.
    void advance() {
        const auto started_at = std::chrono::steady_clock::now();
        if (frozen) --pending_steps;
        auto commands = queue.drain();
        if (world) {
            // The input is logged before it is applied, so that recovery can
            // repeat every tick that may have been published.
            const auto tick = engine->time() + 1;
            if (auto ok = world->append({tick, commands}, tick % config.log_sync_ticks == 0); !ok) {
                log_error(std::format("cannot write the input log: {}", ok.error()));
                // The engine has not advanced: save its state if the disk allows.
                if (auto saved_now = save(); !saved_now) log_error(std::format("cannot save the world: {}", saved_now.error()));
                throw std::runtime_error("the world could not be written; stopping");
            }
        }
        engine->advance(commands);
        publish_blocks();
        publish_entities();
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_at).count());
        last_tick_ns = ns;
        total_tick_ns += ns;
        durations[durations_seen++ % durations.size()] = static_cast<double>(ns) / 1e6;
    }

    // Last member: modules are unloaded before anything they use is destroyed.
    std::unique_ptr<Extensions> extensions;
};

Game::Game(const ServerConfig& config, const contracts::VersionBundle& bundle, const Wire& wire,
           const protocol::ContentTables& content, const evidence::ContentMapping& mapping,
           const std::vector<RegistryEntries>& registries)
    : impl_(std::make_unique<Impl>(config, bundle, wire, content, mapping, registries)) {}
Game::~Game() = default;

void Game::join(JoinRequest request) {
    std::lock_guard lock(impl_->inbox_mutex);
    const auto peer = request.peer->id();
    impl_->inbox.push_back({Message::Kind::join, peer, std::move(request), std::nullopt});
}
void Game::submit(std::uint64_t peer, Intent intent) {
    std::lock_guard lock(impl_->inbox_mutex);
    impl_->inbox.push_back({Message::Kind::intent, peer, std::nullopt, std::move(intent)});
}
void Game::leave(std::uint64_t peer) {
    std::lock_guard lock(impl_->inbox_mutex);
    impl_->inbox.push_back({Message::Kind::leave, peer, std::nullopt, std::nullopt});
}
void Game::tick() { impl_->tick(); }
void Game::shutdown(std::string_view reason) {
    auto& i = *impl_;
    if (i.world && i.healthy) {
        if (auto ok = i.save(); !ok) log_error(std::format("cannot save the world: {}", ok.error()));
        i.healthy = false; // saved once; later ticks are not expected
    }
    if (i.extensions) i.extensions->unload();
    for (auto& [id, p] : impl_->players) p.peer->kick(std::string(reason));
    impl_->players.clear();
    impl_->player_count = 0;
}
std::size_t Game::players() const noexcept { return impl_->player_count.load(); }
std::uint64_t Game::ticks() const noexcept { return impl_->tick_count.load(); }
const runtime::Engine& Game::engine() const noexcept { return *impl_->engine; }

} // namespace circuit::server
