// SPDX-License-Identifier: AGPL-3.0-only
#include "game.hpp"
#include "log.hpp"
#include <circuit/gameplay/rules.hpp>
#include <algorithm>
#include <atomic>
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

struct Game::Impl {
    const ServerConfig& config;
    const contracts::VersionBundle& bundle;
    const Wire& wire;
    const protocol::ContentTables& content;
    const evidence::ContentMapping& mapping;
    const std::vector<RegistryEntries>& registries;
    BlockPalette palette;
    runtime::Engine engine;
    contracts::CommandQueue queue;

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

    Impl(const ServerConfig& c, const contracts::VersionBundle& b, const Wire& w, const protocol::ContentTables& t,
         const evidence::ContentMapping& m, const std::vector<RegistryEntries>& r)
        : config(c), bundle(b), wire(w), content(t), mapping(m), registries(r), palette(t, m),
          engine(b, c.seed, engine_options(c, b)) {
        dimension_type = registry_index(registries, "minecraft:dimension_type", "minecraft:overworld").value();
        biome = registry_index(registries, "minecraft:worldgen/biome", "minecraft:plains").value();
        item_entity_type = content.entity_type("minecraft:item").value();
        player_entity_type = content.entity_type("minecraft:player").value();
        for (std::uint16_t i = 1; i <= gameplay::items::max_id; ++i)
            if (const auto id = content.item_id(mapping.item_name(i))) item_wire[i] = *id;
    }

    static runtime::EngineOptions engine_options(const ServerConfig& c, const contracts::VersionBundle& b) {
        runtime::EngineOptions o;
        o.workers = c.workers;
        const auto floor_y = c.floor_y;
        const auto min_y = b.world.min_y;
        if (b.world.contains_y(floor_y)) {
            // Flat world: one stone layer.
            o.generator = [floor_y, min_y](ChunkPos, const auto& blocks) {
                auto& section = blocks(static_cast<std::size_t>((floor_y - min_y) >> 4));
                const auto y = static_cast<std::size_t>((floor_y - min_y) & 15);
                for (std::size_t i = 0; i < 256; ++i) section[y * 256 + i] = gameplay::blocks::stone();
            };
        }
        return o;
    }

    BlockState at(BlockPos p) const {
        const auto v = engine.view().block(p);
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
        p.pos = {0.5, static_cast<double>(config.floor_y) + 1.0, 0.5};
        p.view_distance = std::clamp(r.view_distance, 2, config.view_distance);
        p.center = chunk_at(p.pos);
        p.ticket = next_ticket++;
        admit(contracts::AddTicket{p.ticket, p.center, static_cast<std::uint8_t>(config.simulation_distance)});

        Wire::JoinGame j{p.entity_id, config.max_players, p.view_distance, config.simulation_distance, dimension_type,
                         p.game_mode};
        auto& peer = *p.peer;
        peer.send(wire.join_game(j));
        peer.send(wire.player_abilities(0x01 | 0x04 | 0x08, 0.05f, 0.1f)); // invulnerable, may fly, instant build (creative)
        peer.send(wire.set_held_slot(p.held));
        peer.send(wire.set_health(20.0f, 20, 5.0f));
        peer.send(wire.empty_commands());
        peer.send(wire.spawn_position(wire_pos({0, config.floor_y + 1, 0}), 0.0f));
        peer.send(wire.set_time(static_cast<std::int64_t>(engine.time()), 6000, false));
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

    PlayerListEntry entry(const Player& p) const {
        return {p.uuid, p.name, p.properties, p.game_mode, true, 0};
    }

    void on_leave(std::uint64_t peer_id) {
        const auto it = players.find(peer_id);
        if (it == players.end()) return;
        auto& p = it->second;
        admit(contracts::RemoveTicket{p.ticket});
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
        if (!engine.view().block({static_cast<std::int32_t>(std::floor(to.x)), bundle.world.min_y,
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
            message(p, "Commands: /help, /players, /tp <x> <y> <z>, /gamemode <creative|spectator>, /tps");
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
            p.peer->send(wire.player_abilities(p.game_mode == creative ? 0x01 | 0x04 | 0x08 : 0x01 | 0x02 | 0x04, 0.05f, 0.1f));
            broadcast(wire.player_info_add({entry(p)}));
        } else if (args[0] == "tps") {
            message(p, std::format("tick {}", engine.time()));
        } else {
            message(p, std::format("Unknown command: /{}", args[0]), "red");
        }
    }

    // ---- publishing ---------------------------------------------------------

    void publish_blocks() {
        std::set<BlockPos> changed;
        std::set<ChunkPos> unloaded;
        for (const auto& r : engine.trace()) {
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
        const auto view = engine.view();
        std::set<std::uint64_t> touched;
        for (const auto& r : engine.trace()) {
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
        for (const auto& [c, level] : engine.loaded_columns()) loaded.emplace(c, level);
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
                const auto column = engine.column(c);
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

    void tick() {
        if (!started) {
            admit(contracts::AddTicket{spawn_ticket, {0, 0}, spawn_radius});
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
        engine.advance(queue.drain());
        publish_blocks();
        publish_entities();
        publish_chunks();
        publish_players();
        for (auto& [id, p] : players) {
            for (const auto seq : p.acks) p.peer->send(wire.block_changed_ack(seq));
            p.acks.clear();
        }
        ++tick_count;
    }
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
    for (auto& [id, p] : impl_->players) p.peer->kick(std::string(reason));
    impl_->players.clear();
    impl_->player_count = 0;
}
std::size_t Game::players() const noexcept { return impl_->player_count.load(); }
std::uint64_t Game::ticks() const noexcept { return impl_->tick_count.load(); }
const runtime::Engine& Game::engine() const noexcept { return impl_->engine; }

} // namespace circuit::server
