// SPDX-License-Identifier: AGPL-3.0-only
#include "game.hpp"
#include "command_reader.hpp"
#include "extensions.hpp"
#include "log.hpp"
#include "movement.hpp"
#include "world_meta.hpp"
#include <circuit/evidence/fixture_runner.hpp>
#include <circuit/gameplay/attributes.hpp>
#include <circuit/gameplay/block_registry.hpp>
#include <circuit/gameplay/damage.hpp>
#include <circuit/gameplay/effects.hpp>
#include <circuit/gameplay/equipment.hpp>
#include <circuit/gameplay/experience.hpp>
#include <circuit/gameplay/food.hpp>
#include <circuit/gameplay/potions.hpp>
#include <circuit/gameplay/rules.hpp>
#include <circuit/gameplay/survival.hpp>
#include <circuit/storage/world_directory.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <atomic>
#include <asio/post.hpp>
#include <asio/thread_pool.hpp>
#include <chrono>
#include <cmath>
#include <latch>
#include <limits>
#include <condition_variable>
#include <thread>
#include <tuple>
#include <cstdlib>
#include <format>
#include <deque>
#include <map>
#include <numbers>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

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
// Block interaction range plus 1, to the nearest point of the block: 5 + 1 in
// creative, 4.5 + 1 otherwise.
constexpr double creative_reach = 6.0, survival_reach = 5.5;
// How far the view of an item's use goes: the block interaction range.
constexpr double creative_view = 5.0, survival_view = 4.5;
constexpr std::size_t chunks_per_tick = 32; // per player
constexpr std::uint32_t spawn_ticket = 1;
constexpr std::size_t inventory_slots = contracts::player_slots;
constexpr std::uint8_t survival = 0, creative = 1, spectator = 3;

// The level players are in. The overworld is the only level generated and
// reachable so far; the engine keeps every level's state apart (C2.1), and
// travel between levels arrives with portals.
constexpr auto overworld = contracts::DimensionId::overworld;

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
    std::uint32_t ticket{}; // and ticket + 1: the loading and the simulation ticket
    std::optional<std::tuple<contracts::Vec3, bool, std::int32_t>> presence; // last sent (position, spectator, held)
    std::optional<std::tuple<float, std::int32_t, std::int32_t>> experience; // last sent (progress, level, total)
    std::int32_t next_teleport{1};
    std::optional<std::int32_t> awaiting_teleport;
    std::int32_t held{}; // hotbar slot 0-8; the inventory itself is simulation state
    // A survival player's block breaking in progress: the block, the tick it
    // started, and whether the client finished early (then it completes when
    // the progress reaches the whole block).
    struct Digging {
        BlockPos pos;
        std::uint64_t start{};
        bool delayed{};
    };
    std::optional<Digging> digging;
    // The block the player last clicked in this tick, with the hand: a use of
    // the item that follows (a bucket) goes along a view that met it.
    struct UsedOn {
        std::uint64_t tick{};
        std::int32_t hand{};
        contracts::BlockHit hit;
    };
    std::optional<UsedOn> used_on;
    // The container window the player has open, if any (window ids 1-100).
    struct Window {
        std::int32_t id{};
        BlockPos pos;
        std::size_t container_slots{};
        // The window's own state id (the own window keeps `inventory_state`),
        // as each of the target version's menus counts its packets.
        std::int32_t state{};
        // The window's properties as last sent (a furnace's fuel ticks left
        // and burn time, cooking progress and time; a stonecutter's recipe),
        // INT32_MIN when not sent yet.
        static constexpr std::int32_t unsent = std::numeric_limits<std::int32_t>::min();
        std::array<std::int32_t, 10> data{unsent, unsent, unsent, unsent, unsent, unsent, unsent, unsent, unsent, unsent};
    };
    std::optional<Window> window;
    std::int32_t next_window{1};
    // The client's view of its window, as the target version's container
    // synchronizer keeps it: the window it describes and its slots and carried
    // stack as last sent. Unknown entries (nullopt), and the whole window when
    // `remote_window` differs, are sent at the next synchronization.
    std::int32_t remote_window{-1};
    std::vector<std::optional<WireStack>> remote;
    std::optional<WireStack> remote_carried;
    // While another window is open, the client's view of its own window (as
    // the target version's player menu keeps it): the player slots the open
    // window shows follow it, the others stay as last sent; empty when not
    // known. Closing the window makes it the window the client describes.
    std::vector<std::optional<WireStack>> own;
    // A CloseContainer of this player not yet applied: the stacks it puts
    // back into the inventory are sent as the official servers send them.
    std::optional<std::uint64_t> closing;
    // The ContainerClicks of this player not yet seen applied: each change
    // of a crafting grid's slot they make sends the result again.
    std::vector<std::uint64_t> clicks;
    // The item uses (PlayerUseItem) and drops (DropItem) of this player not
    // yet seen applied, with the slot each takes from: a use that changes
    // that slot sends the whole own window; a drop's change is not sent, the
    // client having made it (client/window_sync).
    struct HandAction {
        std::uint64_t admission{};
        std::uint8_t slot{};
        bool drop{};
        contracts::ItemStack left; // a drop's: the stack it leaves, as the server knew the slot at admission
    };
    std::vector<HandAction> hand_actions;
    std::int32_t inventory_state{}; // the own window's state id
    std::vector<std::int32_t> acks;
    bool moved{};
    std::set<std::uint64_t> seen_by; // peers whose clients show this player's entity
    // Vitals, kept by the server as the target version keeps them on the
    // player: health and hunger (food, saturation, exhaustion and the
    // regeneration timer), the distance fallen since the ground, the hurt
    // cooldown (damage within its last 10 ticks counts only above the last
    // hurt), floating without sinking, and death; plus what the client was
    // last told.
    float health{20.0f};
    std::int32_t food{20};
    float saturation{5.0f};
    float exhaustion{};
    std::int32_t food_timer{};
    std::uint64_t ticks_alive{};
    double fall_distance{};
    std::int32_t hurt_cooldown{};
    float last_hurt{};
    bool hurt_marked{}; // hurt since its velocity was last told
    // The velocity the server keeps for the player (client/combat): one a
    // knockback or a jump set since the player's velocity was last told (an
    // impulse; else a standing player's), and the last told of it to those
    // who see the player (a new entity's: none).
    std::optional<Vec3> impulse;
    Vec3 told_velocity{};
    // The attack's charge (client/combat, combat-timing): the ticks since the
    // player's last attack or swing, or since the main hand's item changed,
    // and that item as of the player's last tick.
    std::int32_t attack_ticks{};
    std::uint16_t main_item{};
    // The arm's swing (client/combat): the ticks into the current swing (6
    // long), none when not swinging. Another swing shows only from halfway.
    std::optional<std::int32_t> swing_ticks;
    // Burning (client/fire): the remaining fire ticks (-20 when not on fire:
    // a player in fire catches it once they reach 0), whether the player
    // touched fire in the last tick, and the on-fire flag the player's last
    // tick decided (told at the next entity update).
    std::int32_t fire_ticks{-20};
    bool in_fire{};
    bool burning{};
    // The tick in which a hurt wore the player's armor: its slots, and
    // (`wear_seen`: but for 1.21.4's lava) what others see of it, are told
    // at the next tick, after the hurt's velocity (client/fire).
    std::optional<std::uint64_t> wear_tick;
    bool wear_seen{};
    std::uint64_t wear_slots{}; // the inventory slots held back (a bit each): the armor's, an attack's weapon's
    // Hurt by an attack in this tick: the health is told at the next, after
    // the wear held back (client/combat).
    bool attacked{};
    // Breath and frost (client/drown): the air supply (entity data 1) and
    // the frozen ticks (entity data 7), with what was last told of them;
    // whether the box met powder snow at the last tick.
    std::int32_t air{gameplay::damage::max_air};
    std::int32_t frozen_ticks{};
    std::int32_t told_air{gameplay::damage::max_air};
    std::int32_t told_frozen{};
    bool in_snow{};
    // The pose the space around the player forced at their last tick
    // (client/drown: crawling, 3, with a block at the head; crouching, 5,
    // where only a crouching player fits, by minecraft.wiki), if any.
    std::optional<std::int32_t> forced_pose;
    // Since the player's last tick: teleported, or moved inside a sweet
    // berry bush (a hurt's velocity is then told as 0: client/drown);
    // teleported; and where the player was at it.
    bool stopped{};
    bool teleported{};
    Vec3 tick_pos;
    bool tick_in_lava{}; // in lava at the end of the player's last tick (a hurt's velocity)
    std::int32_t dead_ticks{}; // since death; at 20 the body goes (client/player_data)
    // The client's settings the entity data shows: the skin parts (index 17)
    // and the main hand (18, 1 right); and what was last told of the
    // entity's data: the health (none for a new entity: joined or
    // respawned), the skin parts and the main hand (a new entity's are 0 and
    // right).
    std::uint8_t skin_parts{};
    std::uint8_t main_arm{1};
    std::optional<float> told_health;
    std::uint8_t told_skin_parts{};
    std::uint8_t told_main_arm{1};
    std::uint8_t told_flags{};  // the shared flags (index 0) last told
    std::int32_t told_pose{};   // the pose (index 6) last told: 0 standing
    // What the player eats or drinks (client/eat): the hand (0 main, 1 off)
    // and its slot, the stack, the use's length and the ticks it has left;
    // once they ran out, the consumption admitted (ConsumeItem), finished
    // with the rules' tick. The living flags (index 8: 1 using an item, 2
    // the off hand, which stays after the use) and those last told; the
    // items whose use waits for a cooldown, with the tick it ends.
    struct Using {
        std::int32_t hand{};
        std::uint8_t slot{};
        contracts::ItemStack stack;
        std::int32_t ticks{};
        std::int32_t left{};
        std::optional<std::uint64_t> consuming;
        bool shield{}; // a shield raised (client/shield): held until let go, never used up
    };
    std::optional<Using> using_item;
    std::uint8_t living{};
    std::uint8_t told_living{};
    std::map<std::uint16_t, std::uint64_t> cooldowns;
    // What the player holds and wears, by equipment slot, as last looked at
    // (client/armor): its attribute modifiers and what those who see the
    // player are told follow it; not looked at yet after joining or
    // respawning (that look adds the modifiers, without sounds or equipment
    // packets: the player appears to others with it).
    std::array<contracts::ItemStack, gameplay::equipment::slot_count> equipped{};
    bool equipment_known{};
    // What the attribute modifiers follow: the equipment as last looked
    // at, also while what others see of it is held back (client/combat: a
    // weapon worn by an attack and switched away in its tick).
    std::array<contracts::ItemStack, gameplay::equipment::slot_count> attributed{};
    // A spectator's camera: the entity whose view it follows (client/spectators).
    std::optional<std::int32_t> camera;
    // A new entity's first data is due (`fresh`); once sent, it and the tick
    // it went in: those who see the entity appear later in that tick get it
    // after the appearance, as both official servers send it twice then.
    bool fresh{};
    std::optional<Bytes> fresh_data;
    std::uint64_t fresh_data_tick{};
    bool respawn_attributes{}; // a respawn's attributes, after its first data
    bool floating{};
    std::int32_t floating_ticks{};
    bool sprinting{};
    bool dead{};
    float sent_health{-1.0f};
    std::int32_t sent_food{-1};
    bool sent_saturation_zero{};
    // Health and experience wait for the end of the joining tick (on_join).
    bool vitals_pending{};
    // Status effects by effect id, and what clients were last told of them:
    // the shared flags they set (invisibility, glowing), the effect
    // particles and their ambience; dirty when they may have changed, and
    // what the end of a tick decided of them, sent at the start of the next.
    std::vector<gameplay::effects::Effect> effects;
    struct EffectView {
        std::uint8_t flags{};
        std::vector<gameplay::effects::Particle> particles;
        bool ambient{};
        float absorption{};
    };
    EffectView effect_view;
    bool effects_dirty{};
    bool absorption_dirty{}; // the absorption alone changed (damage, a lower maximum)
    struct Decided {
        EffectView view;
        bool effects{}; // effects changed, not only the absorption
    };
    std::optional<Decided> effects_decided;
    // Attributes (client/attributes): those the player has (made on first
    // joining, by the game mode, sprinting and effects; kept across
    // respawns and saved), in the registry's order, each with its modifiers
    // in the order added; those changed in a tick, decided at its end and
    // sent after the entity data at the start of the next. A player joining
    // for the first time has none until the end of the joining tick; one
    // rejoining or respawning is sent all of them before the health, and
    // whoever sees them gets them again at the end of that tick, then the
    // game mode's (Arrival). The absorption the absorption effect gives is
    // entity data (15).
    std::vector<gameplay::attributes::Instance> attributes;
    std::set<gameplay::attributes::Attribute> attributes_changed;
    std::vector<gameplay::attributes::Instance> attributes_decided;
    enum class Arrival : std::uint8_t { none, first, again };
    Arrival arrival{};
    float absorption{};
    // The maps the client was sent, with the markers it last got for each.
    // The player's recipe book, and their inventory (armor, main, hotbar,
    // off hand) as its triggers last saw it.
    PlayerBook book;
    std::vector<contracts::ItemStack> book_inventory = std::vector<contracts::ItemStack>(41);
    // Per map the player carries: the markers and colors last sent.
    struct SentMap {
        std::vector<gameplay::MapMarker> markers;
        std::vector<std::uint8_t> colors;
    };
    std::map<std::int32_t, SentMap> maps_sent;
};

// What the world keeps of a player between sessions besides the inventory,
// which is simulation state.
// A recipe book by recipe and advancement ids (kept across data pack changes).
struct SavedBook {
    std::vector<std::string> known, highlighted;
    std::array<bool, 8> settings{};
    std::vector<std::string> done;
    std::vector<std::pair<std::string, std::vector<std::string>>> progress; // advancement, criteria done
};
struct SavedPlayer {
    Vec3 pos;
    float yaw{}, pitch{};
    std::uint8_t game_mode{creative};
    std::int32_t held{};
    float health{20.0f};
    std::int32_t food{20};
    float saturation{5.0f};
    float exhaustion{};
    SavedBook book;
    std::vector<gameplay::effects::Effect> effects;
    std::vector<std::string> attributes; // those the player has, by name
    float absorption{};
    std::int32_t fire_ticks{-20};
    std::int32_t air{gameplay::damage::max_air};
    std::int32_t frozen_ticks{};
};

void write_strings(protocol::Writer& w, const std::vector<std::string>& v) {
    w.varint(static_cast<std::int32_t>(v.size()));
    for (const auto& s : v) w.string(s);
}
std::optional<std::vector<std::string>> read_strings(protocol::Reader& r) {
    const auto n = r.length(1u << 20, 1);
    if (!n) return std::nullopt;
    std::vector<std::string> out;
    for (std::size_t i = 0; i < *n; ++i) {
        auto s = r.string(32767);
        if (!s) return std::nullopt;
        out.push_back(std::move(*s));
    }
    return out;
}

std::vector<std::byte> encode_players(const std::map<Uuid, SavedPlayer>& players) {
    protocol::Writer w;
    w.varint(8).varint(static_cast<std::int32_t>(players.size()));
    for (const auto& [uuid, p] : players) {
        w.uuid(uuid).f64(p.pos.x).f64(p.pos.y).f64(p.pos.z).f32(p.yaw).f32(p.pitch).u8(p.game_mode).varint(p.held).f32(p.health)
            .varint(p.food).f32(p.saturation).f32(p.exhaustion);
        write_strings(w, p.book.known);
        write_strings(w, p.book.highlighted);
        std::uint8_t bits = 0;
        for (std::size_t k = 0; k < 8; ++k) bits |= static_cast<std::uint8_t>(p.book.settings[k] ? 1u << k : 0u);
        w.u8(bits);
        write_strings(w, p.book.done);
        w.varint(static_cast<std::int32_t>(p.book.progress.size()));
        for (const auto& [id, criteria] : p.book.progress) {
            w.string(id);
            write_strings(w, criteria);
        }
        // Each effect: its id, then its levels (amplifier, duration, ambient,
        // particles and icon bits), the one shown first.
        w.varint(static_cast<std::int32_t>(p.effects.size()));
        for (const auto& e : p.effects) {
            w.varint(e.id).varint(static_cast<std::int32_t>(e.levels.size()));
            for (const auto& l : e.levels)
                w.varint(l.amplifier).varint(l.duration).u8(static_cast<std::uint8_t>((l.ambient ? 1 : 0) | (l.particles ? 2 : 0) | (l.icon ? 4 : 0)));
        }
        // The attributes the player has, by name, and the absorption; the
        // remaining fire ticks; the air supply and the frozen ticks.
        write_strings(w, p.attributes);
        w.f32(p.absorption);
        w.varint(p.fire_ticks);
        w.varint(p.air).varint(p.frozen_ticks);
    }
    return w.take();
}
std::optional<std::map<Uuid, SavedPlayer>> decode_players(std::span<const std::byte> bytes) {
    protocol::Reader r(bytes);
    const auto version = r.varint();
    const auto count = r.length(1u << 20, 30);
    // Version 2 had no health, version 3 no recipe book, version 4 no
    // effects, version 5 no attributes or absorption, version 6 no fire
    // ticks, version 7 no air supply or frozen ticks.
    if (!version || *version < 2 || *version > 8 || !count) return std::nullopt;
    std::map<Uuid, SavedPlayer> out;
    for (std::size_t i = 0; i < *count; ++i) {
        SavedPlayer p;
        const auto uuid = r.uuid();
        const auto x = r.f64(), y = r.f64(), z = r.f64();
        const auto yaw = r.f32(), pitch = r.f32();
        const auto mode = r.u8();
        const auto held = r.varint();
        if (!uuid || !x || !y || !z || !yaw || !pitch || !mode || !held) return std::nullopt;
        if (*version >= 3) {
            const auto health = r.f32();
            const auto food = r.varint();
            const auto saturation = r.f32(), exhaustion = r.f32();
            if (!health || !food || !saturation || !exhaustion) return std::nullopt;
            p.health = *health;
            p.food = *food;
            p.saturation = *saturation;
            p.exhaustion = *exhaustion;
        }
        p.pos = {*x, *y, *z};
        p.yaw = *yaw;
        p.pitch = *pitch;
        p.game_mode = *mode;
        p.held = *held;
        if (*version >= 4) {
            auto known = read_strings(r), highlighted = read_strings(r);
            const auto bits = r.u8();
            auto done = read_strings(r);
            const auto n = r.length(1u << 20, 2);
            if (!known || !highlighted || !bits || !done || !n) return std::nullopt;
            p.book.known = std::move(*known);
            p.book.highlighted = std::move(*highlighted);
            for (std::size_t k = 0; k < 8; ++k) p.book.settings[k] = (*bits >> k) & 1;
            p.book.done = std::move(*done);
            for (std::size_t k = 0; k < *n; ++k) {
                auto id = r.string(32767);
                auto criteria = read_strings(r);
                if (!id || !criteria) return std::nullopt;
                p.book.progress.emplace_back(std::move(*id), std::move(*criteria));
            }
        }
        if (*version >= 5) {
            const auto n = r.length(gameplay::effects::count, 2);
            if (!n) return std::nullopt;
            for (std::size_t k = 0; k < *n; ++k) {
                const auto id = r.varint();
                const auto levels = r.length(1u << 16, 3);
                if (!id || !levels) return std::nullopt;
                if (*id < 0 || *id > 255) return std::nullopt;
                gameplay::effects::Effect e{static_cast<std::uint8_t>(*id), {}};
                for (std::size_t m = 0; m < *levels; ++m) {
                    const auto amplifier = r.varint(), duration = r.varint();
                    const auto bits = r.u8();
                    if (!amplifier || !duration || !bits) return std::nullopt;
                    e.levels.push_back({*amplifier, *duration, (*bits & 1) != 0, (*bits & 2) != 0, (*bits & 4) != 0});
                }
                p.effects.push_back(std::move(e));
            }
        }
        if (*version >= 6) {
            auto attributes = read_strings(r);
            const auto absorption = r.f32();
            if (!attributes || !absorption) return std::nullopt;
            p.attributes = std::move(*attributes);
            p.absorption = *absorption;
        }
        if (*version >= 7) {
            const auto fire = r.varint();
            if (!fire) return std::nullopt;
            p.fire_ticks = *fire;
        }
        if (*version >= 8) {
            const auto air = r.varint(), frozen = r.varint();
            if (!air || !frozen) return std::nullopt;
            p.air = *air;
            p.frozen_ticks = *frozen;
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

ChunkPos chunk_at(Vec3 v) {
    return {static_cast<std::int32_t>(std::floor(v.x)) >> 4, static_cast<std::int32_t>(std::floor(v.z)) >> 4};
}
WirePosition wire_pos(BlockPos p) { return {p.x, p.y, p.z}; }
BlockPos block_pos(WirePosition p) { return {p.x, p.y, p.z}; }
Uuid entity_uuid(std::uint64_t id) { return Uuid{0x43697263756974ULL /* "Circuit" */, id}; }
std::int32_t chebyshev(ChunkPos a, ChunkPos b) { return std::max(std::abs(a.x - b.x), std::abs(a.z - b.z)); }

// Horizontal direction the player looks at: yaw 0 is south, 90 west, 180 north, 270 east.
// Player Abilities flags: invulnerable, flying, may fly, instant build.
std::uint8_t abilities(std::uint8_t game_mode) {
    if (game_mode == spectator) return 0x01 | 0x02 | 0x04;
    if (game_mode == creative) return 0x01 | 0x04 | 0x08;
    return 0;
}

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
    const ServerData& data;
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
    std::unique_ptr<asio::thread_pool> encode_pool; // chunk packet encoders (workers - 1 threads)

    std::mutex inbox_mutex;
    std::vector<Message> inbox;
    std::atomic<std::size_t> player_count{0};
    std::atomic<std::uint64_t> tick_count{0};

    std::map<std::uint64_t, Player> players;
    // The rules' recipes by id (rule_recipe), filled when first asked.
    std::map<std::string, std::pair<std::uint8_t, std::uint32_t>, std::less<>> rule_recipes;
    std::uint32_t next_ticket{1000};
    std::map<ChunkPos, std::uint32_t> forced; // the overworld's force-loaded chunks and their tickets
    // The world's game rules as set, including changes admitted this tick.
    std::map<std::string, std::int32_t> rules;
    // A day time admitted for the coming tick.
    std::optional<std::int64_t> pending_day_time;
    // The weather players last got.
    gameplay::Weather sent_weather;
    std::int32_t next_entity_id{1};
    std::uint64_t sound_seed{}; // the official servers draw a sound's seed at random
    std::uint64_t voice_draws{}; // and a voice's pitch (Circuit's own draws: splitmix64 of a counter)
    std::uint64_t spawn_draws{}; // and where a player appears around the spawn (the same)
    std::unordered_map<std::uint64_t, Tracked> tracked;
    std::int32_t dimension_type{};
    std::int32_t biome{};
    std::int32_t item_entity_type{}, falling_block_type{}, tnt_entity_type{}, arrow_entity_type{}, snowball_entity_type{}, minecart_entity_type{},
        orb_entity_type{},
        player_entity_type{};
    std::array<std::int32_t, gameplay::boat_types.size()> boat_entity_types{};
    std::int32_t chest_block{}, trapped_chest_block{}, ender_chest_block{};
    std::array<std::int32_t, gameplay::blocks::shulker_box_names.size()> shulker_blocks{};
    // Blocks /setblock set for the next tick: a later /setblock compares with
    // them, as commands run one after another on the official servers.
    std::map<BlockPos, BlockState> pending_setblocks;
    // Parameters of block events queued and not yet run, by event in queue
    // order (the trace of a run names none).
    std::map<std::tuple<BlockPos, std::uint64_t, std::uint64_t>, std::deque<std::uint8_t>> queued_events;
    std::unordered_map<std::uint16_t, std::int32_t> item_wire; // reference item -> wire item id
    Bytes recipes_bytes;                                        // update_recipes, once built
    bool started{false};

    // Tick control (freeze and step, pause when empty) and timing of recent ticks.
    bool frozen{false};
    std::uint64_t pending_steps{};
    std::uint64_t empty_ticks{};
    bool paused{false};
    std::array<double, 1200> durations{}; // milliseconds, ring buffer
    std::size_t durations_seen{};
    std::uint64_t last_tick_ns{}, total_tick_ns{};

    Impl(const ServerConfig& c, const contracts::VersionBundle& b, const Wire& w, const protocol::ContentTables& t,
         const evidence::ContentMapping& m, const ServerData& d)
        : config(c), bundle(b), wire(w), content(t), mapping(m), data(d), registries(d.registries), palette(t, m) {
        dimension_type = registry_index(registries, "minecraft:dimension_type", "minecraft:overworld").value();
        biome = registry_index(registries, "minecraft:worldgen/biome", "minecraft:plains").value();
        item_entity_type = content.entity_type("minecraft:item").value();
        falling_block_type = content.entity_type("minecraft:falling_block").value();
        tnt_entity_type = content.entity_type("minecraft:tnt").value();
        arrow_entity_type = content.entity_type("minecraft:arrow").value();
        snowball_entity_type = content.entity_type("minecraft:snowball").value();
        minecart_entity_type = content.entity_type("minecraft:minecart").value();
        orb_entity_type = content.entity_type("minecraft:experience_orb").value();
        for (std::size_t i = 0; i < gameplay::boat_types.size(); ++i) boat_entity_types[i] = content.entity_type(gameplay::boat_types[i]).value();
        player_entity_type = content.entity_type("minecraft:player").value();
        chest_block = content.block_id("minecraft:chest").value();
        trapped_chest_block = content.block_id("minecraft:trapped_chest").value();
        ender_chest_block = content.block_id("minecraft:ender_chest").value();
        for (std::size_t k = 0; k < shulker_blocks.size(); ++k)
            shulker_blocks[k] = content.block_id("minecraft:" + std::string(gameplay::blocks::shulker_box_names[k])).value();
        for (std::uint16_t i = 1; i <= gameplay::items::max_id; ++i)
            if (const auto id = content.item_id(mapping.item_name(i))) item_wire[i] = *id;
        open_world();
        sent_weather = engine->weather();
        if (config.workers > 1) encode_pool = std::make_unique<asio::thread_pool>(config.workers - 1);
        extensions = std::make_unique<Extensions>(*this, config, bundle);
        extensions->load();
    }

    // A new world's engine: its state starts with the configured game rules
    // (game-rule.<name>), which its first checkpoint keeps.
    std::unique_ptr<runtime::Engine> new_engine() {
        auto created = std::make_unique<runtime::Engine>(bundle, seed, engine_options(meta.floor_y));
        if (config.initial_game_rules.empty()) return created;
        auto state = created->dump();
        for (const auto& [name, value] : config.initial_game_rules) {
            const contracts::SetGameRule set{name, static_cast<std::int32_t>(value)};
            if (!gameplay::check_game_rule(bundle, set))
                throw std::runtime_error(std::format("game-rule.{}: no such game rule, or {} is out of its range", name, value));
            gameplay::set_game_rule(bundle, state.game_rules, set);
        }
        return std::make_unique<runtime::Engine>(bundle, seed, state, engine_options(meta.floor_y));
    }

    runtime::EngineOptions engine_options(std::int32_t floor_y) const {
        auto o = world_engine_options(bundle, floor_y, config.workers);
        o.data = data.game;
        return o;
    }

    // ---- persistence ------------------------------------------------------

    // Creates the engine: from the world directory's latest durable state
    // (a checkpoint plus the logged inputs after it), or a new world.
    void open_world() {
        meta = fresh_meta(config.floor_y);
        meta.data = data.fingerprint();
        seed = config.seed;
        if (config.world_directory.empty()) {
            engine = new_engine();
            rules = engine->dump().game_rules;
            log_warn("no world directory configured: the world is not saved");
            return;
        }
        const auto where = config.world_directory.string();
        fs = std::make_unique<storage::PosixFilesystem>();
        world = std::make_unique<storage::WorldDirectory>(*fs, config.world_directory, std::string(bundle.id),
                                                          std::string(bundle.rules->id));
        auto recovered = world->recover();
        if (!recovered) {
            // A world of an earlier rule set: --migrate-world carries it over.
            const auto identity = storage::WorldDirectory::identity(*fs, config.world_directory);
            const bool other_rules = identity && *identity && (*identity)->rules_id != bundle.rules->id;
            throw std::runtime_error(std::format("world {}: {}{}", where, recovered.error(),
                                                 other_rules ? std::format("; circuit-server --migrate-world <new directory> --world {} "
                                                                           "--game-version {} copies it to {}",
                                                                           where, bundle.id, bundle.rules->id)
                                                             : std::string{}));
        }
        if (!*recovered) {
            engine = new_engine();
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
            // The rules read the data packs: a log replays only under the
            // data it was written with. Without a log the data may change.
            if (meta.data != data.fingerprint()) {
                if (!r.inputs.empty())
                    throw std::runtime_error(std::format("world {}: its log was written with data {} and this server loads {}; start it "
                                                         "with the same data packs so that the log replays as written",
                                                         where, meta.data, data.fingerprint()));
                log_info(std::format("the world's data changes from {} to {}", meta.data, data.fingerprint()));
                meta.data = data.fingerprint();
            }
            seed = r.checkpoint.seed;
            if (seed != config.seed || meta.floor_y != config.floor_y)
                log_info(std::format("the world keeps its own seed {} and floor height {}", seed, meta.floor_y));
            // Stored components must be this protocol's, canonical and within
            // the registries it synchronizes, like those a client sends.
            if (const auto bad = invalid_components(r.checkpoint.state, r.inputs))
                throw std::runtime_error(std::format("world {}: invalid item components: {}", where, *bad));
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
        // Players are not connected after a restart: their chunk tickets go,
        // and so do the other tickets the target versions do not save. The
        // start ticket is added again on the first tick. A world written
        // before tickets had types keeps none.
        queue = contracts::CommandQueue(65536, meta.next_admission);
        const auto opened = engine->dump();
        rules = opened.game_rules;
        for (const auto& t : opened.tickets) {
            if ((t.type != contracts::TicketType::forced && t.type != contracts::TicketType::portal) || t.id == spawn_ticket
                || !meta.typed_tickets)
                admit(contracts::RemoveTicket{t.id});
            else if (t.type == contracts::TicketType::forced && t.dimension == overworld && t.radius == 0)
                forced[t.center] = t.id;
        }
        meta.typed_tickets = true;
        next_ticket = meta.next_ticket;
        // A checkpoint of the opened state starts a fresh log segment.
        if (auto ok = save(); !ok) throw std::runtime_error(std::format("world {}: cannot save: {}", where, ok.error()));
    }

    SavedPlayer snapshot(const Player& p) const {
        std::vector<std::string> attributes;
        for (const auto& i : p.attributes) attributes.emplace_back(gameplay::attributes::name(i.attribute));
        return {p.pos, p.yaw, p.pitch, p.game_mode, p.held, p.dead ? 0.0f : p.health, p.food, p.saturation, p.exhaustion, saved_book(p.book),
                p.dead ? std::vector<gameplay::effects::Effect>{} : p.effects, std::move(attributes), p.dead ? 0.0f : p.absorption,
                p.dead ? -20 : p.fire_ticks, p.dead ? gameplay::damage::max_air : p.air, p.dead ? 0 : p.frozen_ticks};
    }
    // A recipe book by ids, and back (ids the data packs no longer have are left out).
    SavedBook saved_book(const PlayerBook& b) const {
        SavedBook out;
        out.settings = b.settings;
        const auto* book = data.recipe_book.get();
        if (!book) return out;
        for (const auto r : b.known) out.known.push_back(book->recipes[r].id);
        for (const auto r : b.highlighted) out.highlighted.push_back(book->recipes[r].id);
        for (const auto a : b.done) out.done.push_back(book->advancements[a].id);
        for (const auto& [a, criteria] : b.progress) {
            std::vector<std::string> names;
            for (const auto c : criteria) names.push_back(book->advancements[a].criteria[c].name);
            if (!names.empty()) out.progress.emplace_back(book->advancements[a].id, std::move(names));
        }
        return out;
    }
    PlayerBook player_book(const SavedBook& s) const {
        PlayerBook out;
        out.settings = s.settings;
        const auto* book = data.recipe_book.get();
        if (!book) return out;
        const auto recipe = [&](const std::string& id) {
            const auto it = book->by_id.find(id);
            return it != book->by_id.end() && book->recipes[it->second].in_book ? std::optional(it->second) : std::nullopt;
        };
        for (const auto& id : s.known)
            if (const auto r = recipe(id)) out.known.insert(*r);
        for (const auto& id : s.highlighted)
            if (const auto r = recipe(id); r && out.known.contains(*r)) out.highlighted.insert(*r);
        for (const auto& id : s.done)
            if (const auto it = book->advancement_ids.find(id); it != book->advancement_ids.end()) out.done.insert(it->second);
        for (const auto& [id, names] : s.progress) {
            const auto it = book->advancement_ids.find(id);
            if (it == book->advancement_ids.end() || out.done.contains(it->second)) continue;
            const auto& criteria = book->advancements[it->second].criteria;
            for (const auto& n : names)
                for (std::size_t c = 0; c < criteria.size(); ++c)
                    if (criteria[c].name == n) out.progress[it->second].insert(c);
        }
        return out;
    }

    // ---- checkpoints --------------------------------------------------------
    //
    // A checkpoint starts on the game thread: a copy-on-write snapshot of the
    // engine and the start of a new log segment. A writer thread encodes and
    // writes it while the game goes on; the game thread releases the snapshot
    // when the writer is done. Shutdown and errors use the same steps
    // synchronously.

    struct CheckpointJob {
        storage::Checkpoint checkpoint; // state without its block lists
        std::vector<std::vector<storage::ColumnSections>> sections; // per level, per column
        storage::PendingCheckpoint pending;
        std::chrono::steady_clock::time_point began;
        double game_thread_ms{};
        std::vector<std::shared_ptr<const storage::SectionBlocks>> dropped; // from the section cache
    };
    std::mutex writer_mutex;
    std::condition_variable writer_wake;
    std::optional<CheckpointJob> to_write, written;
    std::string writer_error;
    bool writer_stop{false}, writing{false};
    storage::SectionCache section_cache; // writer thread, or the game thread while no job runs
    std::thread writer;

    // Snapshot and begin, on the game thread.
    std::expected<CheckpointJob, std::string> begin_save() {
        const auto started_at = std::chrono::steady_clock::now();
        for (const auto& [id, p] : players) saved[p.uuid] = snapshot(p);
        meta.next_admission = queue.next_admission();
        meta.next_ticket = next_ticket;
        auto snap = engine->snapshot();
        CheckpointJob job{{std::string(bundle.id), std::string(bundle.rules->id), seed, std::move(snap.state), {}},
                          std::move(snap.sections), {}, started_at, 0, {}};
        job.checkpoint.extras[std::string(meta_extra)] = encode_meta(meta);
        job.checkpoint.extras[std::string(players_extra)] = encode_players(saved);
        auto pending = world->begin_checkpoint(job.checkpoint.state.time);
        if (!pending) return std::unexpected(pending.error());
        job.pending = std::move(*pending);
        job.game_thread_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_at).count();
        return job;
    }
    storage::Status finish_save(CheckpointJob& job) {
        const auto bytes = storage::encode_checkpoint(job.checkpoint, job.sections, bundle, &section_cache);
        job.dropped = section_cache.take_dropped(); // released with the job, on the game thread
        return world->finish_checkpoint(job.pending, bytes);
    }
    void log_saved(const CheckpointJob& job) {
        const auto total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - job.began).count();
        log_info(std::format("saved the world at tick {}: {:.1f} ms on the game thread, {:.1f} ms in all", job.checkpoint.state.time,
                             job.game_thread_ms, total));
    }

    void writer_loop() {
        std::unique_lock lock(writer_mutex);
        for (;;) {
            writer_wake.wait(lock, [&] { return writer_stop || to_write.has_value(); });
            if (!to_write) return;
            auto job = std::move(*to_write);
            to_write.reset();
            lock.unlock();
            const auto ok = finish_save(job);
            lock.lock();
            if (!ok && writer_error.empty()) writer_error = ok.error();
            written = std::move(job);
            writer_wake.notify_all();
        }
    }

    // Game thread: releases a finished background checkpoint. Throws when it failed.
    void collect_save(bool wait) {
        std::optional<CheckpointJob> done;
        std::string error;
        {
            std::unique_lock lock(writer_mutex);
            if (wait) writer_wake.wait(lock, [&] { return !writing || written.has_value(); });
            if (!written) return;
            done = std::move(written);
            written.reset();
            error = std::move(writer_error);
            writer_error.clear();
            writing = false;
        }
        if (!error.empty()) {
            log_error(std::format("cannot save the world: {}", error));
            throw std::runtime_error("the world could not be saved; stopping");
        }
        log_saved(*done);
        // The snapshot's sections are released here, between ticks.
    }

    void start_background_save() {
        if (writing) {
            log_warn("the previous checkpoint is still being written; this autosave is skipped");
            return;
        }
        auto job = begin_save();
        if (!job) {
            log_error(std::format("cannot save the world: {}", job.error()));
            throw std::runtime_error("the world could not be saved; stopping");
        }
        if (!writer.joinable()) writer = std::thread([this] { writer_loop(); });
        std::lock_guard lock(writer_mutex);
        to_write = std::move(*job);
        writing = true;
        writer_wake.notify_all();
    }

    // Writes a checkpoint of the current state now, after any background one.
    storage::Status save() {
        if (!world) return {};
        try {
            collect_save(true);
        } catch (const std::runtime_error&) {
            // Reported; try a complete checkpoint below.
        }
        auto job = begin_save();
        if (!job) return std::unexpected(job.error());
        auto ok = finish_save(*job);
        if (ok) log_saved(*job);
        return ok;
    }

    void stop_writer() {
        {
            std::lock_guard lock(writer_mutex);
            writer_stop = true;
            writer_wake.notify_all();
        }
        if (writer.joinable()) writer.join();
    }

    BlockState at(BlockPos p) const {
        const auto v = engine->view().block(overworld, p);
        return v ? *v : blocks::air();
    }
    NeighborLookup lookup() const {
        return [this](BlockPos p) { return at(p); };
    }

    void admit(contracts::CommandBody body, contracts::DimensionId level = overworld) {
        if (!queue.admit(std::move(body), level)) log_warn("game: command queue full; input dropped");
    }

    void broadcast(const Bytes& packet, std::uint64_t except = 0) {
        for (auto& [id, p] : players)
            if (id != except) p.peer->send(packet);
    }
    void message(Player& p, std::string text, std::string_view color = "gray") {
        p.peer->send(wire.system_chat(nbt::colored_text(std::move(text), color)));
    }

    // A game mode change, as both official servers tell it (client/spectators):
    // the abilities, the game mode event, the player list's game mode for
    // everyone, the abilities again. Spectators fly permanently; creative
    // players may fly and build instantly. A spectator leaves the view of
    // those who are not at once (on both official servers the invisibility
    // came first in two of eight runs: when their entity tracker ran before
    // the spectator's next move).
    void set_game_mode(Player& p, std::uint8_t mode) {
        const bool was_spectator = p.game_mode == spectator;
        p.game_mode = mode;
        p.digging.reset();
        const auto abilities_packet = wire.player_abilities(abilities(p.game_mode), 0.05f, 0.1f);
        p.peer->send(abilities_packet);
        p.peer->send(wire.game_event(3, static_cast<float>(p.game_mode)));
        broadcast(wire.player_info_game_mode(p.uuid, p.game_mode));
        p.peer->send(abilities_packet);
        p.flying = p.game_mode == spectator; // as the abilities just sent say (a creative flyer stops flying)
        if (mode != spectator && p.camera) set_camera(p, std::nullopt);
        if (mode == spectator && !was_spectator) {
            const auto gone = wire.remove_entities({p.entity_id});
            for (auto it = p.seen_by.begin(); it != p.seen_by.end();) {
                const auto o = players.find(*it);
                if (o != players.end() && o->second.game_mode != spectator) {
                    o->second.peer->send(gone);
                    it = p.seen_by.erase(it);
                } else {
                    ++it;
                }
            }
        }
        game_mode_attributes(p);
    }
    // A spectator's camera: an entity it follows (its view and position), or
    // its own (none); the client is told and teleported to where it now looks
    // from (client/spectators).
    void set_camera(Player& p, std::optional<std::int32_t> entity) {
        p.camera = entity;
        p.peer->send(wire.set_camera(entity.value_or(p.entity_id)));
        if (const auto at = entity ? entity_position(*entity) : std::optional(p.pos)) {
            place_spectator(p, *at);
            teleport(p, *at);
        }
    }
    // A spectator moved by its camera or a teleport: its chunks and tickets
    // follow it, as a move's do.
    void place_spectator(Player& p, Vec3 at) {
        p.pos = at;
        p.moved = true;
        if (const auto chunk = chunk_at(at); chunk.x != p.center.x || chunk.z != p.center.z) {
            p.center = chunk;
            p.peer->send(wire.chunk_center(chunk.x, chunk.z));
            move_tickets(p, chunk);
        }
    }
    // Where an entity Circuit shows is: a living player or a tracked entity
    // (none if gone).
    std::optional<Vec3> entity_position(std::int32_t entity) const {
        for (const auto& [id, o] : players)
            if (o.entity_id == entity) return o.dead ? std::nullopt : std::optional(o.pos);
        for (const auto& [id, t] : tracked)
            if (t.protocol_id == entity) return t.last;
        return std::nullopt;
    }
    // A spectator follows its camera's entity each tick, and goes back to its
    // own when the shift key is down or the entity is gone.
    void follow_camera(Player& p) {
        if (!p.camera || p.dead) return;
        const auto at = entity_position(*p.camera);
        if (!at || p.sneaking || p.game_mode != spectator) {
            set_camera(p, std::nullopt);
            return;
        }
        if (p.pos.x != at->x || p.pos.y != at->y || p.pos.z != at->z) place_spectator(p, *at);
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
        p.skin_parts = r.skin_parts;
        p.main_arm = r.main_hand;
        p.fresh = true;
        p.pos = spawn_point();
        p.arrival = Player::Arrival::first;
        if (const auto it = saved.find(p.uuid); it != saved.end()) {
            restore(p, it->second);
            p.arrival = Player::Arrival::again;
        }
        // Restored effects go to the player at the end of the joining tick;
        // the entity data they decide follows a tick later (client/effects).
        p.effects_dirty = !p.effects.empty();
        // A returning player's absorption is told before the attributes
        // (send_joining_vitals), and is part of the entity's data from then on.
        p.effect_view.absorption = p.absorption;
        p.view_distance = std::clamp(r.view_distance, 2, config.view_distance);
        p.center = chunk_at(p.pos);
        p.ticket = next_ticket;
        next_ticket += 2; // loading and simulation
        add_tickets(p, p.center);

        Wire::JoinGame j{p.entity_id, config.max_players, p.view_distance, config.simulation_distance, dimension_type,
                         p.game_mode};
        auto& peer = *p.peer;
        peer.send(wire.join_game(j));
        peer.send(wire.player_abilities(abilities(p.game_mode), 0.05f, 0.1f));
        peer.send(wire.set_held_slot(p.held));
        peer.send(recipes_packet());
        // The operator level (entity event 28: every player may run every
        // command), then the commands it allows, as both official servers
        // send them (client/player_data).
        peer.send(wire.entity_event(p.entity_id, 28));
        peer.send(wire.empty_commands());
        // The recipe book: its settings, then every recipe the player knows.
        if (data.recipe_book) {
            peer.send(wire.recipe_book_settings(p.book.settings));
            std::vector<Wire::BookEntry> entries;
            for (const auto r : p.book.known)
                entries.push_back({static_cast<std::int32_t>(r), &data.recipe_book->recipes[r], static_cast<std::uint8_t>(p.book.highlighted.contains(r) ? 2 : 0)});
            peer.send(wire.recipe_book_add(entries, true, wire_item()));
            tell_known(p, p.book.known, true, true);
        }
        peer.send(wire.spawn_position(wire_pos({meta.spawn.x, meta.spawn.y + 1, meta.spawn.z}), 0.0f));
        peer.send(time_packet());
        // Rain, as both official servers report it on joining.
        if (const auto& w = engine->weather(); w.rain_visible()) {
            peer.send(wire.game_event(1, 0.0f));
            peer.send(wire.game_event(7, w.rain_level));
            peer.send(wire.game_event(8, w.thunder_level));
        }
        // Everyone sees the newcomer; the newcomer sees everyone.
        std::vector<PlayerListEntry> all;
        for (const auto& [id, other] : players) all.push_back(entry(other));
        all.push_back(entry(p));
        peer.send(wire.player_info_add(all));
        broadcast(wire.player_info_add({entry(p)}));
        sync_inventory(p);
        peer.send(wire.game_event(13, 0.0f)); // start waiting for level chunks
        peer.send(wire.chunk_center(p.center.x, p.center.z));
        teleport(p, p.pos);
        // Health, then experience, follow at the end of the tick, after the
        // inventory, the position and the tick's recipe book triggers
        // (send_joining_vitals).
        p.vitals_pending = true;
        log_info(std::format("{} joined ({}, {})", p.name, p.uuid.text(), p.verified ? "verified" : "offline"));
        players.emplace(peer_id, std::move(p));
        player_count = players.size();
        auto& joined = players.at(peer_id);
        // Everyone but the newcomer is told (client/join_message, both versions).
        for (const auto& [id, other] : players)
            if (id != peer_id) message(players.at(id), std::format("{} joined the game", joined.name), "yellow");
    }

    // The recipe data clients need (built once): the item property sets of
    // the cooking and smithing recipes, and the stonecutter's recipes in the
    // order its buttons number them.
    const Bytes& recipes_packet() {
        if (recipes_bytes.empty()) {
            const auto ids = [&](std::span<const std::uint16_t> items) {
                std::vector<std::int32_t> out;
                for (const auto i : items)
                    if (const auto it = item_wire.find(i); it != item_wire.end()) out.push_back(it->second);
                return out;
            };
            std::vector<Wire::PropertySet> sets;
            std::vector<Wire::StonecutterEntry> cutter;
            using Kind = gameplay::recipes::Cooking::Kind;
            const auto* game = data.game.get();
            for (const auto& [name, kind] : {std::pair{"minecraft:furnace_input", Kind::smelting}, std::pair{"minecraft:blast_furnace_input", Kind::blasting},
                                             std::pair{"minecraft:smoker_input", Kind::smoking}, std::pair{"minecraft:campfire_input", Kind::campfire}}) {
                std::vector<std::uint16_t> items;
                if (game)
                    for (const auto& c : game->cooking)
                        if (c.kind == kind) items.insert(items.end(), c.ingredient.items.begin(), c.ingredient.items.end());
                std::ranges::sort(items);
                items.erase(std::ranges::unique(items).begin(), items.end());
                sets.push_back({name, ids(items)});
            }
            const std::vector<std::uint16_t> none;
            sets.push_back({"minecraft:smithing_template", ids(game ? game->smithing_template : none)});
            sets.push_back({"minecraft:smithing_base", ids(game ? game->smithing_base : none)});
            sets.push_back({"minecraft:smithing_addition", ids(game ? game->smithing_addition : none)});
            if (game)
                for (const auto& r : game->stonecutting) cutter.push_back({ids(r.ingredient.items), wire_stack(r.result)});
            recipes_bytes = wire.update_recipes(sets, cutter);
        }
        return recipes_bytes;
    }

    // Applies saved data; values the movement rules would refuse are ignored.
    void restore(Player& p, const SavedPlayer& s) {
        if (finite(s.pos) && std::abs(s.pos.x) < 3.0e7 && std::abs(s.pos.z) < 3.0e7 && std::abs(s.pos.y) <= 4096.0) p.pos = s.pos;
        if (std::isfinite(s.yaw) && std::isfinite(s.pitch)) {
            p.yaw = s.yaw;
            p.pitch = std::clamp(s.pitch, -90.0f, 90.0f);
        }
        if (s.game_mode == survival || s.game_mode == creative || s.game_mode == spectator) p.game_mode = s.game_mode;
        if (s.held >= 0 && s.held < 9) p.held = s.held;
        // A player who left dead joins respawned around the world spawn.
        if (std::isfinite(s.health) && s.health > 0.0f) {
            p.health = std::min(s.health, 20.0f); // up to a boosted maximum below, once the effects are back
            if (s.food >= 0 && s.food <= 20) p.food = s.food;
            if (std::isfinite(s.saturation)) p.saturation = std::clamp(s.saturation, 0.0f, static_cast<float>(p.food));
            if (std::isfinite(s.exhaustion)) p.exhaustion = std::clamp(s.exhaustion, 0.0f, 40.0f);
            // Still burning when it left (a short's range, as the target
            // versions keep it).
            p.fire_ticks = std::clamp(s.fire_ticks, -20, 32767);
            p.burning = p.fire_ticks > 0;
            // Their breath and frost (minecraft.wiki "Entity format": Air,
            // TicksFrozen).
            p.air = std::clamp(s.air, -19, gameplay::damage::max_air);
            p.frozen_ticks = std::clamp(s.frozen_ticks, 0, gameplay::damage::frozen_max);
        } else {
            p.pos = spawn_point();
        }
        p.book = player_book(s.book);
        // Effects a player who left dead had are gone; others are kept if
        // they are effects of the registry, in order, with valid levels: the
        // shown one with time left, those underneath with time left or run
        // out (a level underneath that runs out stays until the one above it
        // ends, client/effect_edges).
        if (std::isfinite(s.health) && s.health > 0.0f) {
            const auto valid = [](const gameplay::effects::Effect& e) {
                if (e.id >= gameplay::effects::count || e.levels.empty()) return false;
                if (e.levels.front().duration == 0) return false;
                return std::ranges::all_of(e.levels, [](const auto& l) {
                    return l.amplifier >= 0 && l.amplifier <= 255 && (l.duration >= 0 || l.duration == gameplay::effects::infinite);
                });
            };
            for (const auto& e : s.effects)
                if (valid(e) && (p.effects.empty() || p.effects.back().id < e.id)) p.effects.push_back(e);
        }
        // The attributes the player had, with the modifiers of their
        // effects; the game mode's follow at the end of the joining tick.
        // Every player who joined before has those of a first join (also one
        // saved before version 6, or who left in the tick they first joined).
        for (const auto a : {gameplay::attributes::Attribute::block_interaction_range, gameplay::attributes::Attribute::entity_interaction_range,
                             gameplay::attributes::Attribute::movement_speed})
            attribute(p, a);
        for (const auto& name : s.attributes)
            if (const auto a = gameplay::attributes::by_name(name)) attribute(p, *a);
        for (const auto& e : p.effects) effect_attributes(p, e.id);
        p.attributes_changed.clear();
        if (std::isfinite(s.health) && s.health > 0.0f) {
            p.health = std::min(s.health, max_health(p));
            if (std::isfinite(s.absorption))
                p.absorption = std::clamp(s.absorption, 0.0f, static_cast<float>(attribute_value(p, gameplay::attributes::Attribute::max_absorption)));
        }
    }
    // Where a new or respawning player appears: at the world spawn's
    // center, or with the spawnRadius game rule in a random column of the
    // square that many blocks around it, on the column's highest block that
    // stops movement when its top is full and it holds no water
    // (minecraft.wiki "World spawn"; client/player_data: within 10 blocks on
    // both official servers, which draw it at random; Circuit draws its own;
    // after 16 columns that do not do, the spawn itself).
    Vec3 spawn_point() {
        const Vec3 center{meta.spawn.x + 0.5, static_cast<double>(meta.spawn.y) + 1.0, meta.spawn.z + 0.5};
        const auto radius = std::clamp(gameplay::game_rule_value(bundle, rules, "spawnRadius"), 0, 64);
        if (radius == 0) return center;
        const auto draw = [this](std::int32_t n) {
            auto z = (++spawn_draws) * 0x9E3779B97F4A7C15ull;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return static_cast<std::int32_t>((z ^ (z >> 31)) % static_cast<std::uint64_t>(n));
        };
        const auto top = bundle.world.min_y + bundle.world.height;
        for (int attempt = 0; attempt < 16; ++attempt) {
            const auto x = meta.spawn.x + draw(2 * radius + 1) - radius, z = meta.spawn.z + draw(2 * radius + 1) - radius;
            for (std::int32_t y = top - 1; y >= bundle.world.min_y; --y) {
                const auto v = engine->view().block(overworld, {x, y, z});
                if (!v) break; // not loaded
                if (v->is_air()) continue;
                if (v->is(BlockType::water) || gameplay::is_lava(*v)) break; // a liquid covers the column
                const auto boxes = gameplay::physics::collision_boxes(*v, bundle.schema.content_release);
                if (boxes.empty()) continue;
                const bool full_top = std::ranges::any_of(boxes, [](const auto& b) {
                    return b.min_x <= 0.0 && b.max_x >= 1.0 && b.min_z <= 0.0 && b.max_z >= 1.0 && b.max_y >= 1.0;
                });
                if (full_top && !gameplay::waterlogged_block(*v)) return {x + 0.5, y + 1.0, z + 0.5};
                break;
            }
        }
        return center;
    }

    PlayerListEntry entry(const Player& p) const {
        return {p.uuid, p.name, p.properties, p.game_mode, true, 0};
    }

    void on_leave(std::uint64_t peer_id) {
        const auto it = players.find(peer_id);
        if (it == players.end()) return;
        auto& p = it->second;
        // The crafting grids and the carried stack drop where the player
        // was, as the official servers drop a departed player's menus.
        if (!p.dead) {
            const auto [position, velocity] = throw_from(p);
            std::optional<BlockPos> container;
            if (p.window) container = p.window->pos;
            admit(contracts::CloseContainer{key_of(p), position, velocity, true, container});
        }
        remove_tickets(p);
        admit(contracts::PlayerPresence{key_of(p), {}, false, false}, overworld);
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

    // A player's tickets: the view distance loads (level 31 over the square),
    // and the simulation distance simulates (level 31 - distance at the
    // player's chunk), as in the target versions.
    void add_tickets(const Player& p, ChunkPos center) {
        admit(contracts::AddTicket{p.ticket, center, static_cast<std::uint8_t>(p.view_distance), contracts::TicketType::player_loading,
                                   31, 0},
              overworld);
        admit(contracts::AddTicket{p.ticket + 1, center, 0, contracts::TicketType::player_simulation,
                                   static_cast<std::uint8_t>(31 - config.simulation_distance), 0},
              overworld);
    }
    void remove_tickets(const Player& p) {
        admit(contracts::RemoveTicket{p.ticket});
        admit(contracts::RemoveTicket{p.ticket + 1});
    }
    void move_tickets(const Player& p, ChunkPos center) {
        remove_tickets(p);
        add_tickets(p, center);
    }
    // Where each player is, for the rules that depend on players (random
    // ticks and fire near players, experience orbs drawn to them, the maps
    // they hold): sent when the position, the spectator flag or the hotbar
    // slot in the main hand changed.
    void send_presences() {
        for (auto& [id, p] : players) {
            const auto at = p.pos;
            const auto now = std::tuple{at, p.game_mode == spectator, p.held};
            if (p.presence && *p.presence == now) continue;
            p.presence = now;
            admit(contracts::PlayerPresence{key_of(p), at, std::get<1>(now), true, static_cast<std::uint8_t>(p.held)}, overworld);
        }
    }

    // A teleport stops the player: a hurt's velocity is 0 and blocks are not
    // touched until the player's tick (client/drown); a correction of a
    // refused move (`stops` false) puts the player back where they were.
    void teleport(Player& p, Vec3 to, bool stops = true) {
        p.pos = to;
        if (stops) p.stopped = p.teleported = true;
        // A dead player's body moves with them for those who see it (a
        // living player's client moves after confirming).
        if (p.dead) p.moved = true;
        const auto id = p.next_teleport++;
        p.awaiting_teleport = id;
        p.peer->send(wire.player_position(id, to, p.yaw, p.pitch));
    }

    static contracts::PlayerKey key_of(const Player& p) { return {p.uuid.high, p.uuid.low}; }

    // The first problem with the components of a recovered state and its log.
    std::optional<std::string> invalid_components(const gameplay::WorldDump& d, const std::vector<contracts::TickInput>& inputs) const {
        std::optional<std::string> bad;
        const auto check = [&](const contracts::ComponentPatch& patch) {
            if (bad || patch.empty()) return;
            const auto checked = wire.components().check(patch, wire.component_context());
            if (!checked) bad = checked.error();
            else if (!(checked->typed() == patch.typed())) bad = "typed fields that differ from the components";
        };
        for (const auto& l : d.levels) {
            for (const auto& [pos, be] : l.block_entities) {
                check(be.components);
                for (const auto& s : be.slots) check(s.components);
            }
            for (const auto& e : l.entities) check(e.stack.components);
        }
        for (const auto& p : d.players) {
            check(p.cursor.components);
            for (const auto& s : p.slots) check(s.components);
        }
        for (const auto& input : inputs)
            for (const auto& c : input.commands)
                std::visit([&](const auto& body) {
                    if constexpr (requires { body.stack; }) check(body.stack.components);
                }, c.body);
        return bad;
    }

    WireStack wire_stack(const contracts::ItemStack& s) const {
        if (s.empty()) return {};
        const auto it = item_wire.find(s.item);
        return it == item_wire.end() ? WireStack{} : WireStack{it->second, s.count, s.damage, s.components};
    }

    // The player's open window as the simulation holds it: the own inventory
    // (window 0), or a container's slots (a crafting table's result and grid)
    // followed by the main inventory and the hotbar.
    // Sends the window's slots that differ from the client's view, one by
    // one, or the whole window when the client describes another one. Each
    // window counts its own state ids.
    void sync_inventory(Player& p) {
        const auto view = engine->view();
        const auto inv = view.player(key_of(p));
        const auto player_slot = [&](std::size_t i) {
            return inv && *inv ? wire_stack((*inv)->slots[i]) : WireStack{};
        };
        const auto carried = inv && *inv ? wire_stack((*inv)->cursor) : WireStack{};
        std::vector<WireStack> slots;
        if (!p.window) {
            for (std::size_t i = 0; i < inventory_slots; ++i) slots.push_back(player_slot(i));
        } else {
            if (gameplay::is_crafting_table(at(p.window->pos)))
                for (std::size_t i = 0; i < contracts::table_slots; ++i)
                    slots.push_back(inv && *inv ? wire_stack((*inv)->table[i]) : WireStack{});
            if (gameplay::is_stonecutter(at(p.window->pos)))
                for (std::size_t i = 0; i < contracts::cutter_slots; ++i)
                    slots.push_back(inv && *inv ? wire_stack((*inv)->cutter[i]) : WireStack{});
            if (gameplay::is_smithing_table(at(p.window->pos)))
                for (std::size_t i = 0; i < contracts::smith_slots; ++i)
                    slots.push_back(inv && *inv ? wire_stack((*inv)->smith[i]) : WireStack{});
            if (gameplay::is_grindstone(at(p.window->pos)))
                for (std::size_t i = 0; i < contracts::grind_slots; ++i)
                    slots.push_back(inv && *inv ? wire_stack((*inv)->grind[i]) : WireStack{});
            if (gameplay::is_loom(at(p.window->pos)))
                for (std::size_t i = 0; i < contracts::loom_slots; ++i)
                    slots.push_back(inv && *inv ? wire_stack((*inv)->loom[i]) : WireStack{});
            if (gameplay::is_anvil(at(p.window->pos)))
                for (std::size_t i = 0; i < contracts::anvil_slots; ++i)
                    slots.push_back(inv && *inv ? wire_stack((*inv)->anvil[i]) : WireStack{});
            if (gameplay::is_enchanting_table(at(p.window->pos)))
                for (std::size_t i = 0; i < contracts::enchant_slots; ++i)
                    slots.push_back(inv && *inv ? wire_stack((*inv)->enchant[i]) : WireStack{});
            if (gameplay::is_cartography_table(at(p.window->pos)))
                for (std::size_t i = 0; i < contracts::cartography_slots; ++i)
                    slots.push_back(inv && *inv ? wire_stack((*inv)->cartography[i]) : WireStack{});
            if (at(p.window->pos).is(BlockType::beacon))
                for (std::size_t i = 0; i < contracts::beacon_slots; ++i)
                    slots.push_back(inv && *inv ? wire_stack((*inv)->beacon[i]) : WireStack{});
            // An ender chest's window shows the player's own ender inventory.
            if (at(p.window->pos).is(BlockType::ender_chest))
                for (std::size_t i = 0; i < contracts::ender_slots; ++i)
                    slots.push_back(inv && *inv ? wire_stack((*inv)->ender[i]) : WireStack{});
            for (const auto part : container_parts_of(p.window->pos))
                if (const auto be = view.block_entity(overworld, part); be && *be)
                    for (const auto& s : (*be)->slots) slots.push_back(wire_stack(s));
            // A lectern's window shows its book alone.
            if (at(p.window->pos).is(BlockType::lectern)) {
                const auto be = view.block_entity(overworld, p.window->pos);
                slots.push_back(be && *be && !(*be)->slots.empty() ? wire_stack((*be)->slots[0]) : WireStack{});
            } else {
                for (std::size_t i = 9; i < 45; ++i) slots.push_back(player_slot(i));
            }
            // A crafter's window ends with what its grid would make.
            if (at(p.window->pos).is(BlockType::crafter)) slots.push_back(wire_stack(crafter_result(p.window->pos)));
        }
        const auto id = p.window ? p.window->id : 0;
        // Each change of a crafting grid's slot by a click (the own window's,
        // or an open crafting table's) sends the window's result again,
        // changed or not, as both official servers do (client/special_a,
        // client/own_grid_counts); other changes (closing, the recipe book)
        // send it only when it differs.
        std::size_t resends = 0;
        if (!p.clicks.empty()) {
            const auto key = key_of(p);
            const bool table = p.window && gameplay::is_crafting_table(at(p.window->pos));
            const std::uint64_t first = table ? contracts::table_first + 1 : 1, last = table ? contracts::table_first + 9 : 4;
            bool in_click = false;
            for (const auto& r : engine->trace()) {
                if (r.kind == contracts::TraceKind::command_applied || r.kind == contracts::TraceKind::command_rejected) {
                    const auto it = std::ranges::find(p.clicks, r.a);
                    in_click = it != p.clicks.end() && r.kind == contracts::TraceKind::command_applied;
                    if (it != p.clicks.end()) p.clicks.erase(it);
                    continue;
                }
                if (!in_click || (p.window && !table) || r.kind != contracts::TraceKind::player_slot_changed || r.a != key.hi || r.b != key.lo) continue;
                if (const auto slot = r.c >> 48; slot >= first && slot <= last) ++resends;
            }
        }
        // As both official servers do (client/window_sync): a use that changed
        // the stack it took from sends the whole own window, while another
        // window is open too (before that window's changes); a drop's change
        // of its slot is the client's view, which the client made itself.
        bool whole_own = false;
        if (!p.hand_actions.empty()) {
            const auto key = key_of(p);
            std::optional<Player::HandAction> current;
            for (const auto& r : engine->trace()) {
                if (r.kind == contracts::TraceKind::command_applied || r.kind == contracts::TraceKind::command_rejected) {
                    current.reset();
                    const auto it = std::ranges::find(p.hand_actions, r.a, &Player::HandAction::admission);
                    if (it == p.hand_actions.end()) continue;
                    if (r.kind == contracts::TraceKind::command_applied) current = *it;
                    p.hand_actions.erase(it);
                    continue;
                }
                if (!current || r.kind != contracts::TraceKind::player_slot_changed || r.a != key.hi || r.b != key.lo || r.c >> 48 != current->slot)
                    continue;
                if (!current->drop) {
                    whole_own = true;
                    continue;
                }
                // The client shows the slot as the drop left it (the stack the
                // server expected, or the slot now if a click before it in
                // the tick changed it); a later change of the tick is sent.
                const auto index = !p.window ? std::size_t{current->slot}
                                 : current->slot >= 9 && current->slot < 45 ? p.window->container_slots + current->slot - 9
                                                                            : slots.size();
                const auto left = r.c & ((std::uint64_t{1} << 48) - 1);
                const auto now = inv && *inv ? (*inv)->slots[current->slot] : contracts::ItemStack{};
                if (p.remote_window != id || index >= p.remote.size()) continue;
                if (contracts::pack_stack(current->left) == left) p.remote[index] = wire_stack(current->left);
                else if (contracts::pack_stack(now) == left) p.remote[index] = wire_stack(now);
            }
        }
        if (whole_own && !p.window) p.remote_window = -1;
        if (whole_own && p.window) {
            std::vector<WireStack> own;
            for (std::size_t i = 0; i < inventory_slots; ++i) own.push_back(player_slot(i));
            p.peer->send(wire.container_content(0, ++p.inventory_state, own, WireStack{}));
            p.own.assign(own.begin(), own.end());
        }
        auto& state = p.window ? p.window->state : p.inventory_state;
        // An open window's player slots are the client's own window's too.
        const auto mirror = [&] {
            if (p.window && p.own.size() == inventory_slots && p.remote.size() >= p.window->container_slots + 36)
                for (std::size_t k = 0; k < 36; ++k) p.own[9 + k] = p.remote[p.window->container_slots + k];
        };
        if (p.remote_window != id || p.remote.size() != slots.size()) {
            p.peer->send(wire.container_content(id, ++state, slots, carried));
            p.remote_window = id;
            p.remote.assign(slots.begin(), slots.end());
            p.remote_carried = carried;
            mirror();
            return;
        }
        // A hurt's armor wear and an attack's weapon wear go to the player at
        // the next tick, after the hurt's velocity (client/fire, client/combat).
        if (!p.window && p.wear_tick == tick_count)
            for (std::size_t i = 0; i < slots.size() && i < 64; ++i)
                if ((p.wear_slots >> i & 1) != 0 && p.remote[i]) slots[i] = *p.remote[i];
        for (; resends > 0; --resends) {
            p.peer->send(wire.container_slot(id, ++state, 0, slots[0]));
            p.remote[0] = slots[0];
        }
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (p.remote[i] != slots[i]) {
                p.peer->send(wire.container_slot(id, ++state, static_cast<std::int16_t>(i), slots[i]));
                p.remote[i] = slots[i];
            }
        if (p.remote_carried != carried) {
            p.peer->send(wire.cursor_item(carried));
            p.remote_carried = carried;
        }
        mirror();
        // A crafter window's properties: each slot disabled (1) or not, and
        // whether it is triggered.
        if (p.window && at(p.window->pos).is(BlockType::crafter))
            if (const auto be = view.block_entity(overworld, p.window->pos); be && *be)
                for (std::size_t k = 0; k < 10; ++k) {
                    const std::int32_t now = k < 9 ? ((*be)->disabled >> k & 1) : at(p.window->pos).flag() ? 1 : 0;
                    if (p.window->data[k] != now) {
                        p.peer->send(wire.container_data(p.window->id, static_cast<std::int16_t>(k), static_cast<std::int16_t>(now)));
                        p.window->data[k] = now;
                    }
                }
        // A stonecutter's or loom's window property: the chosen recipe or
        // pattern (-1: none).
        if (p.window && (gameplay::is_stonecutter(at(p.window->pos)) || gameplay::is_loom(at(p.window->pos)))) {
            const std::int32_t recipe = !inv || !*inv ? -1 : gameplay::is_loom(at(p.window->pos)) ? (*inv)->loom_pattern : (*inv)->cutter_recipe;
            if (p.window->data[0] != recipe) {
                p.peer->send(wire.container_data(p.window->id, 0, static_cast<std::int16_t>(recipe)));
                p.window->data[0] = recipe;
            }
        }
        // An anvil window's property: the level cost of its result.
        if (p.window && gameplay::is_anvil(at(p.window->pos))) {
            const std::int32_t cost = inv && *inv ? (*inv)->anvil_cost : 0;
            if (p.window->data[0] != cost) {
                p.peer->send(wire.container_data(p.window->id, 0, static_cast<std::int16_t>(std::clamp(cost, -32768, 32767))));
                p.window->data[0] = cost;
            }
        }
        // An enchanting table window's properties: the three offers' level
        // costs, the player's enchantment seed, the enchantments the offers
        // show and their levels (as shorts: the seed's low 16 bits).
        if (p.window && gameplay::is_enchanting_table(at(p.window->pos))) {
            std::array<std::int32_t, 10> now{0, 0, 0, 0, -1, -1, -1, -1, -1, -1};
            if (inv && *inv) {
                const auto& i = **inv;
                now = {i.enchant_costs[0], i.enchant_costs[1], i.enchant_costs[2], gameplay::experience::enchantment_seed(i),
                       i.enchant_clues[0], i.enchant_clues[1], i.enchant_clues[2], i.enchant_clue_levels[0], i.enchant_clue_levels[1],
                       i.enchant_clue_levels[2]};
            } else {
                gameplay::PlayerInventory none;
                none.player = key_of(p);
                now[3] = gameplay::experience::enchantment_seed(none);
            }
            for (std::size_t k = 0; k < now.size(); ++k)
                if (p.window->data[k] != now[k]) {
                    p.peer->send(wire.container_data(p.window->id, static_cast<std::int16_t>(k),
                                                     static_cast<std::int16_t>(static_cast<std::uint16_t>(now[k] & 0xFFFF))));
                    p.window->data[k] = now[k];
                }
        }
        // A beacon window's properties: its levels and powers (each an effect
        // id plus one), each when it changed (client/beacon).
        if (p.window && at(p.window->pos).is(BlockType::beacon))
            if (const auto be = view.block_entity(overworld, p.window->pos); be && *be) {
                const std::array<std::int32_t, 3> now{(*be)->levels, (*be)->primary, (*be)->secondary};
                for (std::size_t k = 0; k < now.size(); ++k)
                    if (p.window->data[k] != now[k]) {
                        p.peer->send(wire.container_data(p.window->id, static_cast<std::int16_t>(k), static_cast<std::int16_t>(now[k])));
                        p.window->data[k] = now[k];
                    }
            }
        // A lectern window's property: the page its book is open at.
        if (p.window && at(p.window->pos).is(BlockType::lectern))
            if (const auto be = view.block_entity(overworld, p.window->pos); be && *be && p.window->data[0] != (*be)->cook_time) {
                p.peer->send(wire.container_data(p.window->id, 0, static_cast<std::int16_t>((*be)->cook_time)));
                p.window->data[0] = (*be)->cook_time;
            }
        // A brewing stand window's properties: its brew time and its fuel.
        if (p.window && at(p.window->pos).is(BlockType::brewing_stand))
            if (const auto be = view.block_entity(overworld, p.window->pos); be && *be) {
                const std::array<std::int32_t, 2> now{(*be)->cook_time, (*be)->lit_time};
                for (std::size_t k = 0; k < now.size(); ++k)
                    if (p.window->data[k] != now[k]) {
                        p.peer->send(wire.container_data(p.window->id, static_cast<std::int16_t>(k), static_cast<std::int16_t>(now[k])));
                        p.window->data[k] = now[k];
                    }
            }
        // A furnace window's properties, each when it changed.
        if (p.window && at(p.window->pos).is(BlockType::furnace))
            if (const auto be = view.block_entity(overworld, p.window->pos); be && *be) {
                const std::array<std::int32_t, 4> now{(*be)->lit_time, (*be)->lit_total, (*be)->cook_time, (*be)->cook_total};
                for (std::size_t k = 0; k < now.size(); ++k)
                    if (p.window->data[k] != now[k]) {
                        p.peer->send(wire.container_data(p.window->id, static_cast<std::int16_t>(k), static_cast<std::int16_t>(now[k])));
                        p.window->data[k] = now[k];
                    }
            }
    }

    // What a crafter's grid would make (the result its window shows):
    // disabled slots count as empty; a map amid paper is shown marked when
    // its data exists (client/crafter_map).
    contracts::ItemStack crafter_result(BlockPos pos) const {
        const auto view = engine->view();
        const auto be = view.block_entity(overworld, pos);
        if (!data.game || !be || !*be || (*be)->slots.size() != 9) return {};
        std::array<contracts::ItemStack, 9> grid{};
        for (std::size_t k = 0; k < 9; ++k)
            if (((*be)->disabled >> k & 1) == 0) grid[k] = (*be)->slots[k];
        const auto maps = [&view](std::int32_t id) -> const gameplay::MapData* {
            const auto map = view.map(id);
            return map ? *map : nullptr;
        };
        return gameplay::crafted(*data.game, {3, 3, grid}, maps);
    }

    // The parts of the container at `pos` in the menu's slot order: the right
    // half of a double chest first.
    std::vector<BlockPos> container_parts_of(BlockPos pos) const {
        const auto s = at(pos);
        if (!gameplay::is_container(s.type())) return {};
        if (const auto d = gameplay::chest_partner(s)) {
            const auto other = pos.relative(*d);
            const auto o = at(other);
            if (o.is(BlockType::chest) && o.facing() == s.facing() && o.variant() == s.variant() && gameplay::chest_partner(o) == contracts::opposite(*d)) {
                if (s.level() == static_cast<std::uint8_t>(gameplay::blocks::ChestType::right)) return {pos, other};
                return {other, pos};
            }
        }
        return {pos};
    }
    std::size_t container_slot_count(BlockPos pos) const {
        if (gameplay::is_crafting_table(at(pos))) return contracts::table_slots;
        if (gameplay::is_stonecutter(at(pos))) return contracts::cutter_slots;
        if (gameplay::is_smithing_table(at(pos))) return contracts::smith_slots;
        if (gameplay::is_grindstone(at(pos))) return contracts::grind_slots;
        if (gameplay::is_loom(at(pos))) return contracts::loom_slots;
        if (gameplay::is_anvil(at(pos))) return contracts::anvil_slots;
        if (gameplay::is_enchanting_table(at(pos))) return contracts::enchant_slots;
        if (gameplay::is_cartography_table(at(pos))) return contracts::cartography_slots;
        if (at(pos).is(BlockType::beacon)) return contracts::beacon_slots;
        if (at(pos).is(BlockType::lectern)) return 1;
        if (at(pos).is(BlockType::ender_chest)) return contracts::ender_slots;
        std::size_t n = 0;
        for (const auto part : container_parts_of(pos)) n += gameplay::container_size(at(part).type());
        return n;
    }

    // Where items a player throws appear, and how fast: at eye height less
    // 0.3, 0.3 blocks per tick along the look and 0.1 up (the target version
    // adds a small random spread).
    std::pair<contracts::Vec3, contracts::Vec3> throw_from(const Player& p) const {
        constexpr double rad = 3.14159265358979323846 / 180.0;
        const double yaw = p.yaw * rad, pitch = p.pitch * rad;
        return {{p.pos.x, p.pos.y + eye_height(p) - 0.3, p.pos.z},
                {-std::sin(yaw) * std::cos(pitch) * 0.3, -std::sin(pitch) * 0.3 + 0.1, std::cos(yaw) * std::cos(pitch) * 0.3}};
    }

    void open_window(Player& p, BlockPos pos) {
        const auto slots = container_slot_count(pos);
        const auto block = at(pos);
        // Menu types of the protocol's menu registry (minecraft.wiki, Java
        // Edition protocol/Inventory, and observed from the target version):
        // generic_9x3 2, generic_9x6 5, generic_3x3 6, crafter_3x3 7, anvil 8, beacon 9,
        // blast_furnace 10, brewing_stand 11, crafting 12, enchantment 13, furnace 14, lectern 17,
        // grindstone 15, hopper 16, loom 18, shulker_box 20, smithing 21, smoker 22, cartography_table 23,
        // stonecutter 24.
        constexpr std::array<std::int32_t, 3> furnace_menus{14, 22, 10};
        const bool table = gameplay::is_crafting_table(block);
        const bool cutter = gameplay::is_stonecutter(block);
        const bool smith = gameplay::is_smithing_table(block);
        const bool grind = gameplay::is_grindstone(block);
        const bool loom = gameplay::is_loom(block);
        const bool anvil = gameplay::is_anvil(block);
        const bool enchanting = gameplay::is_enchanting_table(block);
        const bool cartography = gameplay::is_cartography_table(block);
        const std::int32_t type = block.is(BlockType::furnace) ? furnace_menus[block.variant() % 3]
                                : block.is(BlockType::crafter) ? 7
                                : block.is(BlockType::brewing_stand) ? 11
                                : block.is(BlockType::lectern) ? 17
                                : block.is(BlockType::beacon) ? 9
                                : block.is(BlockType::shulker_box) ? 20
                                : table ? 12 : cutter ? 24 : smith ? 21 : grind ? 15 : loom ? 18 : anvil ? 8 : enchanting ? 13 : cartography ? 23 : slots == 54 ? 5 : slots == 5 ? 16 : slots == 9 ? 6 : 2;
        // The title is the translation the official servers send for the
        // window (client/titles, both versions); a container's custom name
        // replaces it (for a double chest, the first named half's).
        constexpr std::array<std::string_view, 3> furnace_titles{"container.furnace", "container.smoker", "container.blast_furnace"};
        const std::string_view key = block.is(BlockType::furnace) ? furnace_titles[block.variant() % 3]
                                   : block.is(BlockType::crafter) ? "container.crafter"
                                   : block.is(BlockType::brewing_stand) ? "container.brewing"
                                   : block.is(BlockType::lectern) ? "container.lectern"
                                   : block.is(BlockType::beacon) ? "container.beacon"
                                   : block.is(BlockType::barrel) ? "container.barrel"
                                   : block.is(BlockType::ender_chest) ? "container.enderchest"
                                   : block.is(BlockType::shulker_box) ? "container.shulkerBox"
                                   : table ? "container.crafting" : cutter ? "container.stonecutter" : smith ? "container.upgrade"
                                   : grind ? "container.grindstone_title" : loom ? "container.loom" : anvil ? "container.repair"
                                   : enchanting ? "container.enchant" : cartography ? "container.cartography_table" : slots == 54 ? "container.chestDouble" : slots == 5 ? "container.hopper"
                                   : block.is(BlockType::dispenser) ? (block.variant() == 1 ? "container.dropper" : "container.dispenser")
                                                                    : "container.chest";
        auto title = translation(key, protocol::nbt::Tag::list(protocol::nbt::TagType::string));
        const auto view = engine->view();
        const auto name_type = wire.components().type_id("custom_name");
        auto named = container_parts_of(pos);
        if (block.is(BlockType::beacon)) named = {pos}; // a beacon's own name
        for (const auto part : named) {
            const auto be = view.block_entity(overworld, part);
            if (!be || !*be) continue;
            const auto added = (*be)->components.added();
            const auto it = std::ranges::find(added, name_type, &contracts::ComponentPatch::Added::type);
            if (it == added.end()) continue;
            protocol::Reader r(it->value);
            if (auto name = protocol::nbt::read_network(r)) {
                title = std::move(*name);
                break;
            }
        }
        if (p.window) close_window(p, true);
        // The client's own window as last sent, which the new window's player
        // slots will update.
        p.own = p.remote_window == 0 ? p.remote : std::vector<std::optional<WireStack>>{};
        p.window = Player::Window{p.next_window, pos, slots};
        p.next_window = p.next_window % 100 + 1;
        p.peer->send(wire.open_screen(p.window->id, type, title));
        sync_inventory(p);
    }
    // Closes the open window; `tell` sends the close to the client.
    void close_window(Player& p, bool tell) {
        if (!p.window) return;
        if (tell) p.peer->send(wire.close_screen(p.window->id));
        admit_close(p);
        p.window.reset();
        // The client keeps the player slots the window showed and carries
        // nothing in its own window: only what differs from that is sent.
        if (p.own.size() == inventory_slots) {
            p.remote_window = 0;
            p.remote = std::move(p.own);
            p.remote_carried = WireStack{};
        } else {
            p.remote_window = -1;
        }
        p.own.clear();
    }
    void admit_close(Player& p) {
        const auto [position, velocity] = throw_from(p);
        std::optional<BlockPos> container;
        if (p.window) container = p.window->pos;
        if (const auto id = queue.admit(contracts::CloseContainer{key_of(p), position, velocity, false, container}, overworld)) p.closing = *id;
        else log_warn("game: command queue full; input dropped");
    }
    // The stacks a close put back into the player's inventory, each as the
    // official servers send it (by its inventory index) before the window's
    // differences.
    void send_put_back(Player& p) {
        if (!p.closing) return;
        const auto& trace = engine->trace();
        const auto applied = std::ranges::find_if(trace, [&](const contracts::TraceRecord& r) {
            return (r.kind == contracts::TraceKind::command_applied || r.kind == contracts::TraceKind::command_rejected) && r.a == *p.closing;
        });
        if (applied == trace.end()) return;
        p.closing.reset();
        if (applied->kind != contracts::TraceKind::command_applied) return;
        const auto key = key_of(p);
        const auto view = engine->view();
        const auto inv = view.player(key);
        if (!inv || !*inv) return;
        for (auto r = std::next(applied); r != trace.end(); ++r) {
            // The close's own changes: up to the next command, within the
            // command phase (a dispenser's armor in a later phase is not one).
            if (r->kind == contracts::TraceKind::command_applied || r->kind == contracts::TraceKind::command_rejected
                || r->key.phase != contracts::Phase::commands)
                break;
            if (r->kind != contracts::TraceKind::player_slot_changed || r->a != key.hi || r->b != key.lo) continue;
            const auto slot = static_cast<std::int32_t>(r->c >> 48);
            const std::int32_t index = slot >= 36 && slot <= 44 ? slot - 36 : slot >= 9 && slot <= 35 ? slot : slot >= 5 && slot <= 8 ? 44 - slot : slot == 45 ? 40 : -1;
            if (index < 0) continue;
            // The stack as the record has it then (its components as the
            // slot's now, where it is still that item).
            const auto& now = (*inv)->slots[static_cast<std::size_t>(slot)];
            contracts::ItemStack then{static_cast<std::uint16_t>(r->c >> 8 & 0xFFFF), static_cast<std::uint8_t>(r->c & 0xFF),
                                      static_cast<std::uint16_t>(r->c >> 24 & 0xFFFF), {}};
            if (then.item == now.item) then.components = now.components;
            p.peer->send(wire.player_inventory(index, wire_stack(then.count == 0 ? contracts::ItemStack{} : then)));
        }
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

    // Block shapes for movement checks; unloaded cells are air.
    movement::ShapeLookup shapes() const {
        return [this](BlockPos b) -> movement::BlockShape {
            const auto v = engine->view().block(overworld, b);
            if (!v || v->is_air()) return {};
            return {false, gameplay::physics::collision_boxes(*v, bundle.schema.content_release)};
        };
    }

    // Movement, as the target version's server checks it: the claimed move
    // is replayed with collision from the last accepted position. A move that
    // ends more than 0.25 blocks away horizontally (survival; vertical
    // differences are not compared) is refused unless the player was stuck in
    // a block, and so is a move into a block the player was not already in
    // (any mode but spectator). Refused moves teleport the player back.
    void handle(Player& p, intent::Move& m) {
        if (p.dead) return;
        if (m.rotation && std::isfinite(m.rotation->first) && std::isfinite(m.rotation->second)) {
            p.yaw = m.rotation->first;
            p.pitch = std::clamp(m.rotation->second, -90.0f, 90.0f);
            p.moved = true;
        }
        const bool was_on_ground = p.on_ground;
        p.on_ground = m.on_ground;
        if (p.awaiting_teleport) return; // moves before the teleport is confirmed are stale
        const auto to = m.position.value_or(p.pos);
        if (!finite(to) || std::abs(to.x) >= 3.0e7 || std::abs(to.z) >= 3.0e7 || std::abs(to.y) > 4096.0) {
            p.peer->kick("Invalid movement");
            return;
        }
        const double dx = to.x - p.pos.x, dy = to.y - p.pos.y, dz = to.z - p.pos.z;
        if (dx * dx + dy * dy + dz * dz > max_move_squared) {
            log_warn(std::format("{} moved too quickly ({:.1f} blocks); corrected", p.name, std::sqrt(dx * dx + dy * dy + dz * dz)));
            teleport(p, p.pos, false);
            return;
        }
        // Positions outside loaded chunks are refused, so players cannot move
        // into terrain the server has not delivered.
        const auto chunk = chunk_at(to);
        if (!engine->view().block(overworld, {static_cast<std::int32_t>(std::floor(to.x)), bundle.world.min_y,
                                  static_cast<std::int32_t>(std::floor(to.z))})) {
            teleport(p, p.pos, false);
            return;
        }
        bool landed_on_block = false;
        if (p.game_mode != spectator) {
            const auto look = shapes();
            const auto from = box_of(p, p.pos);
            const auto moved = movement::collide(from, {dx, dy, dz}, was_on_ground, look);
            const double ex = to.x - (p.pos.x + moved.x), ez = to.z - (p.pos.z + moved.z);
            const bool wrongly = p.game_mode == survival && ex * ex + ez * ez > 0.0625;
            if ((wrongly && !movement::collides(from, look)) || movement::collides_with_anything_new(from, box_of(p, to), look)) {
                log_warn(std::format("{} moved wrongly; corrected", p.name));
                teleport(p, p.pos, false);
                return;
            }
            landed_on_block = dy < 0.0 && moved.y != dy;
            // Hovering: not sinking, not on anything, nothing around, and not
            // allowed to fly.
            // A levitating player rises without being kicked (client/effect_edges).
            static const auto levitation = gameplay::effects::id("levitation");
            const bool levitating = std::ranges::any_of(p.effects, [](const auto& e) { return e.id == levitation; });
            p.floating = p.game_mode == survival && !config.allow_flight && dy >= -0.03125 && !landed_on_block && !levitating
                         && movement::no_blocks_around(box_of(p, to), look);
        }
        // Leaving the ground upwards is a jump: its velocity, an impulse
        // those who see the player are told (client/combat: (0, 0.42, 0)
        // standing; jump boost's 0.1 a level and a sprint's 0.2 forward by
        // minecraft.wiki, not recorded).
        if (was_on_ground && !m.on_ground && dy > 0.0) {
            exhaust(p, p.sprinting ? 0.2f : 0.05f);
            jumped(p);
        }
        p.pos = to;
        if (m.position) p.moved = true;
        // A move inside a sweet berry bush stops the player (client/drown:
        // the hurts of one walked through tell a velocity of 0).
        if ((dx != 0.0 || dy != 0.0 || dz != 0.0) && in_bush(p, false)) p.stopped = true;
        move_exhaustion(p, dx, dy, dz);
        fall(p, dy, m.on_ground);
        if (chunk.x != p.center.x || chunk.z != p.center.z) {
            p.center = chunk;
            p.peer->send(wire.chunk_center(chunk.x, chunk.z));
            move_tickets(p, chunk);
        }
    }

    // ---- vitals --------------------------------------------------------------
    //
    // As the target version keeps them for players (the documented health and
    // hunger rules). Food comes back by eating and drinking (below) and with
    // the saturation effect.

    // Whether the player's box touches water.
    bool in_water(const Player& p) const { return fluid_in(box_of(p, p.pos)); }
    bool fluid_in(const movement::Aabb& player_box) const {
        const auto box = player_box.deflate(0.001);
        for (auto x = static_cast<std::int32_t>(std::floor(box.min_x)); x <= static_cast<std::int32_t>(std::floor(box.max_x)); ++x)
            for (auto y = static_cast<std::int32_t>(std::floor(box.min_y)); y <= static_cast<std::int32_t>(std::floor(box.max_y)); ++y)
                for (auto z = static_cast<std::int32_t>(std::floor(box.min_z)); z <= static_cast<std::int32_t>(std::floor(box.max_z)); ++z) {
                    const BlockPos c{x, y, z};
                    const auto h = gameplay::fluid_height(block_now(c), block_now(c.relative(contracts::Direction::up)));
                    if (h > 0.0f && c.y + static_cast<double>(h) >= box.min_y) return true;
                }
        return false;
    }

    // Whether the player's box touches lava, by its height in each cell as
    // fluid_in reads water.
    bool in_lava(const Player& p) const {
        const auto box = box_of(p, p.pos).deflate(0.001);
        for (auto x = static_cast<std::int32_t>(std::floor(box.min_x)); x <= static_cast<std::int32_t>(std::floor(box.max_x)); ++x)
            for (auto y = static_cast<std::int32_t>(std::floor(box.min_y)); y <= static_cast<std::int32_t>(std::floor(box.max_y)); ++y)
                for (auto z = static_cast<std::int32_t>(std::floor(box.min_z)); z <= static_cast<std::int32_t>(std::floor(box.max_z)); ++z) {
                    const BlockPos c{x, y, z};
                    const auto h = gameplay::lava_height(block_now(c), block_now(c.relative(contracts::Direction::up)));
                    if (h > 0.0f && c.y + static_cast<double>(h) >= box.min_y) return true;
                }
        return false;
    }
    // Whether rain falls on the player (client/fire: put out as it starts
    // raining): the rain shown (a level above 0.2), at the block of the
    // player's feet or of the top of their box, with nothing above it that
    // stops movement or holds a fluid (Circuit's rule: a player under a roof
    // was not recorded). Every column is plains, where it rains.
    bool in_rain(const Player& p) const {
        if (!engine->weather().rain_visible()) return false;
        const auto open_above = [&](double y) {
            const auto x = static_cast<std::int32_t>(std::floor(p.pos.x)), z = static_cast<std::int32_t>(std::floor(p.pos.z));
            const auto top = bundle.world.min_y + bundle.world.height;
            for (auto by = static_cast<std::int32_t>(std::floor(y)); by < top; ++by) {
                const auto b = block_now({x, by, z});
                if (b.is_air()) continue;
                if (b.is(BlockType::water) || gameplay::is_lava(b) || gameplay::waterlogged_block(b)) return false;
                if (!gameplay::physics::collision_boxes(b, bundle.schema.content_release).empty()) return false;
            }
            return true;
        };
        return open_above(p.pos.y) || open_above(p.pos.y + body_height(p));
    }
    // A block as a player's tick sees it: with the blocks this tick's
    // commands set (the rules apply them at its end), as the official
    // servers apply a command's blocks before the players' tick
    // (client/drown: water and powder snow taken away as a hurt comes, the
    // air and frozen ticks at once as a tick sooner).
    BlockState block_now(BlockPos b) const {
        if (const auto it = pending_setblocks.find(b); it != pending_setblocks.end()) return it->second;
        return at(b);
    }
    // A block's registry name, without the namespace ("" for blocks the
    // registry does not name).
    static std::string_view block_name(BlockState s) {
        const auto state = gameplay::physics::registry_state_of(s);
        const auto block = state ? gameplay::registry::block_of(*state) : std::nullopt;
        return block ? gameplay::registry::game_name(*block) : std::string_view{};
    }

    // Burning (client/fire): a hurt (on_fire) when the remaining fire ticks
    // are a multiple of 20 (none in lava: only lava's hurts there), and their
    // count down. Creative and spectator players do not burn (bob in creative
    // mode in fire was never shown burning; spectators by Circuit's choice).
    void burn_tick(Player& p) {
        if (p.game_mode == creative || p.game_mode == spectator) p.fire_ticks = gameplay::damage::not_burning;
        const auto b = gameplay::damage::burn(p.fire_ticks);
        p.fire_ticks = b.ticks;
        if (b.hurts && !in_lava(p)) hurt(p, 1.0f, Harm::on_fire);
    }
    // The on-fire flag, from the fire ticks left (told at the next entity
    // update).
    void decide_burning(Player& p) { p.burning = !p.dead && p.fire_ticks > 0; }
    // A player's tick of fire (client/fire), in each version's order:
    // 1.21.8 burns, decides the flag, then takes the blocks touched, lava
    // and water; 1.21.4 takes water first (a burning hurt due as the player
    // enters it does not come), burns, takes lava, then decides the flag (a
    // tick sooner after lava) and takes the blocks. Either way the burning
    // after lava hurts first 21 ticks after its last tick there.
    void fire_tick(Player& p) {
        if (wire.protocol() >= 772) {
            burn_tick(p);
            decide_burning(p);
            touch_blocks(p);
            touch_lava(p);
            touch_water(p);
        } else {
            touch_water(p);
            burn_tick(p);
            touch_lava(p);
            decide_burning(p);
            touch_blocks(p);
        }
        // Lava halves the distance fallen each tick (minecraft.wiki "Damage";
        // not recorded).
        if (!p.dead && in_lava(p)) p.fall_distance *= 0.5;
    }
    // What a player touches (client/fire): fire (1, soul fire 2 by
    // minecraft.wiki "Soul Fire"; catching fire as gameplay::damage counts
    // it) and a magma block stood on without sneaking (1). Spectators touch
    // nothing (Circuit's choice); creative players take no damage and do
    // not burn.
    bool touches(const Player& p) const { return !p.dead && p.game_mode != spectator && p.game_mode != creative; }
    // The cells a player's box meets (shrunk by 1e-5: a box against a block's
    // side does not meet it).
    template <typename F>
    void each_cell_met(const Player& p, F&& f) const {
        const auto box = box_of(p, p.pos).deflate(1e-5);
        for (auto x = static_cast<std::int32_t>(std::floor(box.min_x)); x <= static_cast<std::int32_t>(std::floor(box.max_x)); ++x)
            for (auto y = static_cast<std::int32_t>(std::floor(box.min_y)); y <= static_cast<std::int32_t>(std::floor(box.max_y)); ++y)
                for (auto z = static_cast<std::int32_t>(std::floor(box.min_z)); z <= static_cast<std::int32_t>(std::floor(box.max_z)); ++z)
                    f(BlockPos{x, y, z});
    }
    // Whether the player's box meets a sweet berry bush (one that has grown
    // at least once, with `grown`).
    bool in_bush(const Player& p, bool grown) const {
        bool met = false;
        each_cell_met(p, [&](BlockPos c) {
            const auto s = block_now(c);
            if (block_name(s) == "sweet_berry_bush" && (!grown || block_property(s, "age") > 0)) met = true;
        });
        return met;
    }
    // Contacts: fire, a cactus's cell (client/drown: one at 10.705 hurt, its
    // box a sixteenth further), a sweet berry bush grown at least once while
    // moving across it (client/drown: walked through, not stood in), powder
    // snow (for the frost), and a magma block stood on without sneaking.
    void touch_blocks(Player& p) {
        p.in_fire = false;
        p.in_snow = false;
        // A player teleported in this tick touches nothing until the next
        // (client/drown: teleported beside a cactus, the first hurt in the
        // tick after the teleport).
        if (!touches(p) || p.teleported) return;
        bool soul = false, cactus = false, bush = false;
        each_cell_met(p, [&](BlockPos c) {
            const auto s = block_now(c);
            const auto name = block_name(s);
            if (name == "fire") p.in_fire = true;
            else if (name == "soul_fire") p.in_fire = soul = true;
            else if (name == "cactus") cactus = true;
            else if (name == "sweet_berry_bush" && block_property(s, "age") > 0) bush = true;
            else if (name == "powder_snow") p.in_snow = true;
        });
        if (p.in_fire) {
            p.fire_ticks = gameplay::damage::touch_fire(p.fire_ticks, random_draw() < 0.5f);
            hurt(p, soul ? 2.0f : 1.0f, soul ? Harm::soul_fire : Harm::in_fire);
        } else if (p.fire_ticks < 0) {
            p.fire_ticks = gameplay::damage::not_burning;
        }
        if (cactus && !p.dead) hurt(p, 1.0f, Harm::cactus);
        if (bush && !p.dead && (p.pos.x != p.tick_pos.x || p.pos.z != p.tick_pos.z)) hurt(p, 1.0f, Harm::sweet_berry_bush);
        if (!p.dead && p.on_ground && !p.sneaking && block_name(block_now(standing_on(p))) == "magma_block") hurt(p, 1.0f, Harm::hot_floor);
    }
    // A block property's number (0 for a block without it).
    static int block_property(BlockState s, std::string_view key) {
        const auto state = gameplay::physics::registry_state_of(s);
        if (!state) return 0;
        for (const auto& [k, v] : gameplay::registry::properties(*state))
            if (k == key) return v == "true" ? 1 : v == "false" ? 0 : std::stoi(std::string(v));
        return 0;
    }

    // ---- poses, breath and frost (client/drown) -----------------------------
    // A player's pose: forced by the space around them, else crouching while
    // the shift key is down (but for a spectator or a flyer); its box and
    // eye height (minecraft.wiki "Player": 1.8 and 1.62 standing, 1.5 and
    // 1.27 crouching, 0.6 and 0.4 crawling).
    std::int32_t pose_of(const Player& p) const {
        if (p.forced_pose) return *p.forced_pose;
        return p.sneaking && p.game_mode != spectator && !p.flying ? 5 : 0;
    }
    // The box follows the forced pose, else the shift key (as before poses
    // were forced, also for flyers).
    static double pose_height(std::int32_t pose) { return pose == 3 ? 0.6 : pose == 5 ? movement::crouching_height : movement::standing_height; }
    static double body_height(const Player& p) {
        return p.forced_pose ? pose_height(*p.forced_pose) : p.sneaking ? movement::crouching_height : movement::standing_height;
    }
    static double eye_height(const Player& p) {
        const auto h = body_height(p);
        return h == pose_height(3) ? 0.4 : h == movement::crouching_height ? 1.27 : 1.62;
    }
    static movement::Aabb box_of(const Player& p, Vec3 at) { return movement::player_box(at, body_height(p)); }
    // At the end of a player's tick, the pose the space forces: where the
    // standing box meets a block's collision shape, crouching if that fits
    // (minecraft.wiki "Sneaking": a player under a 1.5-block ceiling stays
    // crouched; not recorded), else crawling if that does (client/drown:
    // stone at the head of a player in a pit, crawling at the next entity
    // update; stone at the feet and head, no pose).
    void decide_pose(Player& p) {
        p.forced_pose.reset();
        if (p.dead || p.game_mode == spectator || p.flying) return;
        const movement::ShapeLookup look = [this](BlockPos b) -> movement::BlockShape {
            const auto v = block_now(b);
            if (v.is_air()) return {};
            return {false, gameplay::physics::collision_boxes(v, bundle.schema.content_release)};
        };
        const auto fits = [&](double height) { return !movement::collides(movement::player_box(p.pos, height).deflate(1e-7), look); };
        if (fits(movement::standing_height)) return;
        if (fits(movement::crouching_height)) p.forced_pose = 5;
        else if (fits(pose_height(3))) p.forced_pose = 3;
    }
    // Suffocation: the eyes inside the collision shape of a block that
    // conducts redstone (client/drown: stone hurts every 10 ticks, glass
    // not; minecraft.wiki "Damage": the eyeline inside certain blocks;
    // Circuit takes the conductors for them).
    bool eyes_in_wall(const Player& p) const {
        if (p.game_mode == spectator) return false;
        const Vec3 eye{p.pos.x, p.pos.y + eye_height(p), p.pos.z};
        const BlockPos cell{static_cast<std::int32_t>(std::floor(eye.x)), static_cast<std::int32_t>(std::floor(eye.y)),
                            static_cast<std::int32_t>(std::floor(eye.z))};
        const auto s = block_now(cell);
        if (s.is_air() || !gameplay::redstone_conductor(s)) return false;
        for (const auto& b : gameplay::physics::collision_boxes(s, bundle.schema.content_release))
            if (eye.x > cell.x + b.min_x && eye.x < cell.x + b.max_x && eye.y > cell.y + b.min_y && eye.y < cell.y + b.max_y
                && eye.z > cell.z + b.min_z && eye.z < cell.z + b.max_z)
                return true;
        return false;
    }
    // Breath (client/drown): with the eyes under water a player who cannot
    // breathe there (survival, without water breathing or conduit power)
    // loses air, and as it runs out drowns: on 1.21.8 entity event 67 (the
    // bubbles, also with drowningDamage off) before the hurt; otherwise the
    // air comes back. The oxygen bonus attribute, which the official
    // servers consult as air is lost, is the player's from the first time
    // (sent without modifiers; the respiration enchantment that adds to it
    // is not modeled).
    void breathe(Player& p) {
        static const auto water_breathing = gameplay::effects::id("water_breathing"), conduit = gameplay::effects::id("conduit_power");
        const bool breathes = (water_breathing && effect_amplifier(p, *water_breathing)) || (conduit && effect_amplifier(p, *conduit));
        const bool losing = touches(p) && !breathes && eyes_in_water(p);
        if (losing && std::ranges::find(p.attributes, gameplay::attributes::Attribute::oxygen_bonus, &gameplay::attributes::Instance::attribute)
                          == p.attributes.end()) {
            attribute(p, gameplay::attributes::Attribute::oxygen_bonus);
            p.attributes_changed.insert(gameplay::attributes::Attribute::oxygen_bonus);
        }
        const auto b = gameplay::damage::breathe(p.air, losing);
        p.air = b.air;
        if (!b.drowns) return;
        if (wire.protocol() >= 772) {
            const auto bubbles = wire.entity_event(p.entity_id, 67);
            p.peer->send(bubbles);
            for (const auto viewer : p.seen_by)
                if (const auto o = players.find(viewer); o != players.end()) o->second.peer->send(bubbles);
        }
        hurt(p, 2.0f, Harm::drown);
    }
    // The turtle shell worn (as the last tick ended: client/drown, its water
    // breathing at the tick after it went on) gives 10 seconds of water
    // breathing, without particles, each tick the eyes are out of water.
    void turtle_shell(Player& p) {
        static const auto water_breathing = gameplay::effects::id("water_breathing");
        static const auto shell = gameplay::items::by_name("minecraft:turtle_helmet");
        const auto& head = p.equipped[static_cast<std::size_t>(gameplay::equipment::Slot::head)];
        if (!water_breathing || !shell || head.empty() || head.item != *shell || eyes_in_water(p)) return;
        give_effect(p, *water_breathing, {0, 200, false, false, true});
    }
    // Frost (client/drown): in powder snow a player who can freeze (survival,
    // wearing no leather armor) freezes; fully frozen, a hurt each 40 ticks
    // of their age (as natural regeneration counts it). The slowness goes
    // to the movement speed as `powder_snow`, again each tick the frozen
    // ticks change and every second tick while they stay (the official
    // servers' cadence at 140, in step with the hurts), and goes with them.
    void freeze(Player& p) {
        using gameplay::attributes::Attribute;
        bool leather = false;
        for (const auto slot : {gameplay::equipment::Slot::head, gameplay::equipment::Slot::chest, gameplay::equipment::Slot::legs,
                                gameplay::equipment::Slot::feet}) {
            const auto& worn = p.equipped[static_cast<std::size_t>(slot)];
            const auto* piece = worn.empty() ? nullptr : gameplay::equipment::wearable(worn.item);
            if (piece && piece->slot == slot && piece->sound == gameplay::equipment::EquipSound::leather) leather = true;
        }
        const auto was = p.frozen_ticks;
        p.frozen_ticks = gameplay::damage::frost(p.frozen_ticks, p.in_snow && touches(p) && !leather);
        if (p.frozen_ticks > 0 && (p.frozen_ticks != was || p.ticks_alive % 2 == 0))
            add_modifier(p, Attribute::movement_speed, {"powder_snow", gameplay::damage::frost_slowness(p.frozen_ticks), gameplay::attributes::Operation::add_value}, true);
        else if (p.frozen_ticks == 0 && was > 0)
            remove_modifier(p, Attribute::movement_speed, "powder_snow");
        if (p.frozen_ticks >= gameplay::damage::frozen_max && p.ticks_alive % 40 == 0) hurt(p, 1.0f, Harm::freeze);
    }
    // The block a player on the ground stands on: the one under the middle
    // of their feet if it bears them, else the nearest under their box that
    // does (minecraft.wiki "Magma Block": its edge hurts since 1.20; how the
    // official servers pick among several was not recorded).
    BlockPos standing_on(const Player& p) const {
        const auto y = static_cast<std::int32_t>(std::floor(p.pos.y - 0.2));
        const BlockPos middle{static_cast<std::int32_t>(std::floor(p.pos.x)), y, static_cast<std::int32_t>(std::floor(p.pos.z))};
        const auto bears = [&](BlockPos b) { return !gameplay::physics::collision_boxes(block_now(b), bundle.schema.content_release).empty(); };
        if (bears(middle)) return middle;
        std::optional<BlockPos> best;
        double nearest = 0.0;
        const auto box = box_of(p, p.pos).deflate(1e-5);
        for (auto x = static_cast<std::int32_t>(std::floor(box.min_x)); x <= static_cast<std::int32_t>(std::floor(box.max_x)); ++x)
            for (auto z = static_cast<std::int32_t>(std::floor(box.min_z)); z <= static_cast<std::int32_t>(std::floor(box.max_z)); ++z) {
                const BlockPos b{x, y, z};
                if (!bears(b)) continue;
                const double dx = x + 0.5 - p.pos.x, dz = z + 0.5 - p.pos.z, d = dx * dx + dz * dz;
                if (!best || d < nearest) best = b, nearest = d;
            }
        return best.value_or(middle);
    }
    // Lava (client/fire): 4, with its burn sound for all nearby, the player
    // too (on 1.21.8 a death by it is followed by the extinguish sound, as
    // an earlier form of the probe recorded once; 1.21.4's not recorded),
    // and 15 seconds of burning.
    void touch_lava(Player& p) {
        if (!touches(p) || !in_lava(p)) return;
        if (hurt(p, 4.0f, Harm::lava)) {
            sound_near(p, wire.player_sound(Wire::PlayerSound::generic_burn, p.pos, 0.4f, 2.0f + random_draw() * 0.4f,
                                            static_cast<std::int64_t>(sound_seed++)), true);
            if (p.dead && wire.protocol() >= 772) extinguish_sound(p);
        }
        if (!p.dead) p.fire_ticks = gameplay::damage::touch_lava(p.fire_ticks);
    }
    // Water puts the fire out, and so does rain (rain_on_players).
    void touch_water(Player& p) {
        if (touches(p) && p.fire_ticks > 0 && in_water(p)) extinguish(p);
    }
    // Put out, with the extinguish sound: on 1.21.8 for the player and the
    // others within 16 blocks, on 1.21.4 for the others (client/fire).
    void extinguish(Player& p) {
        p.fire_ticks = gameplay::damage::not_burning;
        extinguish_sound(p);
    }
    void extinguish_sound(Player& p) {
        const auto a = random_draw();
        const auto pitch = 1.6f + (a - random_draw()) * 0.4f;
        sound_near(p, wire.player_sound(Wire::PlayerSound::extinguish_fire, p.pos, 0.7f, pitch, static_cast<std::int64_t>(sound_seed++)),
                   wire.protocol() >= 772);
    }
    // Rain puts burning players out once the tick's weather is published, as
    // the official servers do in the tick it starts to show (client/fire:
    // the extinguish sound after start_raining, the flag two ticks later).
    void rain_on_players() {
        for (auto& [id, p] : players)
            if (touches(p) && p.fire_ticks > 0 && in_rain(p)) extinguish(p);
    }

    // Exhaustion from actions; players in creative and spectator mode do not tire.
    void exhaust(Player& p, float amount) {
        if (p.game_mode != survival || p.dead) return;
        p.exhaustion = std::min(p.exhaustion + amount, 40.0f);
    }

    // Exhaustion from an accepted move: 0.01 per metre through water (with
    // the eyes under water, in three dimensions), 0.1 per metre sprinting on
    // the ground; distances count in whole centimetres.
    void move_exhaustion(Player& p, double dx, double dy, double dz) {
        const auto cm = [](double d) { return static_cast<float>(std::lround(d * 100.0)); };
        if (eyes_in_water(p)) exhaust(p, 0.01f * cm(std::sqrt(dx * dx + dy * dy + dz * dz)) * 0.01f);
        else if (in_water(p)) exhaust(p, 0.01f * cm(std::sqrt(dx * dx + dz * dz)) * 0.01f);
        else if (p.on_ground && p.sprinting) exhaust(p, 0.1f * cm(std::sqrt(dx * dx + dz * dz)) * 0.01f);
    }

    // Fall distance grows while the player moves down off the ground; on
    // landing, every block beyond the safe fall distance deals one point of
    // damage (none on a slime block unless sneaking). 1.21.8 counts the move
    // that lands too, 1.21.4 does not (client/falls). Water and flying reset
    // it.
    void fall(Player& p, double dy, bool on_ground) {
        // Slow falling keeps the distance at 0 while the player moves down
        // (client/drown: a fall of 10 blocks with it hurt nothing; minecraft.wiki
        // "Slow Falling": a fall after the effect ends counts from there),
        // and so does a sweet berry bush (minecraft.wiki "Sweet Berry Bush";
        // not recorded).
        static const auto slow_falling = gameplay::effects::id("slow_falling");
        const bool slowed = slow_falling && effect_amplifier(p, *slow_falling) && dy < 0.0;
        if (p.game_mode == spectator || p.flying || in_water(p) || slowed || in_bush(p, false)) {
            p.fall_distance = 0.0;
            return;
        }
        if (!on_ground) {
            if (dy < 0.0) p.fall_distance -= dy;
            return;
        }
        const auto distance = p.fall_distance + (wire.protocol() >= 772 && dy < 0.0 ? -dy : 0.0);
        p.fall_distance = 0.0;
        if (distance <= 0.0) return;
        const BlockPos below{static_cast<std::int32_t>(std::floor(p.pos.x)), static_cast<std::int32_t>(std::floor(p.pos.y - 0.2)),
                             static_cast<std::int32_t>(std::floor(p.pos.z))};
        if (at(below).is(BlockType::slime_block) && !p.sneaking) return;
        // Beyond the safe fall distance (3, more with jump boost), rounded up
        // on 1.21.4 and down on 1.21.8 (client/attribute_rules: 5.5 blocks
        // took 3 and 2; client/falls).
        const auto beyond = distance - attribute_value(p, gameplay::attributes::Attribute::safe_fall_distance);
        const auto damage = wire.protocol() >= 772 ? std::floor(beyond) : std::ceil(beyond);
        if (damage <= 0.0 || p.game_mode != survival) return;
        // The fall's sound comes first, for the others nearby (client/effects_heal:
        // 7 points gave big_fall; 4 or less gives small_fall), also with
        // fallDamage off, when the hurt does not come (client/drown).
        sound_around(p, damage > 4.0 ? Wire::PlayerSound::big_fall : Wire::PlayerSound::small_fall, 1.0f);
        hurt(p, static_cast<float>(damage), Harm::fall, distance);
    }

    enum class Harm : std::uint8_t {
        fall, void_, starve, kill, magic, wither, in_fire, soul_fire, on_fire, lava, hot_floor, drown, in_wall, cactus, freeze, sweet_berry_bush,
        player_attack
    };
    // What a damage type does, from the official data packs (both versions
    // alike): its name, death message key, exhaustion, whether armor (not in
    // bypasses_armor) and the resistance effect (not in bypasses_resistance
    // or bypasses_effects: the void, /kill, starvation) reduce it, whether it
    // is fire (is_fire: fire resistance keeps it off; netherite does not wear
    // by it), whether it moves the player (all but no_impact, drowning: no
    // velocity is told), the game rule that turns it off (fallDamage,
    // fireDamage for fire, drowningDamage, freezeDamage: client/drown), and
    // the hurt sound those nearby hear (its effects: burning gives
    // hurt_on_fire, client/fire; drowning hurt_drown, freezing hurt_freeze
    // and poking hurt_sweet_berry_bush, client/drown).
    struct HarmType {
        const char* name;
        const char* death;
        float exhaustion;
        bool armor, resistance, fire, impact;
        const char* rule;
        Wire::PlayerSound sound;
    };
    static const HarmType& harm_type(Harm h) {
        using enum Wire::PlayerSound;
        static const std::array<HarmType, 17> types{{
            {"minecraft:fall", "death.attack.fall", 0.0f, false, true, false, true, "fallDamage", hurt},
            {"minecraft:out_of_world", "death.attack.outOfWorld", 0.0f, false, false, false, true, nullptr, hurt},
            {"minecraft:starve", "death.attack.starve", 0.0f, false, false, false, true, nullptr, hurt},
            {"minecraft:generic_kill", "death.attack.genericKill", 0.0f, false, false, false, true, nullptr, hurt},
            {"minecraft:magic", "death.attack.magic", 0.0f, false, true, false, true, nullptr, hurt},
            {"minecraft:wither", "death.attack.wither", 0.0f, false, true, false, true, nullptr, hurt},
            {"minecraft:in_fire", "death.attack.inFire", 0.1f, true, true, true, true, "fireDamage", hurt_on_fire},
            {"minecraft:in_fire", "death.attack.inFire", 0.1f, true, true, true, true, "fireDamage", hurt_on_fire},
            {"minecraft:on_fire", "death.attack.onFire", 0.0f, false, true, true, true, "fireDamage", hurt_on_fire},
            {"minecraft:lava", "death.attack.lava", 0.1f, true, true, true, true, "fireDamage", hurt_on_fire},
            {"minecraft:hot_floor", "death.attack.hotFloor", 0.1f, true, true, true, true, "fireDamage", hurt_on_fire},
            {"minecraft:drown", "death.attack.drown", 0.0f, false, true, false, false, "drowningDamage", hurt_drown},
            {"minecraft:in_wall", "death.attack.inWall", 0.0f, false, true, false, true, nullptr, hurt},
            {"minecraft:cactus", "death.attack.cactus", 0.1f, true, true, false, true, nullptr, hurt},
            {"minecraft:freeze", "death.attack.freeze", 0.0f, false, true, false, true, "freezeDamage", hurt_freeze},
            {"minecraft:sweet_berry_bush", "death.attack.sweetBerryBush", 0.1f, true, true, false, true, nullptr, hurt_sweet_berry_bush},
            {"minecraft:player_attack", "death.attack.player", 0.1f, true, true, false, true, nullptr, hurt},
        }};
        return types[static_cast<std::size_t>(h)];
    }
    // Whether a game rule that turns damage off leaves it on.
    bool harm_allowed(const HarmType& type) const {
        return !type.rule || gameplay::game_rule_value(bundle, rules, type.rule) != 0;
    }

    // A player's own sound, for the other players within 16 blocks (the
    // player's client plays its own).
    void sound_around(const Player& p, Wire::PlayerSound sound, float pitch) {
        sound_near(p, wire.player_sound(sound, p.pos, 1.0f, pitch, static_cast<std::int64_t>(sound_seed++)), false);
    }
    // A sound at a position, for the players within 16 blocks of it (and the
    // player there, with `self`).
    void sound_near(const Player& p, const Bytes& packet, bool self, std::optional<Vec3> where = std::nullopt) {
        const auto at = where.value_or(p.pos);
        for (auto& [oid, o] : players) {
            if (&o == &p && !self) continue;
            const double dx = o.pos.x - at.x, dy = o.pos.y - at.y, dz = o.pos.z - at.z;
            if (dx * dx + dy * dy + dz * dz < 256.0) o.peer->send(packet);
        }
    }
    // A uniform draw in [0, 1): the official servers draw from the entity's
    // random; Circuit's own draws are splitmix64 of a counter.
    float random_draw() {
        auto z = (++voice_draws) * 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return static_cast<float>((z ^ (z >> 31)) >> 40) / 16777216.0f;
    }
    // A voice's pitch: 1 give or take 0.2, the difference of two uniform draws.
    float voice_pitch() {
        const auto a = random_draw();
        return (a - random_draw()) * 0.2f + 1.0f;
    }

    // The amplifier of an effect a player has (its level shown), if any.
    static std::optional<std::int32_t> effect_amplifier(const Player& p, std::uint8_t id) {
        const auto it = std::ranges::find(p.effects, id, &gameplay::effects::Effect::id);
        if (it == p.effects.end() || it->levels.empty()) return std::nullopt;
        return it->levels.front().amplifier;
    }
    // Damage: creative and spectator players take none but from the void
    // (spectators not even that); fire resistance keeps fire's off. Within
    // the first 10 ticks of the 20-tick hurt cooldown, damage counts only by
    // how much it exceeds the last. Then, as both official servers reduce it
    // (client/fire; minecraft.wiki "Armor", "Resistance"): armor wears by a
    // quarter of it (at least one) and takes away what its points and
    // toughness allow, resistance 20% a level, absorption what it holds.
    // Damage of the types that tire costs that exhaustion. Damage a player
    // dealt (`by`: client/combat) names them in the damage event and the
    // death message, shows the one hurt its direction (the hurt animation)
    // and knocks them 0.4 away. Damage within the cooldown that exceeds the
    // last takes only the difference, without the damage event, the
    // animation, the knockback or a sound (client/combat). Whether it hurt.
    bool hurt(Player& p, float amount, Harm harm, double fall_distance = 0.0, const Player* by = nullptr) {
        if (p.dead || amount <= 0.0f) return false;
        // `/kill` reaches players in every game mode.
        if (harm != Harm::void_ && harm != Harm::kill && p.game_mode != survival) return false;
        if (harm == Harm::void_ && p.game_mode == spectator) return false;
        const auto& type = harm_type(harm);
        if (!harm_allowed(type)) return false;
        static const auto fire_resistance = gameplay::effects::id("fire_resistance"), resistance = gameplay::effects::id("resistance");
        if (type.fire && fire_resistance && effect_amplifier(p, *fire_resistance)) return false;
        if (by && harm == Harm::player_attack && blocking(p)) {
            if (facing(p, *by)) {
                block(p, amount, *by);
                return false;
            }
            // 1.21.8 knocks a holder hit from behind 0.5 towards the attacker
            // first, as a blocked attack does (client/shield; 1.21.4 not).
            if (wire.protocol() >= 772) knock(p, 0.5, by->pos.x - p.pos.x, by->pos.z - p.pos.z);
        }
        float applied = amount;
        const bool fresh = p.hurt_cooldown <= 10;
        if (!fresh) {
            if (amount <= p.last_hurt) return false;
            applied = amount - p.last_hurt;
        } else {
            p.hurt_cooldown = 20;
        }
        p.last_hurt = amount;
        if (type.armor) {
            wear_armor(p, applied, type.fire, harm == Harm::lava && wire.protocol() < 772);
            const auto armor = static_cast<float>(std::floor(attribute_value(p, gameplay::attributes::Attribute::armor)));
            const auto toughness = static_cast<float>(attribute_value(p, gameplay::attributes::Attribute::armor_toughness));
            applied = gameplay::damage::after_armor(applied, armor, toughness);
        }
        if (const auto level = resistance ? effect_amplifier(p, *resistance) : std::nullopt; level && type.resistance)
            applied = gameplay::damage::after_resistance(applied, *level);
        // Absorption takes the damage first.
        if (p.absorption > 0.0f) {
            const auto absorbed = std::min(p.absorption, applied);
            p.absorption -= absorbed;
            applied -= absorbed;
            p.absorption_dirty = true;
        }
        if (applied > 0.0f) exhaust(p, type.exhaustion);
        p.health = std::max(0.0f, p.health - applied);
        if (by) p.attacked = true; // the health told after the attack's wear (client/combat, client/shield)
        if (!fresh) {
            // A death this way makes no sound (not recorded).
            if (p.health <= 0.0f) die(p, harm, fall_distance, by);
            return true;
        }
        const auto by_id = by ? by->entity_id : -1;
        const auto event = wire.damage_event(p.entity_id, registry_index(registries, "minecraft:damage_type", type.name).value_or(0), by_id, by_id);
        p.peer->send(event);
        for (const auto viewer : p.seen_by)
            if (const auto o = players.find(viewer); o != players.end()) o->second.peer->send(event);
        if (by) {
            p.peer->send(wire.hurt_animation(p.entity_id, gameplay::damage::hurt_angle(by->pos.x - p.pos.x, by->pos.z - p.pos.z, p.yaw)));
            knock(p, 0.4, p.pos.x - by->pos.x, p.pos.z - by->pos.z);
        }
        // Then the others nearby hear the hurt or the death; the hurt player's
        // velocity is told at the next entity update (client/effects_heal),
        // but for drowning (client/drown).
        if (type.impact) p.hurt_marked = true;
        if (p.health <= 0.0f) {
            sound_around(p, Wire::PlayerSound::death, voice_pitch());
            die(p, harm, fall_distance, by);
            return true;
        }
        sound_around(p, type.sound, voice_pitch());
        return true;
    }
    // Armor worn down by a hurt: each piece a quarter of the damage, at least
    // one (minecraft.wiki "Durability"; client/fire), by the rules in the
    // hurt's tick; its slots are told at the next tick, and so is what others
    // see of it unless `seen_now` (1.21.4 shows a lava hurt's wear to others
    // in its tick, before the burning's flag: client/fire).
    void wear_armor(Player& p, float amount, bool fire, bool seen_now) {
        bool worn = false;
        for (std::uint8_t slot = 5; slot <= 8 && !worn; ++slot)
            worn = gameplay::equipment::worn_down(slot_of(p, slot), 1, fire).has_value();
        if (!worn) return;
        admit(contracts::WearArmor{key_of(p), std::min(gameplay::damage::armor_wear(amount), contracts::max_armor_wear), fire});
        hold_wear(p, 0b1'1110'0000, seen_now);
    }
    // Holds back the slots worn in this tick (a bit each) for the next.
    void hold_wear(Player& p, std::uint64_t slots, bool seen_now) {
        if (p.wear_tick != tick_count) {
            p.wear_seen = seen_now;
            p.wear_slots = 0;
        } else {
            p.wear_seen = p.wear_seen && seen_now;
        }
        p.wear_slots |= slots;
        p.wear_tick = tick_count;
    }
    bool wear_held(const Player& p) const { return p.wear_tick == tick_count && !p.wear_seen; }
    // The wear held back in the last tick goes after its velocity, before
    // the players' tick: every player's own slots, then what others see of
    // them (client/fire: before the extinguish sound of water a command set
    // as the hurt came; client/combat: the target's armor slot before the
    // attacker's weapon others see).
    void release_wear() {
        const auto held = [&](const Player& p) { return p.wear_tick && *p.wear_tick + 1 == tick_count; };
        for (auto& [id, p] : players)
            if (held(p) && !p.window) sync_inventory(p);
        for (auto& [id, p] : players) {
            if (!held(p) || p.wear_seen || !p.equipment_known) continue;
            sound_equipment(p); // a piece put on in the held tick (not recorded)
            show_equipment(p, equipment_now(p));
        }
    }
    // The velocity of the players hurt since the last entity update, to
    // them and to those who see them: the server's, a player standing still
    // (client/effects_heal), or at the bottom of lava (client/fire; a hurt
    // player in the air, moving or in water was not recorded).
    // A velocity an impulse (a knockback or a jump) changed from the last
    // told, to those who see the player (client/combat: a jump's (0, 0.42,
    // 0) to the others only; an attacked player's, put back after the attack
    // told the knockback, the first time), before the tick's entity data (a
    // sprint's knockback before the attacker's sprint ending).
    void tell_impulses() {
        for (auto& [id, p] : players) {
            if (!p.impulse || (p.dead && p.dead_ticks >= 20)) continue;
            const auto v = *p.impulse;
            const double dx = v.x - p.told_velocity.x, dy = v.y - p.told_velocity.y, dz = v.z - p.told_velocity.z;
            const double change = dx * dx + dy * dy + dz * dz;
            if (change <= 1.0e-7 && !(change > 0.0 && v.x == 0.0 && v.y == 0.0 && v.z == 0.0)) continue;
            p.told_velocity = v;
            const auto packet = wire.entity_motion(p.entity_id, v);
            for (const auto viewer : p.seen_by)
                if (const auto o = players.find(viewer); o != players.end()) o->second.peer->send(packet);
        }
    }
    // A hurt player's velocity (the impulse's, else a standing player's), to
    // the player and those who see them; the impulse is spent.
    void tell_hurt_velocities() {
        for (auto& [id, p] : players) {
            const auto impulse = std::exchange(p.impulse, std::nullopt);
            const bool marked = std::exchange(p.hurt_marked, false);
            if (!marked || (p.dead && p.dead_ticks >= 20)) continue; // the body went
            const auto motion = wire.entity_motion(p.entity_id, impulse.value_or(standing_velocity(p)));
            p.peer->send(motion);
            for (const auto viewer : p.seen_by)
                if (const auto o = players.find(viewer); o != players.end()) o->second.peer->send(motion);
        }
    }
    // The velocity the server keeps for a player standing (client/fire,
    // client/drown): in lava at the player's last tick, as the player stands
    // at its bottom (also when a command took the lava away since); none
    // after a teleport or a move through a sweet berry bush since the
    // player's last tick; else falling onto the ground. An impulse since
    // replaces it.
    static Vec3 standing_velocity(const Player& p) { return {0.0, p.stopped ? 0.0 : p.tick_in_lava ? -0.02 : -0.0784, 0.0}; }
    static Vec3 velocity_of(const Player& p) { return p.impulse.value_or(standing_velocity(p)); }
    // A knockback (client/combat): `strength`, less the knockback
    // resistance, along (x, z), from the player's velocity; an impulse.
    void knock(Player& p, double strength, double x, double z) {
        strength *= 1.0 - attribute_value(p, gameplay::attributes::Attribute::knockback_resistance);
        if (strength <= 0.0) return;
        const auto before = velocity_of(p);
        const auto after = gameplay::damage::knock({before.x, before.y, before.z}, strength, x, z, p.on_ground);
        p.impulse = Vec3{after.x, after.y, after.z};
    }
    void heal(Player& p, float amount) {
        if (!p.dead) p.health = std::min(max_health(p), p.health + amount);
    }

    // Death: the inventory drops (a command, so that the rules account for
    // every item), the client shows the death screen and everyone reads the
    // death message.
    void die(Player& p, Harm harm, double fall_distance, const Player* by = nullptr) {
        p.dead = true;
        p.dead_ticks = 0;
        p.camera.reset(); // the death screen replaces the view; a respawn has its own
        p.digging.reset();
        stop_use(p);
        p.floating = false;
        p.fall_distance = 0.0;
        // The window closes without telling the client (the death screen
        // replaces it), as on the official servers; the death drops an open
        // crafting table's grid with the rest.
        p.window.reset();
        admit(contracts::PlayerDeath{key_of(p), contracts::Vec3{p.pos.x, p.pos.y + 1.32, p.pos.z}});
        const auto key = harm == Harm::fall && fall_distance > 5.0 ? "death.fell.accident.generic" : harm_type(harm).death;
        auto with = nbt::Tag::list(nbt::TagType::compound);
        with.push(display_name(p));
        if (by) with.push(display_name(*by)); // death.attack.player: the one hurt, then the attacker (client/combat)
        auto text = nbt::Tag::compound();
        text.set("with", std::move(with));
        text.set("translate", nbt::Tag::string(key));
        p.peer->send(wire.combat_kill(p.entity_id, text));
        broadcast(wire.system_chat(text));
        log_info(std::format("{} died ({})", p.name, key));
        // The death animation, for the player and those who see them; the
        // body stays in their view until the player respawns.
        const auto dying = wire.entity_event(p.entity_id, 3);
        p.peer->send(dying);
        for (const auto viewer : p.seen_by)
            if (const auto o = players.find(viewer); o != players.end()) o->second.peer->send(dying);
    }
    // A player's name in messages, as both official servers write it
    // (client/effects_heal): a click suggests "/tell <name> ", a hover shows
    // the player entity, a shift-click inserts the name; 1.21.5 and later
    // write the events' new form. The keys come in the official servers' order.
    nbt::Tag display_name(const Player& p) const {
        auto uuid = nbt::Tag::int_array({static_cast<std::int32_t>(p.uuid.high >> 32), static_cast<std::int32_t>(p.uuid.high),
                                         static_cast<std::int32_t>(p.uuid.low >> 32), static_cast<std::int32_t>(p.uuid.low)});
        auto name = nbt::Tag::compound();
        if (wire.protocol() >= 772) {
            auto click = nbt::Tag::compound();
            click.set("action", nbt::Tag::string("suggest_command"));
            click.set("command", nbt::Tag::string(std::format("/tell {} ", p.name)));
            auto hover = nbt::Tag::compound();
            hover.set("name", nbt::Tag::string(p.name));
            hover.set("action", nbt::Tag::string("show_entity"));
            hover.set("id", nbt::Tag::string("minecraft:player"));
            hover.set("uuid", std::move(uuid));
            name.set("click_event", std::move(click));
            name.set("insertion", nbt::Tag::string(p.name));
            name.set("text", nbt::Tag::string(p.name));
            name.set("hover_event", std::move(hover));
        } else {
            auto contents = nbt::Tag::compound();
            contents.set("name", nbt::Tag::string(p.name));
            contents.set("id", std::move(uuid));
            contents.set("type", nbt::Tag::string("minecraft:player"));
            auto hover = nbt::Tag::compound();
            hover.set("contents", std::move(contents));
            hover.set("action", nbt::Tag::string("show_entity"));
            auto click = nbt::Tag::compound();
            click.set("action", nbt::Tag::string("suggest_command"));
            click.set("value", nbt::Tag::string(std::format("/tell {} ", p.name)));
            name.set("hoverEvent", std::move(hover));
            name.set("clickEvent", std::move(click));
            name.set("insertion", nbt::Tag::string(p.name));
            name.set("text", nbt::Tag::string(p.name));
        }
        return name;
    }

    // 20 ticks after a death the body goes, as on both official servers
    // (client/player_data: a player on the death screen): its last entity
    // event (60), to the player and those who see the body; then it leaves
    // their view, and the others leave the dead player's.
    void body_goes(Player& p) {
        const auto event = wire.entity_event(p.entity_id, 60);
        p.peer->send(event);
        for (const auto viewer : p.seen_by)
            if (const auto o = players.find(viewer); o != players.end()) o->second.peer->send(event);
        const auto body = wire.remove_entities({p.entity_id});
        for (const auto viewer : p.seen_by)
            if (const auto o = players.find(viewer); o != players.end()) o->second.peer->send(body);
        p.seen_by.clear();
        std::vector<std::int32_t> seen;
        for (auto& [oid, o] : players)
            if (o.seen_by.erase(p.peer->id())) seen.push_back(o.entity_id);
        if (!seen.empty()) p.peer->send(wire.remove_entities(seen));
    }

    // Respawning at the world spawn with full health and hunger.
    void respawn(Player& p) {
        p.dead = false;
        // A respawned player has no effects; the respawn resets the client's.
        // They keep the attributes they had, without modifiers (the game
        // mode's come back at the end of the tick: finish_arrival).
        p.effects.clear();
        p.effect_view = {};
        p.effects_dirty = false;
        p.absorption_dirty = false;
        p.effects_decided.reset();
        p.absorption = 0.0f;
        p.sprinting = false;
        for (auto& i : p.attributes) i.modifiers.clear();
        p.attributes_changed.clear();
        p.attributes_decided.clear();
        p.arrival = Player::Arrival::again;
        // Those the client saw go before the respawn, and appear again after
        // it; those who saw the player see the body go and the player appear
        // again (client/effects_heal).
        std::vector<std::int32_t> seen;
        for (auto& [oid, o] : players)
            if (o.seen_by.erase(p.peer->id())) seen.push_back(o.entity_id);
        if (!seen.empty()) p.peer->send(wire.remove_entities(seen));
        const auto body = wire.remove_entities({p.entity_id});
        for (const auto viewer : p.seen_by)
            if (const auto o = players.find(viewer); o != players.end()) o->second.peer->send(body);
        p.seen_by.clear();
        p.health = 20.0f;
        p.food = 20;
        p.saturation = 5.0f;
        p.exhaustion = 0.0f;
        p.food_timer = 0;
        p.hurt_cooldown = 0;
        p.last_hurt = 0.0f;
        p.hurt_marked = false;
        p.impulse.reset();
        p.told_velocity = {}; // a new entity's (client/combat)
        p.attacked = false;
        p.attack_ticks = 0;
        p.swing_ticks.reset();
        p.fire_ticks = -20;
        p.in_fire = false;
        p.burning = false;
        p.air = p.told_air = gameplay::damage::max_air;
        p.frozen_ticks = p.told_frozen = 0;
        p.in_snow = false;
        p.forced_pose.reset();
        p.fall_distance = 0.0;
        p.floating = false;
        p.floating_ticks = 0;
        p.living = p.told_living = 0; // a new entity's
        p.equipped = p.attributed = {};
        p.equipment_known = false;
        // A new entity: its first data, then its attributes (without
        // modifiers), at the next entity update, the health after them
        // (client/player_data, client/attribute_join).
        p.fresh = true;
        p.told_health.reset();
        p.told_skin_parts = 0;
        p.told_main_arm = 1;
        p.respawn_attributes = true;
        p.dead_ticks = 0;
        // The abilities, the respawn, the position, the spawn (twice), the
        // operator level and its commands, the view's center and the
        // inventory, as both official servers send them (client/player_data;
        // they send no held slot, and the difficulty, world border and time
        // again, which Circuit does not).
        Wire::JoinGame j{p.entity_id, config.max_players, p.view_distance, config.simulation_distance, dimension_type, p.game_mode};
        p.peer->send(wire.player_abilities(abilities(p.game_mode), 0.05f, 0.1f));
        p.peer->send(wire.respawn(j));
        p.flying = p.game_mode == spectator;
        p.remote_window = -1;
        // The client may have dropped its chunks: send the view again.
        p.sent.clear();
        const auto to = spawn_point();
        const auto chunk = chunk_at(to);
        p.center = chunk;
        move_tickets(p, chunk);
        teleport(p, to);
        const auto spawn = wire.spawn_position(wire_pos({meta.spawn.x, meta.spawn.y + 1, meta.spawn.z}), 0.0f);
        p.peer->send(spawn);
        p.peer->send(spawn);
        p.peer->send(wire.game_event(13, 0.0f));
        p.peer->send(wire.entity_event(p.entity_id, 28));
        p.peer->send(wire.empty_commands());
        p.peer->send(wire.chunk_center(chunk.x, chunk.z));
        sync_inventory(p);
        p.moved = true;
    }

    // ---- equipment ----------------------------------------------------------
    // As both official servers show it (client/armor, client/armor2): after
    // the rules' tick (and in a tick without the world, for the hotbar slot
    // held), what each player holds and wears is looked at. A piece put on in
    // an armor slot (not taken off) plays its sound to the player and the
    // others within 16 blocks, before the tick's level events; then the
    // armor's and the main hand weapon's attribute modifiers follow (sent at
    // the next tick's start), and those who see the player get the slots that
    // changed, or entity event 55 for the hands swapped.
    std::array<contracts::ItemStack, gameplay::equipment::slot_count> equipment_now(const Player& p) const {
        std::array<contracts::ItemStack, gameplay::equipment::slot_count> now;
        for (std::size_t k = 0; k < now.size(); ++k)
            now[k] = slot_of(p, gameplay::equipment::inventory_slot(static_cast<gameplay::equipment::Slot>(k), static_cast<std::uint8_t>(p.held)));
        return now;
    }
    void equipment_sounds() {
        for (auto& [id, p] : players)
            if (p.equipment_known && !wear_held(p)) sound_equipment(p);
    }
    // The equip sounds of the pieces put on since the equipment was last seen.
    void sound_equipment(Player& p) {
        using gameplay::equipment::Slot;
        const auto now = equipment_now(p);
        const auto& was = p.equipped;
        // A shield put into the off hand (client/shield: equip_generic; not
        // in the main hand), before the armor's.
        static const auto shield = gameplay::items::by_name("minecraft:shield");
        if (const auto& o = now[std::size_t(Slot::offhand)], &w = was[std::size_t(Slot::offhand)];
            shield && !o.empty() && o.item == *shield && (w.empty() || w.item != o.item))
            sound_around_at(p, Wire::PlayerSound::equip_generic, 1.0f, 1.0f, Wire::SoundCategory::player, true);
        for (std::size_t k = std::size_t(Slot::feet); k < now.size(); ++k) {
            if (now[k] == was[k] || now[k].empty() || (!was[k].empty() && was[k].item == now[k].item)) continue;
            if (const auto* worn = gameplay::equipment::wearable(now[k].item); worn && worn->slot == static_cast<Slot>(k))
                sound_around_at(p, equip_sound(worn->sound), 1.0f, 1.0f, Wire::SoundCategory::player, true);
        }
    }
    void look_at_equipment() {
        for (auto& [id, p] : players) {
            if (!wear_held(p)) show_equipment(p, equipment_now(p));
            else if (p.equipment_known) attribute_items(p, equipment_now(p));
        }
    }
    // While the wear is held back, the modifiers of the slots holding
    // another item (a weapon switched away in the attack's tick), at once;
    // the wear itself (the same item worn) follows at the next tick, as the
    // official servers send it (client/combat: the sword's attack speed in a
    // packet after the sprint's ending).
    void attribute_items(Player& p, const std::array<contracts::ItemStack, gameplay::equipment::slot_count>& now) {
        for (std::size_t k = 0; k < now.size(); ++k)
            if (now[k].empty() != p.attributed[k].empty() || now[k].item != p.attributed[k].item) {
                equipment_attributes(p, static_cast<gameplay::equipment::Slot>(k), now[k]);
                p.attributed[k] = now[k];
            }
    }
    // The modifiers of the slots changed since they were last followed.
    void attribute_equipment(Player& p, const std::array<contracts::ItemStack, gameplay::equipment::slot_count>& now) {
        for (std::size_t k = 0; k < now.size(); ++k)
            if (!(now[k] == p.attributed[k])) equipment_attributes(p, static_cast<gameplay::equipment::Slot>(k), now[k]);
        p.attributed = now;
    }
    void show_equipment(Player& p, const std::array<contracts::ItemStack, gameplay::equipment::slot_count>& now) {
        using gameplay::equipment::Slot;
        if (!p.equipment_known) {
            p.equipment_known = true;
            for (std::size_t k = 0; k < now.size(); ++k) equipment_attributes(p, static_cast<Slot>(k), now[k]);
            p.equipped = p.attributed = now;
            return;
        }
        attribute_equipment(p, now);
        if (now == p.equipped) return;
        const auto& was = p.equipped;
        const bool swapped = !(now[0] == was[0]) && now[0] == was[1] && now[1] == was[0];
        std::vector<std::pair<std::uint8_t, WireStack>> changed;
        for (std::size_t k = swapped ? 2 : 0; k < now.size(); ++k)
            if (!(now[k] == was[k])) changed.emplace_back(static_cast<std::uint8_t>(k), wire_stack(now[k]));
        for (const auto viewer : p.seen_by)
            if (const auto v = players.find(viewer); v != players.end()) {
                if (swapped) v->second.peer->send(wire.entity_event(p.entity_id, 55));
                if (!changed.empty()) v->second.peer->send(wire.set_equipment(p.entity_id, changed));
            }
        p.equipped = now;
    }
    static Wire::PlayerSound equip_sound(gameplay::equipment::EquipSound s) {
        using enum gameplay::equipment::EquipSound;
        switch (s) {
        case leather: return Wire::PlayerSound::equip_leather;
        case chain: return Wire::PlayerSound::equip_chain;
        case iron: return Wire::PlayerSound::equip_iron;
        case gold: return Wire::PlayerSound::equip_gold;
        case diamond: return Wire::PlayerSound::equip_diamond;
        case netherite: return Wire::PlayerSound::equip_netherite;
        case turtle: return Wire::PlayerSound::equip_turtle;
        case elytra: return Wire::PlayerSound::equip_elytra;
        default: return Wire::PlayerSound::equip_generic;
        }
    }
    // The modifiers of what a slot holds now, the old ones taken away: a
    // weapon's or tool's attack damage and speed in the main hand, a piece
    // of armor's armor and toughness (netherite's knockback resistance too)
    // in its armor slot, each added again, last, when the slot changed.
    void equipment_attributes(Player& p, gameplay::equipment::Slot s, const contracts::ItemStack& now) {
        using gameplay::attributes::Attribute;
        using gameplay::attributes::Operation;
        using gameplay::equipment::Slot;
        if (s == Slot::offhand) return;
        if (s == Slot::mainhand) {
            if (const auto w = now.empty() ? std::nullopt : gameplay::equipment::weapon(now.item)) {
                add_modifier(p, Attribute::attack_damage, {"base_attack_damage", static_cast<double>(w->damage), Operation::add_value}, true);
                add_modifier(p, Attribute::attack_speed, {"base_attack_speed", static_cast<double>(w->speed), Operation::add_value}, true);
            } else {
                remove_modifier(p, Attribute::attack_damage, "base_attack_damage");
                remove_modifier(p, Attribute::attack_speed, "base_attack_speed");
            }
            return;
        }
        const auto id = gameplay::equipment::armor_modifier(s);
        const auto* worn = now.empty() ? nullptr : gameplay::equipment::wearable(now.item);
        const bool armor = worn && worn->slot == s && (worn->armor > 0.0f || worn->toughness > 0.0f);
        if (armor) {
            add_modifier(p, Attribute::armor, {id, static_cast<double>(worn->armor), Operation::add_value}, true);
            add_modifier(p, Attribute::armor_toughness, {id, static_cast<double>(worn->toughness), Operation::add_value}, true);
        } else {
            remove_modifier(p, Attribute::armor, id);
            remove_modifier(p, Attribute::armor_toughness, id);
        }
        if (armor && worn->knockback_resistance > 0.0f)
            add_modifier(p, Attribute::knockback_resistance, {id, static_cast<double>(worn->knockback_resistance), Operation::add_value}, true);
        else
            remove_modifier(p, Attribute::knockback_resistance, id);
    }

    // ---- status effects -----------------------------------------------------
    // As both official servers show them (client/effects, client/effect_rules,
    // client/effect_particles, client/effect_time, client/effect_edges): each
    // effect added, changed or ending goes to the player at once; the entity
    // data they decide is decided at the end of the tick and goes to the
    // player and to whoever sees them at the start of the next. The official
    // servers list effects in an order that varies from run to run; here
    // they come in id order.

    // An effect's level as the client is told it: flags 1 ambient, 2
    // particles, 4 icon (particles hidden hide the icon too), 8 a new effect.
    void send_effect(Player& p, const gameplay::effects::Effect& e, bool added) {
        const auto& l = e.levels.front();
        const auto flags = static_cast<std::uint8_t>((l.ambient ? 1 : 0) | (l.particles ? 2 : 0) | (l.icon ? 4 : 0) | (added ? 8 : 0));
        p.peer->send(wire.update_mob_effect(p.entity_id, e.id, l.amplifier, l.duration, flags));
    }
    // Gives a level of an effect; returns whether it changed what the player has.
    bool give_effect(Player& p, std::uint8_t id, const gameplay::effects::Level& level) {
        auto it = std::ranges::lower_bound(p.effects, id, {}, &gameplay::effects::Effect::id);
        if (it == p.effects.end() || it->id != id) {
            it = p.effects.insert(it, {id, {level}});
            send_effect(p, *it, true);
            // The omens are heard when they come (client/eat: an ominous
            // bottle's bad omen, to the player and the others nearby).
            static const std::array omens{gameplay::effects::id("bad_omen"), gameplay::effects::id("trial_omen"), gameplay::effects::id("raid_omen")};
            if (const auto k = std::ranges::find(omens, std::optional<std::uint8_t>(id)); k != omens.end()) {
                constexpr std::array sounds{Wire::PlayerSound::bad_omen, Wire::PlayerSound::trial_omen, Wire::PlayerSound::raid_omen};
                sound_around_at(p, sounds[static_cast<std::size_t>(k - omens.begin())], 1.0f, 1.0f, Wire::SoundCategory::player, true);
            }
        } else if (gameplay::effects::give(*it, level)) {
            send_effect(p, *it, false);
        } else {
            return false;
        }
        p.effects_dirty = true;
        effect_started(p, id);
        return true;
    }
    // What an effect's shown level does when it is added, changed,
    // lengthened or comes back: its modifier, and absorption's absorption
    // (raised to 4 per level: client/attribute_rules).
    void effect_started(Player& p, std::uint8_t id) {
        effect_attributes(p, id);
        static const auto absorption = gameplay::effects::id("absorption");
        if (id != absorption) return;
        const auto it = std::ranges::find(p.effects, id, &gameplay::effects::Effect::id);
        const auto amount = 4.0f * static_cast<float>(it->levels.front().amplifier + 1);
        if (p.absorption < amount) {
            p.absorption = amount;
            p.effects_dirty = true;
        }
    }
    // Removes one effect (with the levels underneath it) or every effect;
    // returns whether there was any.
    bool clear_effects(Player& p, std::optional<std::uint8_t> id) {
        bool any = false;
        for (auto it = p.effects.begin(); it != p.effects.end();) {
            if (id && it->id != *id) {
                ++it;
                continue;
            }
            p.peer->send(wire.remove_mob_effect(p.entity_id, it->id));
            const auto removed = it->id;
            it = p.effects.erase(it);
            effect_attributes(p, removed);
            any = true;
        }
        p.effects_dirty = p.effects_dirty || any;
        return any;
    }
    // One tick of every effect: one ending shows the level underneath it,
    // or is removed (after showing a level underneath that ran out); one
    // whose time left is a multiple of 600 ticks is sent again (after the
    // level underneath, when both).
    void tick_effects(Player& p) {
        static const auto absorption = gameplay::effects::id("absorption");
        for (auto it = p.effects.begin(); it != p.effects.end();) {
            // Absorption ends once its absorption is used up (client/eat:
            // removed when poison's damage took the last of it).
            if (it->id == absorption && p.absorption <= 0.0f) {
                p.peer->send(wire.remove_mob_effect(p.entity_id, it->id));
                const auto removed = it->id;
                it = p.effects.erase(it);
                effect_attributes(p, removed);
                p.effects_dirty = true;
                continue;
            }
            // What it does this tick, by the time its shown level has left
            // before it counts down (an infinite one by the player's age).
            if (const auto& shown = it->levels.front(); shown.duration != 0) {
                const auto time = shown.duration == gameplay::effects::infinite ? static_cast<std::int32_t>(p.ticks_alive % 0x7FFFFFFF)
                                                                                 : shown.duration;
                if (gameplay::effects::acts(it->id, time, shown.amplifier)) effect_acts(p, it->id, shown.amplifier);
            }
            const auto t = gameplay::effects::tick(*it);
            if (t.next) send_effect(p, *it, false);
            if (t.ended) {
                p.peer->send(wire.remove_mob_effect(p.entity_id, it->id));
                const auto removed = it->id;
                it = p.effects.erase(it);
                effect_attributes(p, removed);
                p.effects_dirty = true;
                continue;
            }
            if (t.next) effect_started(p, it->id);
            if (t.again) send_effect(p, *it, false);
            p.effects_dirty = p.effects_dirty || t.next || t.again;
            ++it;
        }
    }
    // An effect that heals, hurts or feeds, in a tick it acts (client/effects_heal,
    // both versions): instant health heals 4 and instant damage hurts 6
    // (magic), doubled per level; regeneration heals 1 below the maximum
    // health; poison hurts 1 (magic) above 1 health; wither hurts 1 (wither);
    // hunger tires by 0.005 per level; saturation gives 1 food and 2
    // saturation per level (the saturation up to the food).
    void effect_acts(Player& p, std::uint8_t id, std::int32_t amplifier) {
        const auto doubled = [&](float base) { return std::ldexp(base, std::clamp(amplifier, 0, 127)); };
        switch (id) {
        case 5: heal(p, doubled(4.0f)); break;
        case 6: hurt(p, doubled(6.0f), Harm::magic); break;
        case 9:
            if (p.health < max_health(p)) heal(p, 1.0f);
            break;
        case 18:
            if (p.health > 1.0f) hurt(p, 1.0f, Harm::magic);
            break;
        case 19: hurt(p, 1.0f, Harm::wither); break;
        case 16: exhaust(p, 0.005f * static_cast<float>(amplifier + 1)); break;
        case 22: {
            const auto n = std::clamp(amplifier + 1, 0, 20);
            p.food = std::min(p.food + n, 20);
            p.saturation = std::clamp(p.saturation + static_cast<float>(n) * 2.0f, 0.0f, static_cast<float>(p.food));
            break;
        }
        default: break;
        }
    }

    // ---- eating and drinking ------------------------------------------------
    // As both official servers run them (client/eat): a use of food or drink
    // starts with the living flags (to the player and those who see them)
    // and lasts its ticks, the others nearby hearing it; then the rules use
    // the item up (ConsumeItem) and, once applied, the player gets what it
    // gives. Letting go, another item in the hand, another hotbar slot,
    // spectating or death end it before.

    // A use starts. Food waits for hunger, but in creative mode and for what
    // is always eaten (refused at full hunger in survival).
    void start_use(Player& p, std::int32_t hand, std::uint8_t slot, const gameplay::food::Consumable& c) {
        if (c.food() && !c.always && p.game_mode != creative && p.food >= 20) return;
        const auto stack = slot_of(p, slot);
        if (p.cooldowns.contains(stack.item)) return;
        p.using_item = Player::Using{hand, slot, stack, c.ticks, c.ticks, std::nullopt};
        p.living = static_cast<std::uint8_t>(1 | (hand == 1 ? 2 : 0));
    }
    // A shield raised (client/shield: the living flags, 1 and the off hand's
    // 2; none while its cooldown lasts), blocking once raised 5 ticks
    // (minecraft.wiki "Shield"; recorded: 2 ticks none, 12 blocked).
    static constexpr std::int32_t shield_raise = 72000;
    void raise_shield(Player& p, std::int32_t hand, std::uint8_t slot) {
        const auto stack = slot_of(p, slot);
        if (p.cooldowns.contains(stack.item)) return;
        p.using_item = Player::Using{hand, slot, stack, shield_raise, shield_raise, std::nullopt, true};
        p.living = static_cast<std::uint8_t>(1 | (hand == 1 ? 2 : 0));
    }
    // Raised 5 ticks and still there: the slot holding it, a main-hand
    // shield's slot still the main hand (a shield swapped, dropped or
    // switched away blocks no more, before its use ends at the tick).
    bool blocking(const Player& p) const {
        if (!p.using_item || !p.using_item->shield || p.using_item->ticks - p.using_item->left < 5) return false;
        const auto& u = *p.using_item;
        const auto now = slot_of(p, u.slot);
        return !now.empty() && now.item == u.stack.item && (u.hand == 1 || u.slot == main_hand(p));
    }
    // Whether what hurts stands before a player (the half of the horizontal
    // plane they face: minecraft.wiki "Shield"; client/shield: an attacker
    // before and behind).
    static bool facing(const Player& p, const Player& from) {
        const double yaw = p.yaw * std::numbers::pi / 180.0;
        return (from.pos.x - p.pos.x) * -std::sin(yaw) + (from.pos.z - p.pos.z) * std::cos(yaw) > 0.0;
    }
    static bool holds_axe(const contracts::ItemStack& held) {
        if (held.empty()) return false;
        const auto* info = gameplay::items::info(held.item);
        return info && std::string_view(info->name).ends_with("_axe");
    }
    // An attack a raised shield blocked (client/shield; both versions alike
    // but for the sounds and the disabling's damage event): an axe disables the shield for 100 ticks (its
    // cooldown told at once) and lowers it; 1.21.8 plays `item.shield.break`
    // (volume 0.8) and sends the damage event, 1.21.4 sends entity events 30
    // and 29; otherwise 1.21.8 plays `item.shield.block`, 1.21.4 sends entity
    // event 29 (the pitch between 0.8 and 1.2 by minecraft.wiki, Circuit's
    // draw). The shield wears by 1 and the damage (rounded down) from 3
    // damage (minecraft.wiki; recorded: 1 none, 6 and 9 worn), told at the
    // next tick; its holder is knocked 0.5 towards the
    // attacker, then 0.4 away (told to the others only); the hurt cooldown
    // starts as by a hurt of nothing; the others hear the hurt sound.
    void block(Player& p, float amount, const Player& by) {
        const auto u = *p.using_item;
        const bool v772 = wire.protocol() >= 772;
        const auto to_all = [&](const Bytes& packet) {
            p.peer->send(packet);
            for (const auto viewer : p.seen_by)
                if (const auto o = players.find(viewer); o != players.end()) o->second.peer->send(packet);
        };
        const bool disables = holds_axe(slot_of(by, main_hand(by)));
        if (disables) {
            p.cooldowns[u.stack.item] = tick_count.load() + 100;
            if (const auto* info = gameplay::items::info(u.stack.item)) p.peer->send(wire.cooldown(info->name, 100));
            if (v772) {
                sound_around_at(p, Wire::PlayerSound::shield_break, 0.8f, 0.8f + random_draw() * 0.4f, Wire::SoundCategory::player, true);
                to_all(wire.damage_event(p.entity_id, registry_index(registries, "minecraft:damage_type", "minecraft:player_attack").value_or(0),
                                         by.entity_id, by.entity_id));
            } else {
                to_all(wire.entity_event(p.entity_id, 30));
                to_all(wire.entity_event(p.entity_id, 29));
            }
        } else if (v772) {
            sound_around_at(p, Wire::PlayerSound::shield_block, 1.0f, 0.8f + random_draw() * 0.4f, Wire::SoundCategory::player, true);
        } else {
            to_all(wire.entity_event(p.entity_id, 29));
        }
        if (amount >= 3.0f && p.game_mode != creative) {
            admit(contracts::WearHeld{key_of(p), u.slot, u.stack.item, std::min(1 + static_cast<std::int32_t>(std::floor(amount)), contracts::max_held_wear)});
            hold_wear(p, std::uint64_t{1} << u.slot, false);
        }
        knock(p, 0.5, by.pos.x - p.pos.x, by.pos.z - p.pos.z);
        knock(p, 0.4, p.pos.x - by.pos.x, p.pos.z - by.pos.z);
        if (p.hurt_cooldown <= 10) {
            p.hurt_cooldown = 20;
            p.last_hurt = 0.0f;
        }
        if (disables) stop_use(p);
        sound_around(p, Wire::PlayerSound::hurt, voice_pitch());
    }
    // Cooldowns that ended, told as cooldowns of 0 ticks (client/eat).
    void end_cooldowns(Player& p) {
        for (auto it = p.cooldowns.begin(); it != p.cooldowns.end();) {
            if (it->second > tick_count.load()) {
                ++it;
                continue;
            }
            if (const auto* info = gameplay::items::info(it->first)) p.peer->send(wire.cooldown(info->name, 0));
            it = p.cooldowns.erase(it);
        }
    }
    // A tick of the use: its sound every fourth tick, and when its ticks run
    // out the item is used up.
    void tick_use(Player& p) {
        if (!p.using_item || p.using_item->consuming) return;
        auto& u = *p.using_item;
        const auto now = slot_of(p, u.slot);
        if (u.shield) {
            // Raised while it is held there, as eating ends (not recorded).
            if (now.empty() || now.item != u.stack.item || p.game_mode == spectator || (u.hand == 0 && u.slot != main_hand(p))) stop_use(p);
            else if (u.left > 0) --u.left, u.stack = now;
            return;
        }
        const auto* c = gameplay::food::of(now.item);
        if (!c || now.empty() || now.item != u.stack.item || p.game_mode == spectator || (u.hand == 0 && u.slot != main_hand(p))) {
            stop_use(p);
            return;
        }
        u.stack = now;
        --u.left;
        if (gameplay::food::use_sound(u.ticks, u.left)) {
            // Eating at volume 0.5 or 1, drinking at 0.5; the player's client plays its own.
            const auto volume = c->drink() ? 0.5f : (random_draw() < 0.5f ? 0.5f : 1.0f);
            sound_around_at(p, use_sound_of(*c), volume, voice_pitch(), Wire::SoundCategory::player, false);
        }
        if (u.left > 0) return;
        const contracts::Vec3 eye{p.pos.x, p.pos.y + eye_height(p), p.pos.z};
        if (const auto admission = queue.admit(contracts::ConsumeItem{key_of(p), u.slot, now, p.game_mode == creative, eye}, overworld)) {
            u.consuming = *admission;
        } else {
            log_warn("game: command queue full; input dropped");
            stop_use(p);
        }
    }
    static Wire::PlayerSound use_sound_of(const gameplay::food::Consumable& c) {
        switch (c.sound) {
        case gameplay::food::Sound::drink: return Wire::PlayerSound::drink;
        case gameplay::food::Sound::honey_drink: return Wire::PlayerSound::honey_drink;
        default: return Wire::PlayerSound::eat;
        }
    }
    void sound_around_at(const Player& p, Wire::PlayerSound sound, float volume, float pitch, Wire::SoundCategory category, bool self,
                         std::optional<Vec3> where = std::nullopt) {
        sound_near(p, wire.player_sound(sound, where.value_or(p.pos), volume, pitch, static_cast<std::int64_t>(sound_seed++), category), self, where);
    }
    // A use ends: the off hand's flag stays (client/eat: 3, then 2).
    static void stop_use(Player& p) {
        p.using_item.reset();
        p.living &= static_cast<std::uint8_t>(~1u);
    }
    // The living flags when they changed, to the player and those who see
    // them, at once: a use's start before its first tick's sounds, its end
    // after the health packet and before the slot (client/eat).
    void send_living(Player& p) {
        const auto flags = p.living;
        if (flags == p.told_living) return;
        p.told_living = flags;
        Wire::PlayerData d;
        d.living = flags;
        const auto data = wire.player_data(p.entity_id, d);
        p.peer->send(data);
        for (const auto viewer : p.seen_by)
            if (const auto v = players.find(viewer); v != players.end()) v->second.peer->send(data);
    }
    // After the rules' tick, the uses whose items they used up finish.
    void finish_uses() {
        const auto& trace = engine->trace();
        for (auto& [id, p] : players) {
            if (!p.using_item || !p.using_item->consuming) continue;
            const auto admission = *p.using_item->consuming;
            const auto stack = p.using_item->stack;
            stop_use(p);
            const auto r = std::ranges::find_if(trace, [&](const contracts::TraceRecord& t) {
                return (t.kind == contracts::TraceKind::command_applied || t.kind == contracts::TraceKind::command_rejected) && t.a == admission;
            });
            if (r != trace.end() && r->kind == contracts::TraceKind::command_applied && !p.dead) consumed(p, stack);
            send_living(p);
        }
    }
    // An item eaten or drunk, in the order both official servers act: the
    // finished use's event (to the player alone), a suspicious stew's
    // effects, a food's sound and a burp (to everyone within 16 blocks), a
    // potion's effects, an ominous bottle's bad omen and its sound, what
    // milk, honey and a chorus fruit do, the item's own effects (each by
    // its chance), and its food; then the health packet (client/eat).
    void consumed(Player& p, const contracts::ItemStack& stack) {
        const auto* c = gameplay::food::of(stack.item);
        if (!c) return;
        p.peer->send(wire.entity_event(p.entity_id, 9));
        using Special = gameplay::food::Special;
        // An effect a stack names is given in the ranges the player's save
        // keeps (amplifier 0-255, a duration of a tick or more or infinite).
        const auto give = [&](std::int32_t effect, const gameplay::effects::Level& level) {
            if (effect < 0 || static_cast<std::size_t>(effect) >= gameplay::effects::count) return;
            if (level.amplifier < 0 || level.amplifier > 255 || (level.duration < 1 && level.duration != gameplay::effects::infinite)) return;
            give_effect(p, static_cast<std::uint8_t>(effect), level);
        };
        if (c->special == Special::stew)
            for (const auto& e : wire.components().stew_effects_of(stack.components)) give(e.effect, {e.amplifier, e.duration, e.ambient, e.particles, e.icon});
        if (c->food()) {
            sound_around_at(p, use_sound_of(*c), 1.0f, voice_pitch(), Wire::SoundCategory::neutral, true);
            sound_around_at(p, Wire::PlayerSound::burp, 0.5f, voice_pitch(), Wire::SoundCategory::player, true);
        }
        switch (c->special) {
        case Special::potion: {
            // The potion's effects, then those its contents add; instant
            // health heals and instant damage hurts at once.
            std::vector<protocol::ComponentCodec::ItemEffect> all;
            if (const auto id = wire.components().potion_of(stack.components))
                for (const auto& e : gameplay::potions::effects(*id))
                    if (const auto effect = gameplay::effects::id(e.effect)) all.push_back({*effect, e.amplifier, e.duration, false, true, true});
            for (const auto& e : wire.components().potion_effects_of(stack.components)) all.push_back(e);
            static const auto instant_health = gameplay::effects::id("instant_health");
            static const auto instant_damage = gameplay::effects::id("instant_damage");
            for (const auto& e : all) {
                if (e.amplifier < 0 || e.amplifier > 255) continue;
                const auto doubled = [&](float base) { return std::ldexp(base, std::clamp(e.amplifier, 0, 127)); };
                if (e.effect == instant_health) heal(p, doubled(4.0f));
                else if (e.effect == instant_damage) hurt(p, doubled(6.0f), Harm::magic);
                else give(e.effect, {e.amplifier, e.duration, e.ambient, e.particles, e.icon});
                if (p.dead) return;
            }
            break;
        }
        case Special::ominous: {
            const auto amplifier = std::clamp(wire.components().varint_of(stack.components, "ominous_bottle_amplifier").value_or(0), 0, 4);
            static const auto bad_omen = gameplay::effects::id("bad_omen");
            give(*bad_omen, {amplifier, 120000, false, false, true}); // its particles hidden (client/eat)
            sound_around_at(p, Wire::PlayerSound::ominous_bottle, 1.0f, 1.0f, Wire::SoundCategory::player, true);
            break;
        }
        case Special::clear_effects: clear_effects(p, std::nullopt); break;
        case Special::remove_poison: clear_effects(p, gameplay::effects::id("poison")); break;
        case Special::teleport: {
            chorus_teleport(p);
            // Every chorus fruit waits a second (minecraft.wiki "Chorus Fruit"; client/eat).
            constexpr std::int32_t cooldown = 20;
            p.cooldowns[stack.item] = tick_count.load() + cooldown;
            p.peer->send(wire.cooldown("minecraft:chorus_fruit", cooldown));
            break;
        }
        default: break;
        }
        for (const auto& e : c->effects) {
            if (e.probability < 1.0f && random_draw() >= e.probability) continue;
            if (const auto effect = gameplay::effects::id(e.effect)) give(*effect, {e.amplifier, e.duration, false, true, true});
        }
        if (c->food()) {
            p.food = std::min(p.food + c->nutrition, 20);
            p.saturation = std::min(p.saturation + c->saturation(), static_cast<float>(p.food));
        }
        send_vitals(p, true);
    }
    // A chorus fruit's teleport (minecraft.wiki "Chorus Fruit"): up to 16
    // tries at a random place within 8 blocks on each axis, inside the
    // world, let down onto the first block with collision below it; the
    // player goes to each try that finds such a block and stays at the first
    // where it fits out of fluids, or goes back. Both official servers sent
    // one to nine positions in a use (client/eat), failing more tries than
    // this does, by rules not determined; the places are Circuit's own
    // draws. Then the teleport's particles (entity event 46, to the player
    // and those who see them) and its sound where the player was.
    void chorus_teleport(Player& p) {
        const auto lookup = shapes();
        const auto bottom = static_cast<double>(bundle.world.min_y);
        const auto top = bottom + bundle.world.height - 1.0;
        const auto from = p.pos;
        bool moved = false;
        for (int attempt = 0; attempt < 16; ++attempt) {
            const double x = from.x + (random_draw() - 0.5) * 16.0;
            const double z = from.z + (random_draw() - 0.5) * 16.0;
            double y = std::clamp(std::floor(from.y) + std::floor(random_draw() * 16.0) - 8.0, bottom, top);
            const auto cell = [&](double at_y) {
                return BlockPos{static_cast<std::int32_t>(std::floor(x)), static_cast<std::int32_t>(std::floor(at_y)), static_cast<std::int32_t>(std::floor(z))};
            };
            while (y > bottom && lookup(cell(y - 1.0)).boxes.empty()) y -= 1.0;
            if (y <= bottom || !gameplay::in_world(bundle, cell(y - 1.0)) || !engine->view().block(overworld, cell(y - 1.0))) continue;
            teleport(p, {x, y, z});
            moved = true;
            const auto box = movement::player_box({x, y, z}, false);
            if (movement::collides(box, lookup) || fluid_in(box)) continue;
            p.fall_distance = 0.0;
            const auto event = wire.entity_event(p.entity_id, 46);
            p.peer->send(event);
            for (const auto viewer : p.seen_by)
                if (const auto v = players.find(viewer); v != players.end()) v->second.peer->send(event);
            sound_around_at(p, Wire::PlayerSound::chorus_teleport, 1.0f, 1.0f, Wire::SoundCategory::player, true, from);
            return;
        }
        if (moved) teleport(p, from);
    }

    // What the effects decide of the entity's data: the invisibility and
    // glowing flags, a particle for each effect showing particles, and
    // whether all of those are ambient (left as it was without effects).
    Player::EffectView effect_view(const Player& p) const {
        Player::EffectView v;
        v.absorption = p.absorption;
        v.ambient = p.effect_view.ambient;
        if (p.effects.empty()) return v;
        v.ambient = true;
        for (const auto& e : p.effects) {
            v.flags |= gameplay::effects::shared_flag(e.id);
            if (!e.levels.front().particles) continue;
            v.particles.push_back(gameplay::effects::particle(e.id));
            v.ambient = v.ambient && e.levels.front().ambient;
        }
        return v;
    }
    // The entity's data as last told, for a client that has not seen it:
    // the entries that differ from a new entity's (the shared flags hold
    // burning's, the effects', a spectator's, sprinting's and the shift
    // key's bits).
    Bytes shown_data(const Player& p) const {
        const auto& v = p.effect_view;
        Wire::PlayerData d;
        if (p.told_flags != 0) d.flags = p.told_flags;
        if (p.told_pose != 0) d.pose = p.told_pose;
        if (p.told_air != gameplay::damage::max_air) d.air = p.told_air;
        if (p.told_frozen != 0) d.frozen = p.told_frozen;
        if (p.told_living != 0) d.living = p.told_living;
        const auto health = p.told_health.value_or(p.health);
        if (health != 1.0f) d.health = health; // a new living entity's is 1
        if (!v.particles.empty()) d.particles = v.particles;
        if (v.ambient) d.ambient = true;
        if (v.absorption != 0.0f) d.absorption = v.absorption;
        if (p.told_skin_parts != 0) d.skin_parts = p.told_skin_parts;
        if (p.told_main_arm != 1) d.main_hand = p.told_main_arm;
        return wire.player_data(p.entity_id, d);
    }
    // At the end of a tick, the entity data of the effects changed in it,
    // or of the absorption alone.
    void decide_effects() {
        for (auto& [id, p] : players) {
            if (!p.effects_dirty && !p.absorption_dirty) continue;
            auto view = p.effects_dirty ? effect_view(p) : p.effect_view;
            view.absorption = p.absorption;
            p.effects_decided = Player::Decided{std::move(view), p.effects_dirty};
            p.effects_dirty = false;
            p.absorption_dirty = false;
        }
    }
    // At the start of the next, what changed of it: the flags, the ambience
    // and the absorption when they changed, the particles when effects
    // changed unless none were shown and none are (both official servers
    // send a list of particles again even when it is the same).
    // With them, at this entity update, the health, skin parts and main hand
    // when they changed since last told, in the same entity data
    // (client/player_data: the health after the health packet, to the player
    // and those who see them); a respawned player's first data instead.
    void send_decided_effects() {
        for (auto& [id, p] : players) {
            Wire::PlayerData d;
            if (p.fresh) {
                if (!p.vitals_pending) send_fresh_data(p); // a respawn's (a join's goes with its vitals)
            } else {
                if (p.told_health != p.health) d.health = p.health;
                if (p.told_skin_parts != p.skin_parts) d.skin_parts = p.skin_parts;
                if (p.told_main_arm != p.main_arm) d.main_hand = p.main_arm;
            }
            if (p.effects_decided) {
                const auto decided = std::move(*p.effects_decided);
                p.effects_decided.reset();
                const auto& now = decided.view;
                const auto& was = p.effect_view;
                if (decided.effects && !(now.particles.empty() && was.particles.empty())) d.particles = now.particles;
                if (now.ambient != was.ambient) d.ambient = now.ambient;
                if (now.absorption != was.absorption) d.absorption = now.absorption;
                p.effect_view = now;
            }
            // The shared flags (the effects' with a spectator's invisibility and
            // the shift key) and the pose (crouching), when they changed.
            if (!p.fresh) {
                if (const auto flags = shared_flags(p); flags != p.told_flags) d.flags = flags;
                if (const auto pose = pose_of(p); pose != p.told_pose) d.pose = pose;
                // The air supply and frozen ticks as the player's last tick
                // left them (client/drown).
                if (p.air != p.told_air) d.air = p.air;
                if (p.frozen_ticks != p.told_frozen) d.frozen = p.frozen_ticks;
            }
            if (!d.flags && !d.pose && !d.health && !d.particles && !d.ambient && !d.absorption && !d.skin_parts && !d.main_hand && !d.air
                && !d.frozen)
                continue;
            if (d.air) p.told_air = *d.air;
            if (d.frozen) p.told_frozen = *d.frozen;
            if (d.flags) p.told_flags = *d.flags;
            if (d.pose) p.told_pose = *d.pose;
            if (d.health) p.told_health = *d.health;
            if (d.skin_parts) p.told_skin_parts = *d.skin_parts;
            if (d.main_hand) p.told_main_arm = *d.main_hand;
            const auto data = wire.player_data(p.entity_id, d);
            p.peer->send(data);
            for (const auto viewer : p.seen_by)
                if (const auto v = players.find(viewer); v != players.end()) v->second.peer->send(data);
        }
    }
    // A player's shared flags: burning (client/fire), the effects'
    // (invisibility, glowing), a spectator's invisibility, sprinting
    // (client/combat) and the shift key (client/spectators). And its
    // pose: crouching while the shift key is down, but for a spectator or a
    // flyer.
    std::uint8_t shared_flags(const Player& p) const {
        return static_cast<std::uint8_t>(p.effect_view.flags | (p.game_mode == spectator ? 0x20 : 0) | (p.sprinting ? 0x08 : 0) | (p.sneaking ? 0x02 : 0)
                                         | (p.burning ? 0x01 : 0));
    }
    // A new entity's first data (joined or respawned): its health, its
    // absorption, the skin parts it shows and a left main hand (by the same
    // rule; not recorded), to the
    // player and those who see it, kept for those who see it appear later
    // in the tick (client/player_data, client/attribute_rules: a rejoin with
    // absorption).
    void send_fresh_data(Player& p) {
        p.fresh = false;
        Wire::PlayerData d;
        if (const auto flags = shared_flags(p); flags != 0) d.flags = flags;
        if (const auto pose = pose_of(p); pose != 0) d.pose = pose;
        if (p.air != gameplay::damage::max_air) d.air = p.air;
        if (p.frozen_ticks != 0) d.frozen = p.frozen_ticks;
        d.health = p.health;
        if (p.absorption > 0.0f) d.absorption = p.absorption;
        if (p.skin_parts != 0) d.skin_parts = p.skin_parts;
        if (p.main_arm != 1) d.main_hand = p.main_arm;
        p.told_health = p.health;
        p.told_skin_parts = p.skin_parts;
        p.told_main_arm = p.main_arm;
        p.told_flags = d.flags.value_or(0);
        p.told_pose = d.pose.value_or(0);
        p.told_air = d.air.value_or(gameplay::damage::max_air);
        p.told_frozen = d.frozen.value_or(0);
        const auto data = wire.player_data(p.entity_id, d);
        p.peer->send(data);
        for (const auto viewer : p.seen_by)
            if (const auto v = players.find(viewer); v != players.end()) v->second.peer->send(data);
        p.fresh_data = data;
        p.fresh_data_tick = tick_count;
        if (std::exchange(p.respawn_attributes, false)) send_attributes(p, p.attributes, true, false);
    }

    // A beacon's cycle gives its powers to the players in range (client/beacon,
    // client/beacon2, both versions): those but spectators whose box meets the
    // beacon's cell grown by 10 blocks per level and 10 more, and then by the
    // world's height upwards (a one-level beacon at y -59 reached a player at
    // y 345.5, not one at 346.5), get the primary power for 9 seconds and 2
    // per level, ambient, at amplifier 1 with four levels and the same
    // secondary, then with four levels another secondary at amplifier 0.
    void give_beacon_effects(BlockPos pos, std::uint8_t levels, std::uint8_t primary, std::uint8_t secondary) {
        if (primary == 0) return;
        const double range = 10.0 * levels + 10.0;
        const gameplay::physics::Aabb area{pos.x - range, pos.y - range, pos.z - range,
                                           pos.x + 1.0 + range, pos.y + 1.0 + range + bundle.world.height, pos.z + 1.0 + range};
        const gameplay::effects::Level level{levels >= 4 && primary == secondary ? 1 : 0, (9 + levels * 2) * 20, true, true, true};
        for (auto& [id, p] : players) {
            if (p.dead || p.vitals_pending || p.game_mode == spectator) continue;
            if (!box_of(p, p.pos).intersects(area)) continue;
            give_effect(p, static_cast<std::uint8_t>(primary - 1), level);
            if (levels >= 4 && secondary != 0 && secondary != primary)
                give_effect(p, static_cast<std::uint8_t>(secondary - 1), {0, level.duration, true, true, true});
        }
    }

    // ---- attributes ---------------------------------------------------------
    // As both official servers keep them on players and send them
    // (client/attributes, client/attribute_rules, client/attribute_join):
    // changed in a tick, they are decided at its end and go to the player
    // and to whoever sees them after the entity data at the start of the
    // next, each changed attribute with a player's base and all its
    // modifiers. The official servers list the attributes of one packet in
    // an order that varies from run to run; here they come in id order.

    gameplay::attributes::Instance& attribute(Player& p, gameplay::attributes::Attribute a) {
        auto it = std::ranges::lower_bound(p.attributes, a, {}, &gameplay::attributes::Instance::attribute);
        if (it == p.attributes.end() || it->attribute != a) it = p.attributes.insert(it, {a, {}});
        return *it;
    }
    double attribute_value(const Player& p, gameplay::attributes::Attribute a) const {
        const auto it = std::ranges::find(p.attributes, a, &gameplay::attributes::Instance::attribute);
        return it == p.attributes.end() ? gameplay::attributes::base(a) : gameplay::attributes::value(*it);
    }
    float max_health(const Player& p) const { return static_cast<float>(attribute_value(p, gameplay::attributes::Attribute::max_health)); }
    // A modifier added (the player has the attribute from then on), or
    // added again: then it moves to the end, also when unchanged (an effect
    // lengthened: client/attribute_rules), unless `again` is false.
    void add_modifier(Player& p, gameplay::attributes::Attribute a, const gameplay::attributes::Modifier& m, bool again = false) {
        auto& i = attribute(p, a);
        const auto it = std::ranges::find(i.modifiers, m.id, &gameplay::attributes::Modifier::id);
        if (it != i.modifiers.end()) {
            if (*it == m && !again) return;
            i.modifiers.erase(it);
        }
        i.modifiers.push_back(m);
        p.attributes_changed.insert(a);
    }
    void remove_modifier(Player& p, gameplay::attributes::Attribute a, std::string_view id) {
        const auto it = std::ranges::find(p.attributes, a, &gameplay::attributes::Instance::attribute);
        if (it == p.attributes.end()) return;
        if (std::erase_if(it->modifiers, [&](const auto& m) { return m.id == id; }) > 0) p.attributes_changed.insert(a);
    }
    // An effect's modifier as its shown level gives it, added again
    // whenever that level is (given, changed, lengthened or coming back; not
    // when the effect is only sent again), or none once the effect is gone.
    void effect_attributes(Player& p, std::uint8_t id) {
        const auto it = std::ranges::find(p.effects, id, &gameplay::effects::Effect::id);
        const auto any = gameplay::attributes::effect_modifier(id, 0);
        if (!any) return;
        if (it == p.effects.end()) return remove_modifier(p, any->attribute, any->modifier.id);
        const auto m = gameplay::attributes::effect_modifier(id, it->levels.front().amplifier);
        add_modifier(p, m->attribute, m->modifier, true);
    }
    // The effects on the player's digging: the stronger of haste and conduit
    // power, and mining fatigue (their shown levels).
    gameplay::DigEffects dig_effects(const Player& p) const {
        static const auto haste = gameplay::effects::id("haste"), conduit = gameplay::effects::id("conduit_power"),
                          fatigue = gameplay::effects::id("mining_fatigue");
        gameplay::DigEffects out;
        for (const auto& e : p.effects) {
            const auto amplifier = e.levels.front().amplifier;
            if (e.id == haste || e.id == conduit) out.haste = std::max(out.haste, amplifier);
            else if (e.id == fatigue) out.fatigue = amplifier;
        }
        return out;
    }
    // Creative mode reaches further.
    void game_mode_attributes(Player& p) {
        using gameplay::attributes::Attribute;
        if (p.game_mode == creative) {
            add_modifier(p, Attribute::block_interaction_range, gameplay::attributes::creative_block_range);
            add_modifier(p, Attribute::entity_interaction_range, gameplay::attributes::creative_entity_range);
        } else {
            remove_modifier(p, Attribute::block_interaction_range, gameplay::attributes::creative_block_range.id);
            remove_modifier(p, Attribute::entity_interaction_range, gameplay::attributes::creative_entity_range.id);
        }
    }
    void send_attributes(Player& p, std::span<const gameplay::attributes::Instance> list, bool to_player, bool to_viewers) {
        if (std::ranges::none_of(list, [](const auto& i) { return gameplay::attributes::synced(i.attribute); })) return;
        const auto packet = wire.update_attributes(p.entity_id, list);
        if (to_player) p.peer->send(packet);
        if (to_viewers)
            for (const auto viewer : p.seen_by)
                if (const auto v = players.find(viewer); v != players.end()) v->second.peer->send(packet);
    }
    // At the end of a tick, the attributes changed in it. A health or an
    // absorption above its new maximum is cut to it then (the health packet
    // follows the attributes, client/attribute_rules).
    void decide_attributes() {
        for (auto& [id, p] : players) {
            if (p.attributes_changed.empty()) continue;
            for (const auto a : p.attributes_changed) p.attributes_decided.push_back(attribute(p, a));
            p.attributes_changed.clear();
            if (p.health > max_health(p)) p.health = max_health(p);
            const auto most = static_cast<float>(attribute_value(p, gameplay::attributes::Attribute::max_absorption));
            if (p.absorption > most) {
                p.absorption = most;
                p.absorption_dirty = true;
            }
        }
    }
    // At the start of the next, after the entity data.
    void send_decided_attributes() {
        for (auto& [id, p] : players) {
            if (p.attributes_decided.empty()) continue;
            const auto list = std::move(p.attributes_decided);
            p.attributes_decided.clear();
            send_attributes(p, list, true, true);
        }
    }
    // The end of the tick a player joined or respawned in, after they
    // appeared to others: on a first join the attributes every player has
    // (the movement speed and both reaches, with the game mode's modifiers),
    // to the player and to those who see them; otherwise the absorption and
    // all the attributes again to those who see them, then the game mode's
    // modifiers to all.
    void finish_arrival(Player& p) {
        using gameplay::attributes::Attribute;
        const auto arrival = std::exchange(p.arrival, Player::Arrival::none);
        if (arrival == Player::Arrival::none) return;
        if (arrival == Player::Arrival::first) {
            for (const auto a : {Attribute::block_interaction_range, Attribute::entity_interaction_range, Attribute::movement_speed}) attribute(p, a);
            game_mode_attributes(p);
            p.attributes_changed.clear();
            send_attributes(p, p.attributes, true, true);
            return;
        }
        send_attributes(p, p.attributes, false, true);
        game_mode_attributes(p);
        std::vector<gameplay::attributes::Instance> changed;
        for (const auto a : p.attributes_changed) changed.push_back(attribute(p, a));
        p.attributes_changed.clear();
        send_attributes(p, changed, true, true);
    }

    // Health, food and saturation, sent when they change (saturation only
    // when it reaches or leaves zero), as the target version does.
    void send_vitals(Player& p, bool if_changed = false) {
        const bool zero = p.saturation == 0.0f;
        if (if_changed && p.health == p.sent_health && p.food == p.sent_food && zero == p.sent_saturation_zero) return;
        p.peer->send(wire.set_health(p.health, p.food, p.saturation));
        p.sent_health = p.health;
        p.sent_food = p.food;
        p.sent_saturation_zero = zero;
    }

    // Hunger, once per tick in every game mode: exhaustion beyond 4 uses up saturation, then
    // food. A hurt player (below their maximum health) regenerates while the
    // naturalRegeneration rule allows: with full food and saturation left,
    // saturation/6 (at most 1) every 10 ticks for that much exhaustion; with
    // food 18 or more, 1 every 80 ticks for 6. Without food, starvation
    // hurts every 80 ticks down to 10 health (easy), 1 (normal) or death
    // (hard). In peaceful, health (by that rule) and food come back by
    // themselves.
    void hunger(Player& p) {
        const auto difficulty = config.difficulty;
        if (p.exhaustion > 4.0f) {
            p.exhaustion -= 4.0f;
            if (p.saturation > 0.0f) p.saturation = std::max(p.saturation - 1.0f, 0.0f);
            else if (difficulty != 0) p.food = std::max(p.food - 1, 0);
        }
        const bool hurt_now = p.health < max_health(p);
        const bool regenerates = gameplay::game_rule_value(bundle, rules, "naturalRegeneration") != 0;
        if (regenerates && p.saturation > 0.0f && hurt_now && p.food >= 20) {
            if (++p.food_timer >= 10) {
                const auto f = std::min(p.saturation, 6.0f);
                heal(p, f / 6.0f);
                p.exhaustion = std::min(p.exhaustion + f, 40.0f);
                p.food_timer = 0;
            }
        } else if (regenerates && p.food >= 18 && hurt_now) {
            if (++p.food_timer >= 80) {
                heal(p, 1.0f);
                p.exhaustion = std::min(p.exhaustion + 6.0f, 40.0f);
                p.food_timer = 0;
            }
        } else if (p.food <= 0) {
            if (++p.food_timer >= 80) {
                if (p.health > 10.0f || difficulty == 3 || (p.health > 1.0f && difficulty == 2)) hurt(p, 1.0f, Harm::starve);
                p.food_timer = 0;
            }
        } else {
            p.food_timer = 0;
        }
        if (difficulty == 0 && !p.dead) {
            if (regenerates && hurt_now && p.ticks_alive % 20 == 0) heal(p, 1.0f);
            if (p.food < 20 && p.ticks_alive % 10 == 0) ++p.food;
        }
    }

    // Once per tick: falling out of the world (4 damage below 64 blocks under
    // it), hunger and regeneration in survival, the hurt cooldown, and the
    // floating kick after 80 ticks.
    void update_vitals(bool world_runs) {
        std::vector<std::uint64_t> kicked;
        tell_hurt_velocities();
        release_wear();
        for (auto& [id, p] : players) {
            if (!p.dead) {
                if (p.floating && ++p.floating_ticks > 80) {
                    log_warn(std::format("{} was kicked for floating too long", p.name));
                    kicked.push_back(id);
                    continue;
                }
                if (!p.floating) p.floating_ticks = 0;
            }
            follow_camera(p);
            if (!p.dead) {
                // The arm's swing goes on; the attack's charge (combat-timing):
                // a tick more, none with another item in the main hand since
                // the last tick. Both go on while the world is frozen: they
                // are the player's own.
                if (p.swing_ticks && ++*p.swing_ticks >= swing_length) p.swing_ticks.reset();
                ++p.attack_ticks;
                if (const auto& held = slot_of(p, main_hand(p)); (held.empty() ? 0 : held.item) != p.main_item) {
                    p.attack_ticks = 0;
                    p.main_item = held.empty() ? 0 : held.item;
                }
            }
            if (world_runs && !p.dead) {
                ++p.ticks_alive;
                if (p.hurt_cooldown > 0) --p.hurt_cooldown;
                fire_tick(p);
                if (!p.dead && p.pos.y < bundle.world.min_y - 64.0) hurt(p, 4.0f, Harm::void_);
                // Suffocation, breath, the turtle shell and frost
                // (client/drown), then the pose the space forces.
                if (!p.dead && eyes_in_wall(p)) hurt(p, 1.0f, Harm::in_wall);
                if (!p.dead) breathe(p);
                if (!p.dead) turtle_shell(p);
                if (!p.dead) freeze(p);
                decide_pose(p);
                // The effects act before hunger takes the tick's exhaustion
                // (client/eat: the last food a hunger effect takes goes
                // before the effect's removal). Hunger in every mode (only
                // survival actions tire), also in the tick of a death.
                if (!p.vitals_pending) tick_effects(p);
                hunger(p);
                end_cooldowns(p);
                tick_use(p);
                p.tick_pos = p.pos;
                p.tick_in_lava = in_lava(p);
            } else if (world_runs && p.dead && ++p.dead_ticks == 20) {
                body_goes(p);
            }
            p.stopped = p.teleported = false;
            if (!p.vitals_pending && !std::exchange(p.attacked, false)) send_vitals(p, true);
            send_living(p);
        }
        for (const auto id : kicked) players.at(id).peer->kick("translate:multiplayer.disconnect.flying");
    }

    bool in_reach(const Player& p, BlockPos b) const {
        const double eye = p.pos.y + eye_height(p);
        const auto axis = [](double v, double low) { return v < low ? low - v : (v > low + 1.0 ? v - low - 1.0 : 0.0); };
        const double dx = axis(p.pos.x, b.x), dy = axis(eye, b.y), dz = axis(p.pos.z, b.z);
        const double reach = p.game_mode == creative ? creative_reach : survival_reach;
        return dx * dx + dy * dy + dz * dz <= reach * reach;
    }

    // Within the interaction range plus `extra` (containers stay open up to 4 further).
    bool in_reach_plus(const Player& p, BlockPos b, double extra) const {
        const double eye = p.pos.y + eye_height(p);
        const auto axis = [](double v, double low) { return v < low ? low - v : (v > low + 1.0 ? v - low - 1.0 : 0.0); };
        const double dx = axis(p.pos.x, b.x), dy = axis(eye, b.y), dz = axis(p.pos.z, b.z);
        const double reach = (p.game_mode == creative ? creative_reach : survival_reach) + extra;
        return dx * dx + dy * dy + dz * dz <= reach * reach;
    }

    // The stack in a player's slot as the last tick left it.
    contracts::ItemStack slot_of(const Player& p, std::uint8_t slot) const {
        if (const auto inv = engine->view().player(key_of(p)); inv && *inv) return (*inv)->slots[slot];
        return {};
    }
    std::uint8_t main_hand(const Player& p) const { return static_cast<std::uint8_t>(contracts::hotbar_first + p.held); }

    // A full block here would intersect a player standing or flying there.
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

    // Breaking and placing are resolved by the rules when the command is
    // applied, after the commands admitted before it (PlayerBreak, PlayerUse).
    // Here only what the world does not decide is checked. A refused command
    // changes nothing; the client restores its prediction at the acknowledgement.
    void handle(Player& p, intent::Dig& d) {
        if (p.dead) return;
        const auto b = block_pos(d.pos);
        if (d.status == 3 || d.status == 4) {
            drop(p, d.status == 3);
            return;
        }
        // The swap key: the main hand's and the off hand's stacks swap places,
        // not for spectators (client/books).
        if (d.status == 6) {
            if (p.game_mode != spectator) admit(contracts::SwapHands{key_of(p), main_hand(p)});
            return;
        }
        p.acks.push_back(d.sequence);
        if (d.status == 5) { // the use let go
            if (p.using_item && !p.using_item->consuming) stop_use(p);
            return;
        }
        if (d.status == 1) { // aborted
            p.digging.reset();
            return;
        }
        if (d.status != 0 && d.status != 2) return;
        if (p.game_mode == spectator) {
            if (d.status == 0) correct(p, b); // the start alone is answered (client/spectators)
            return;
        }
        if (!in_reach(p, b) || !gameplay::in_world(bundle, b)) {
            correct(p, b);
            return;
        }
        if (p.game_mode == creative) {
            // Creative players break blocks when they start digging.
            if (d.status == 0) admit(contracts::PlayerBreak{key_of(p), b, false, main_hand(p)});
            return;
        }
        // Survival: the time a block takes follows the target version's
        // destroy progress; the client's finish is accepted at 70 % (network
        // delay), and an early finish completes once the progress is whole.
        const auto progress = gameplay::destroy_progress(at(b), slot_of(p, main_hand(p)), p.on_ground, eyes_in_water(p), nullptr, dig_effects(p));
        if (d.status == 0) {
            if (progress >= 1.0f) {
                admit(contracts::PlayerBreak{key_of(p), b, true, main_hand(p)});
            exhaust(p, 0.005f); // breaking a block
                exhaust(p, 0.005f); // breaking a block
                p.digging.reset();
            } else {
                p.digging = Player::Digging{b, tick_count.load(), false};
            }
            return;
        }
        if (!p.digging || p.digging->pos != b) {
            correct(p, b);
            return;
        }
        const auto ticks = static_cast<float>(tick_count.load() - p.digging->start);
        if (progress * (ticks + 1.0f) >= 0.7f) {
            admit(contracts::PlayerBreak{key_of(p), b, true, main_hand(p)});
            p.digging.reset();
        } else {
            p.digging->delayed = true;
        }
    }

    // Whether water covers the player's eyes, as the target version decides
    // it: the fluid in the cell 1/9 below the eyes reaches above that point.
    bool eyes_in_water(const Player& p) const {
        const double y = p.pos.y + eye_height(p) - 1.0 / 9.0;
        const BlockPos cell{static_cast<std::int32_t>(std::floor(p.pos.x)), static_cast<std::int32_t>(std::floor(y)),
                            static_cast<std::int32_t>(std::floor(p.pos.z))};
        const auto height = gameplay::fluid_height(block_now(cell), block_now(cell.relative(contracts::Direction::up)));
        return height > 0.0f && cell.y + static_cast<double>(height) > y;
    }

    // Survival digging that the client finished early completes when the
    // progress reaches the whole block.
    void continue_digging() {
        for (auto& [id, p] : players) {
            if (!p.digging || !p.digging->delayed) continue;
            const auto state = at(p.digging->pos);
            if (state.is_air()) {
                p.digging.reset();
                continue;
            }
            const auto ticks = static_cast<float>(tick_count.load() - p.digging->start);
            if (gameplay::destroy_progress(state, slot_of(p, main_hand(p)), p.on_ground, eyes_in_water(p), data.game.get(), dig_effects(p)) * (ticks + 1.0f)
                >= 1.0f) {
                admit(contracts::PlayerBreak{key_of(p), p.digging->pos, true, main_hand(p)});
                exhaust(p, 0.005f); // breaking a block
                p.digging.reset();
            }
        }
    }

    // Dropping from the main hand: one item, or the whole stack.
    void drop(Player& p, bool whole_stack) {
        if (p.game_mode == spectator) return;
        const auto [position, velocity] = throw_from(p);
        const auto view = engine->view();
        const auto inv = view.player(key_of(p));
        auto left = inv && *inv ? (*inv)->slots[main_hand(p)] : contracts::ItemStack{};
        if (whole_stack || left.count <= 1) left = {};
        else --left.count;
        if (const auto admission = queue.admit(contracts::DropItem{key_of(p), main_hand(p), whole_stack, position, velocity}, overworld))
            p.hand_actions.push_back({*admission, main_hand(p), true, left});
        else
            log_warn("game: command queue full; input dropped");
    }

    // Players pick up the items their box, widened by 1 sideways and 0.5 up
    // and down, touches once the pickup delay is over. One player per item
    // and tick, in player order.
    struct Pickup {
        std::uint64_t player;
        std::uint64_t entity;
        std::int32_t count;
    };
    std::map<std::uint64_t, Pickup> pickups; // by admission
    void detect_pickups() {
        const auto view = engine->view();
        std::set<std::uint64_t> taken;
        for (auto& [id, p] : players) {
            if (p.game_mode == spectator) continue;
            for (const auto& [entity_id, t] : tracked) {
                if (taken.contains(entity_id)) continue;
                const auto e = view.entity(overworld, EntityId{entity_id});
                if (!e || !*e) continue;
                const auto pos = (*e)->position;
                // An experience orb (0.5 wide and high) the player's box,
                // grown by 1 sideways and 0.5 up and down, touches.
                if ((*e)->kind == gameplay::EntityKind::experience_orb) {
                    if (pos.x + 0.25 <= p.pos.x - 1.3 || pos.x - 0.25 >= p.pos.x + 1.3 || pos.z + 0.25 <= p.pos.z - 1.3 || pos.z - 0.25 >= p.pos.z + 1.3
                        || pos.y + 0.5 <= p.pos.y - 0.5 || pos.y >= p.pos.y + 1.8 + 0.5)
                        continue;
                    const auto admission = queue.admit(contracts::PickUp{key_of(p), EntityId{entity_id}, main_hand(p), p.game_mode == creative});
                    if (!admission) continue;
                    taken.insert(entity_id);
                    pickups[*admission] = {id, entity_id, 1};
                    continue;
                }
                if ((*e)->kind != gameplay::EntityKind::item || (*e)->pickup_delay > 0) continue;
                // The item's box is 0.25 wide and high.
                const bool x = pos.x + 0.125 > p.pos.x - 1.3 && pos.x - 0.125 < p.pos.x + 1.3;
                const bool z = pos.z + 0.125 > p.pos.z - 1.3 && pos.z - 0.125 < p.pos.z + 1.3;
                const bool y = pos.y + 0.25 > p.pos.y - 0.5 && pos.y < p.pos.y + 1.8 + 0.5;
                if (!x || !y || !z) continue;
                const auto admission = queue.admit(contracts::PickUp{key_of(p), EntityId{entity_id}, main_hand(p), p.game_mode == creative});
                if (!admission) continue;
                taken.insert(entity_id);
                pickups[*admission] = {id, entity_id, (*e)->stack.count};
            }
        }
    }

    // The pickup animation for every pickup the tick applied.
    void publish_pickups(const std::vector<contracts::Command>& commands) {
        if (pickups.empty()) return;
        std::set<std::uint64_t> applied;
        for (const auto& r : engine->trace())
            if (r.kind == contracts::TraceKind::command_applied) applied.insert(r.a);
        for (const auto& c : commands) {
            const auto it = pickups.find(c.admission);
            if (it == pickups.end()) continue;
            const auto pickup = it->second;
            pickups.erase(it);
            if (!applied.contains(c.admission)) continue;
            const auto collector = players.find(pickup.player);
            const auto entity = tracked.find(pickup.entity);
            if (collector == players.end() || entity == tracked.end()) continue;
            const auto packet = wire.take_item_entity(entity->second.protocol_id, collector->second.entity_id, pickup.count);
            collector->second.peer->send(packet);
            for (const auto viewer : entity->second.viewers)
                if (viewer != pickup.player)
                    if (const auto v = players.find(viewer); v != players.end()) v->second.peer->send(packet);
        }
    }

    void handle(Player& p, intent::UseOn& u) {
        if (p.dead) return;
        p.acks.push_back(u.sequence);
        const auto b = block_pos(u.pos);
        // A spectator opens a block's window, if it has one (a chest's,
        // without opening its lid: client/spectators; the other windows by the
        // same rule, but an ender chest's and a lectern's, not recorded), and
        // uses nothing.
        if (p.game_mode == spectator && u.face >= 0 && u.face <= 5 && in_reach(p, b) && gameplay::in_world(bundle, b) && spectator_window(b))
            open_window(p, b);
        if (u.face < 0 || u.face > 5 || p.game_mode == spectator || !in_reach(p, b) || !gameplay::in_world(bundle, b)) {
            // The client predicted a change at the clicked block or next to it.
            correct(p, b);
            if (u.face >= 0 && u.face <= 5) correct(p, b.relative(static_cast<Direction>(u.face)));
            return;
        }
        // The client sends the off hand only when the main hand did nothing,
        // and the off hand holds nothing in this server.
        const auto face = static_cast<Direction>(u.face);
        const auto beside = b.relative(face);
        contracts::PlayerUse use{key_of(p),
                                 static_cast<std::uint8_t>(u.hand == 0 ? contracts::hotbar_first + p.held : contracts::offhand_slot),
                                 b,
                                 face,
                                 horizontal_look(p.yaw),
                                 look(p.yaw, p.pitch),
                                 p.sneaking,
                                 p.game_mode == survival,
                                 1,
                                 0};
        // Where the click hit the block, as the client reports it (0 to 1 on
        // each axis; an unusable value counts as the middle).
        const auto within = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.5f; };
        use.cursor_x = within(u.cursor_x);
        use.cursor_y = within(u.cursor_y);
        use.cursor_z = within(u.cursor_z);
        use.yaw = std::isfinite(p.yaw) ? std::fmod(p.yaw, 360.0f) : 0.0f;
        if (gameplay::in_world(bundle, beside) && in_reach(p, beside)) use.reach |= 2;
        if (!occupied_by_player(b)) use.free |= 1;
        if (!occupied_by_player(beside)) use.free |= 2;
        p.used_on = Player::UsedOn{tick_count.load(), u.hand, {b, face, use.cursor_x, use.cursor_y, use.cursor_z}};
        admit(use);
    }

    // A use of the held item along the player's view: buckets and empty
    // maps. The client sends it after a click on the block its view meets,
    // if any, in the same tick, as both official servers expect.
    void handle(Player& p, intent::Use& u) {
        p.acks.push_back(u.sequence);
        const auto used_on = std::exchange(p.used_on, std::nullopt);
        if (p.dead || p.game_mode == spectator || (u.hand != 0 && u.hand != 1)) return; // 0 the main hand, 1 the off hand
        const auto slot = static_cast<std::uint8_t>(u.hand == 0 ? contracts::hotbar_first + p.held : contracts::offhand_slot);
        const auto item = slot_of(p, slot).item;
        // Food and drink are held until eaten or drunk.
        if (const auto* c = gameplay::food::of(item)) {
            start_use(p, u.hand, slot, *c);
            return;
        }
        // A shield is raised until let go (client/shield).
        if (static const auto shield = gameplay::items::by_name("minecraft:shield"); shield && item == *shield) {
            raise_shield(p, u.hand, slot);
            return;
        }
        // A written book opens for reading (client/books).
        if (static const auto written = gameplay::items::by_name("minecraft:written_book"); written && item == *written) {
            p.peer->send(wire.open_book(u.hand));
            return;
        }
        // A knowledge book gives its recipes, and the rules use it up
        // (client/knowledge_books).
        static const auto knowledge = gameplay::items::by_name("minecraft:knowledge_book");
        const bool knowledge_book = knowledge && item == *knowledge;
        const auto* worn = gameplay::equipment::wearable(item);
        if (item != gameplay::items::bucket && item != gameplay::items::water_bucket && item != gameplay::items::lava_bucket
            && item != gameplay::items::map && !knowledge_book && !(worn && worn->swappable))
            return;
        if (knowledge_book) learn_from_book(p, slot_of(p, slot));
        if (std::isfinite(u.yaw) && std::isfinite(u.pitch)) {
            p.yaw = std::fmod(u.yaw, 360.0f);
            p.pitch = std::clamp(u.pitch, -90.0f, 90.0f);
        }
        contracts::PlayerUseItem use{key_of(p), slot, {p.pos.x, p.pos.y + eye_height(p), p.pos.z}, p.yaw, p.pitch,
                                     p.game_mode == creative ? creative_view : survival_view, p.game_mode == survival, std::nullopt};
        if (used_on && used_on->tick == tick_count.load() && used_on->hand == u.hand) use.hit = used_on->hit;
        if (const auto admission = queue.admit(use, overworld)) p.hand_actions.push_back({*admission, slot, false, {}});
        else log_warn("game: command queue full; input dropped");
    }

    void handle(Player& p, intent::HeldSlot& h) {
        if (h.slot < 0 || h.slot > 8) {
            p.peer->kick("Invalid hotbar slot");
            return;
        }
        p.held = h.slot;
    }

    void handle(Player& p, intent::CreativeSlot& c) {
        if (p.dead) return;
        if (p.game_mode != creative) return;
        if (c.slot < 0 || c.slot >= static_cast<std::int16_t>(inventory_slots)) return; // dropping is not supported
        if (c.count == 0) {
            admit(contracts::SetPlayerSlot{key_of(p), static_cast<std::uint8_t>(c.slot), {}});
            if (!p.window && p.remote_window == 0 && static_cast<std::size_t>(c.slot) < p.remote.size())
                p.remote[static_cast<std::size_t>(c.slot)] = WireStack{};
            return;
        }
        // The item must be one the rule set knows, in a valid count, with
        // components the server could read and check.
        const auto name = content.item_name(c.item);
        const auto item = name ? mapping.item(*name) : std::unexpected(std::string("unknown"));
        contracts::ItemStack stack{item ? *item : std::uint16_t{0}, static_cast<std::uint8_t>(std::clamp(c.count, 0, 255)), 0,
                                   c.components};
        if (!c.refused.empty() || c.count < 0 || !item || c.count > gameplay::items::max_stack(stack)
            || (gameplay::items::max_damage(stack) > 0 && gameplay::items::max_stack(stack) > 1)) {
            if (!c.refused.empty()) log_info(std::format("{}: creative stack refused: {}", p.name, c.refused));
            p.remote_window = -1; // the client shows what it asked for: send everything again
            return;
        }
        stack.damage = static_cast<std::uint16_t>(std::clamp<std::int32_t>(c.damage, 0, gameplay::items::max_damage(stack)));
        admit(contracts::SetPlayerSlot{key_of(p), static_cast<std::uint8_t>(c.slot), stack});
        // As in the target version, the slot is not echoed: the client already shows it.
        if (!p.window && p.remote_window == 0 && static_cast<std::size_t>(c.slot) < p.remote.size())
            p.remote[static_cast<std::size_t>(c.slot)] = wire_stack(stack);
    }

    void handle(Player& p, intent::ClientCommand& c) {
        if (c.action == 0 && p.dead) respawn(p);
    }

    void handle(Player& p, intent::Sneak& s) { p.sneaking = s.sneaking; }
    // An arm's swing, for those who see the player (client/armor: 0 the main
    // arm, 3 the off arm; not to the player).
    void handle(Player& p, intent::Swing& s) {
        if (p.dead || (s.hand != 0 && s.hand != 1)) return;
        p.attack_ticks = 0; // a swing starts the attack's charge again (combat-timing)
        // A swing within the first half of the last is not shown
        // (client/combat: a second hit 2 ticks after the first showed none,
        // one 4 ticks after showed its swing).
        if (p.swing_ticks && *p.swing_ticks >= 0 && *p.swing_ticks < swing_length / 2) return;
        p.swing_ticks = -1;
        const auto packet = wire.animate(p.entity_id, s.hand == 0 ? 0 : 3);
        for (const auto viewer : p.seen_by)
            if (const auto v = players.find(viewer); v != players.end()) v->second.peer->send(packet);
    }
    // Sprinting is faster (client/attributes).
    void handle(Player& p, intent::Sprint& s) {
        if (p.dead || p.sprinting == s.sprinting) return;
        p.sprinting = s.sprinting;
        if (p.sprinting) add_modifier(p, gameplay::attributes::Attribute::movement_speed, gameplay::attributes::sprinting);
        else remove_modifier(p, gameplay::attributes::Attribute::movement_speed, gameplay::attributes::sprinting.id);
    }
    void handle(Player& p, intent::Flying& f) { p.flying = f.flying && (p.game_mode == creative || p.game_mode == spectator); }
    void handle(Player&, intent::Loaded&) {}

    // Clicks apply to the open window (0: the own inventory). As in the target
    // version, the client's predicted slots and carried stack become its view,
    // so that only wrong predictions are corrected after the tick; a click made
    // on a stale state id gets the whole window.
    void handle(Player& p, intent::ContainerClick& c) {
        if (p.dead) return;
        const auto id = p.window ? p.window->id : 0;
        if (c.window != id) return;
        if (p.game_mode == spectator || c.mode < 0 || c.mode > 6 || c.unknown || c.state != (p.window ? p.window->state : p.inventory_state)) {
            p.remote_window = -1;
        } else {
            for (const auto& [i, stack] : c.changed)
                if (i >= 0 && static_cast<std::size_t>(i) < p.remote.size()) p.remote[static_cast<std::size_t>(i)] = stack;
            p.remote_carried = c.carried;
        }
        if (p.game_mode == spectator || c.mode < 0 || c.mode > 6) return;
        std::optional<BlockPos> container;
        if (p.window) container = p.window->pos;
        const auto [position, velocity] = throw_from(p);
        if (const auto admitted = queue.admit(contracts::ContainerClick{key_of(p), container, c.slot, static_cast<std::uint8_t>(c.button),
                                                                         static_cast<std::uint8_t>(c.mode), p.game_mode == creative, position, velocity},
                                              overworld))
            p.clicks.push_back(*admitted);
        else
            log_warn("game: command queue full; input dropped");
    }

    void handle(Player& p, intent::ContainerClose& c) {
        if (p.window && c.window == p.window->id) {
            close_window(p, false);
        } else if (!p.window && c.window == 0) {
            admit_close(p);
        }
    }

    // A slot toggle of the open window (a crafter's grid); one of another
    // window does nothing.
    void handle(Player& p, intent::SlotState& s) {
        // A spectator changes nothing through a window it may look into.
        if (!p.window || p.game_mode == spectator || s.window != p.window->id || s.slot < 0 || s.slot > 8) return;
        admit(contracts::SetSlotEnabled{key_of(p), p.window->pos, static_cast<std::uint8_t>(s.slot), s.enabled});
    }

    // A button of the open window (a stonecutter's recipe); a button of
    // another window does nothing.
    void handle(Player& p, intent::ContainerButton& c) {
        if (!p.window || p.game_mode == spectator || c.window != p.window->id) return;
        admit(contracts::ContainerButton{key_of(p), p.window->pos, c.button, p.game_mode == creative});
    }

    // The name typed into an open anvil window, as both official servers
    // take it: without the section sign and control characters, and ignored
    // when it is still longer than 50 UTF-16 code units.
    // The recipe book: a recipe seen loses its highlight; a book's settings
    // are kept (both official servers keep them for the player's next join).
    void handle(Player& p, intent::RecipeSeen& r) {
        if (r.display >= 0) p.book.highlighted.erase(static_cast<std::size_t>(r.display));
    }
    void handle(Player& p, intent::RecipeBookSettings& r) {
        if (r.book < 0 || r.book > 3) return;
        p.book.settings[static_cast<std::size_t>(2 * r.book)] = r.open;
        p.book.settings[static_cast<std::size_t>(2 * r.book + 1)] = r.filtering;
    }

    // The recipes a knowledge book names, given as /recipe gives them, when
    // every one is a recipe of the book; otherwise none (both official servers).
    void learn_from_book(Player& p, const contracts::ItemStack& stack) {
        const auto* book = data.recipe_book.get();
        const auto ids = wire.components().recipes_of(stack.components);
        if (!book || !ids || ids->empty()) return;
        std::vector<std::size_t> recipes;
        for (const auto& id : *ids) {
            const auto it = book->by_id.find(id.find(':') == std::string::npos ? "minecraft:" + id : id);
            if (it == book->by_id.end()) return;
            recipes.push_back(it->second);
        }
        send_unlocks(p, server::award(*book, p.book, recipes));
    }

    // A book and quill edited or signed in a hotbar slot or the off hand, as
    // both official servers take it (client/books): signed by the player.
    void handle(Player& p, intent::EditBook& e) {
        if (p.dead) return;
        const auto slot = e.hand >= 0 && e.hand < 9 ? std::optional<std::uint8_t>(contracts::hotbar_first + e.hand)
                        : e.hand == 40               ? std::optional<std::uint8_t>(contracts::offhand_slot)
                                                     : std::nullopt;
        if (!slot) return;
        admit(contracts::EditBook{key_of(p), *slot, std::move(e.pages), std::move(e.title), p.name});
    }

    // A recipe of the book placed into the open window (the own window while
    // none is open), as both official servers take it: one the player knows,
    // asked for in the window's id; spectators place none.
    void handle(Player& p, intent::PlaceRecipe& r) {
        const auto* book = data.recipe_book.get();
        if (!book || p.game_mode == spectator || r.display < 0 || r.window != (p.window ? p.window->id : 0)) return;
        const auto display = static_cast<std::size_t>(r.display);
        if (display >= book->recipes.size() || !p.book.known.contains(display)) return;
        const auto recipe = rule_recipe(book->recipes[display].id);
        if (!recipe) return;
        std::optional<BlockPos> container;
        if (p.window) container = p.window->pos;
        admit(contracts::PlaceRecipe{key_of(p), container, recipe->first, recipe->second, r.all, main_hand(p)});
    }

    // The rules' recipes by id, and the id of one by kind and index (as
    // TraceKind::recipe_used names them: 0 crafting, 1 cooking, 2
    // stonecutting, 3 smithing).
    std::optional<std::pair<std::uint8_t, std::uint32_t>> rule_recipe(std::string_view id) {
        if (!data.game) return std::nullopt;
        if (rule_recipes.empty()) {
            const auto& g = *data.game;
            const auto add = [&](std::uint8_t kind, const auto& list) {
                for (std::size_t k = 0; k < list.size(); ++k) rule_recipes.emplace(list[k].id, std::pair{kind, static_cast<std::uint32_t>(k)});
            };
            add(0, g.crafting);
            add(1, g.cooking);
            add(2, g.stonecutting);
            add(3, g.smithing);
        }
        const auto it = rule_recipes.find(id);
        if (it == rule_recipes.end()) return std::nullopt;
        return it->second;
    }
    const std::string* rule_recipe_id(std::uint64_t kind, std::size_t index) const {
        if (!data.game) return nullptr;
        const auto& g = *data.game;
        return kind == 0 && index < g.crafting.size()       ? &g.crafting[index].id
             : kind == 1 && index < g.cooking.size()        ? &g.cooking[index].id
             : kind == 2 && index < g.stonecutting.size()   ? &g.stonecutting[index].id
             : kind == 3 && index < g.smithing.size()       ? &g.smithing[index].id
                                                            : nullptr;
    }
    // The ghosts of recipes a player asked to place without the items
    // (TraceKind::recipe_ghost), shown in the player's window.
    void send_ghosts(Player& p) {
        const auto* book = data.recipe_book.get();
        if (!book) return;
        const auto key = key_of(p);
        for (const auto& r : engine->trace()) {
            if (r.kind != contracts::TraceKind::recipe_ghost || r.a != key.hi || r.b != key.lo) continue;
            const auto* id = rule_recipe_id(r.c >> 32, static_cast<std::size_t>(r.c & 0xFFFFFFFFu));
            const auto it = id ? book->by_id.find(*id) : book->by_id.end();
            if (it != book->by_id.end()) p.peer->send(wire.place_ghost_recipe(p.window ? p.window->id : 0, book->recipes[it->second], wire_item()));
        }
    }

    std::function<std::int32_t(std::uint16_t)> wire_item() const {
        return [this](std::uint16_t i) {
            const auto it = item_wire.find(i);
            return it == item_wire.end() ? 0 : it->second;
        };
    }
    // What unlocking gave a player: each list of new recipes as one packet,
    // a notification for those that show one, all highlighted.
    void send_unlocks(Player& p, const Unlocks& unlocks) {
        std::vector<std::size_t> all;
        for (const auto& recipes : unlocks) {
            std::vector<Wire::BookEntry> entries;
            for (const auto r : recipes) {
                const auto& recipe = data.recipe_book->recipes[r];
                entries.push_back({static_cast<std::int32_t>(r), &recipe, static_cast<std::uint8_t>((recipe.notification ? 1 : 0) | 2)});
            }
            p.peer->send(wire.recipe_book_add(entries, false, wire_item()));
            all.insert(all.end(), recipes.begin(), recipes.end());
        }
        tell_known(p, all, true);
    }
    // Tells the rules which crafting recipes a player knows (KnowRecipes, for
    // doLimitedCrafting): the crafting recipes among the book's `recipes`,
    // known or not, or with `replace` all the player knows.
    template <class Recipes> void tell_known(Player& p, const Recipes& recipes, bool known, bool replace = false) {
        const auto* book = data.recipe_book.get();
        if (!book) return;
        std::vector<std::uint32_t> crafting;
        for (const auto r : recipes)
            if (const auto rule = rule_recipe(book->recipes[r].id); rule && rule->first == 0) crafting.push_back(rule->second);
        if (crafting.empty() && !replace) return;
        admit(contracts::KnowRecipes{key_of(p), std::move(crafting), known, replace});
    }
    // After each tick, in order: the recipes the player's taken results came
    // from (TraceKind::recipe_used), then the triggers of the inventory's
    // changed stacks and filled slots, the first tick and water around the
    // player (both official servers trigger them in their own orders; the
    // recipes of advancements completed at once follow their ids here).
    void update_recipe_book(Player& p, const runtime::WorldView& view) {
        const auto* book = data.recipe_book.get();
        if (!book || !data.game) return;
        const auto key = key_of(p);
        for (const auto& r : engine->trace()) {
            if (r.kind != contracts::TraceKind::recipe_used || r.a != key.hi || r.b != key.lo) continue;
            const auto* id = rule_recipe_id(r.c >> 32, static_cast<std::size_t>(r.c & 0xFFFFFFFFu));
            if (!id) continue;
            if (const auto it = book->by_id.find(*id); it != book->by_id.end()) send_unlocks(p, server::award(*book, p.book, std::array{it->second}));
        }
        const auto inv = view.player(key);
        std::vector<std::uint16_t> changed;
        std::int32_t occupied = 0;
        for (std::size_t k = 0; k < p.book_inventory.size(); ++k) {
            const auto now = inv && *inv ? (*inv)->slots[k + 5] : contracts::ItemStack{};
            if (!now.empty()) ++occupied;
            if (now == p.book_inventory[k]) continue;
            p.book_inventory[k] = now;
            if (!now.empty()) changed.push_back(now.item);
        }
        if (!changed.empty()) send_unlocks(p, server::trigger(*book, p.book, server::inventory_criteria(*book, p.book, changed, occupied)));
        send_unlocks(p, server::trigger(*book, p.book, book->on_tick));
        bool water = false;
        for (auto x = static_cast<std::int32_t>(std::floor(p.pos.x - 0.3)); x <= static_cast<std::int32_t>(std::floor(p.pos.x + 0.3)); ++x)
            for (auto y = static_cast<std::int32_t>(std::floor(p.pos.y)); y <= static_cast<std::int32_t>(std::floor(p.pos.y + 1.8)); ++y)
                for (auto z = static_cast<std::int32_t>(std::floor(p.pos.z - 0.3)); z <= static_cast<std::int32_t>(std::floor(p.pos.z + 0.3)); ++z)
                    water = water || at({x, y, z}).is(BlockType::water);
        if (water) send_unlocks(p, server::trigger(*book, p.book, book->in_water));
    }

    // `/recipe give|take <player> <recipe>|*`, with both official servers'
    // replies: the recipes of the book, given or taken.
    void recipe_command(Player& p, const std::vector<std::string>& args) {
        if (args.size() != 4 || (args[1] != "give" && args[1] != "take")) return message(p, "Usage: /recipe (give|take) <player> (<recipe>|*)", "red");
        const auto* book = data.recipe_book.get();
        Player* target = args[2] == "@s" || args[2] == "@p" ? &p : nullptr;
        for (auto& [id, o] : players)
            if (!target && o.name == args[2]) target = &o;
        if (!target) return message(p, "No player was found", "red");
        std::vector<std::size_t> recipes;
        if (args[3] == "*") {
            if (book)
                for (std::size_t r = 0; r < book->recipes.size(); ++r)
                    if (book->recipes[r].in_book) recipes.push_back(r);
        } else {
            const auto id = args[3].find(':') == std::string::npos ? "minecraft:" + args[3] : args[3];
            const auto it = book ? book->by_id.find(id) : decltype(book->by_id.end()){};
            if (!book || it == book->by_id.end()) return message(p, std::format("Unknown recipe: {}", id), "red");
            recipes.push_back(it->second);
        }
        bool changed = false;
        if (args[1] == "give") {
            const auto unlocks = book ? server::award(*book, target->book, recipes) : Unlocks{};
            changed = !unlocks.empty();
            // One packet with the recipes given, then what they complete.
            send_unlocks(*target, unlocks);
        } else {
            const auto taken = server::forget(target->book, recipes);
            changed = !taken.empty();
            if (changed) {
                std::vector<std::int32_t> ids(taken.begin(), taken.end());
                target->peer->send(wire.recipe_book_remove(ids));
                tell_known(*target, taken, false);
            }
        }
        // The reply counts the recipes named (every recipe for *, special
        // ones too), as both official servers count them, once any changed.
        const auto action = args[1];
        if (!changed) return send_translation(p, std::format("commands.recipe.{}.failed", action), nbt::Tag::list(nbt::TagType::string));
        const auto named = args[3] == "*" ? book->all : std::size_t{1};
        send_translation(p, std::format("commands.recipe.{}.success.single", action), string_arguments({std::to_string(named), target->name}));
    }

    // The powers chosen in an open beacon window.
    // Whether a spectator's use opens the block's window: one with a window
    // (not a chiseled bookshelf's slots, an ender chest or a lectern), but a
    // chest under a block that conducts, which opens for no one.
    bool spectator_window(BlockPos b) const {
        const auto s = at(b);
        if (container_slot_count(b) == 0 || s.is(BlockType::ender_chest) || s.is(BlockType::lectern) || s.is(BlockType::chiseled_bookshelf)) return false;
        for (const auto part : container_parts_of(b))
            if (at(part).is(BlockType::chest) && gameplay::conductor(at(part.relative(Direction::up)))) return false;
        return true;
    }
    // A spectator's attack takes the entity's view (client/spectators);
    // another's hits a player within reach (client/combat); attacks on other
    // entities are not modeled yet.
    void handle(Player& p, intent::Attack& a) {
        if (p.dead || a.entity == p.entity_id) return;
        if (p.game_mode == spectator) {
            if (entity_position(a.entity)) set_camera(p, a.entity);
            return;
        }
        for (auto& [id, o] : players)
            if (o.entity_id == a.entity) {
                if (!o.dead && o.game_mode != spectator && within_reach(p, o)) attack(p, o);
                return;
            }
    }
    static constexpr std::int32_t swing_length = 6; // ticks (haste and mining fatigue's changes are not modeled)
    // Reach (client/combat): the nearest point of the target's box within the
    // entity interaction range and 3 more of the attacker's eyes (a target 4.7
    // away was hit, one 6.7 away not, with the range 3: the margin is
    // Circuit's choice within those).
    bool within_reach(const Player& p, const Player& target) const {
        const auto box = box_of(target, target.pos);
        const Vec3 eye{p.pos.x, p.pos.y + eye_height(p), p.pos.z};
        const double dx = std::max({box.min_x - eye.x, 0.0, eye.x - box.max_x});
        const double dy = std::max({box.min_y - eye.y, 0.0, eye.y - box.max_y});
        const double dz = std::max({box.min_z - eye.z, 0.0, eye.z - box.max_z});
        const double reach = attribute_value(p, gameplay::attributes::Attribute::entity_interaction_range) + 3.0;
        return dx * dx + dy * dy + dz * dz < reach * reach;
    }
    // Whether the block at the player's feet is climbable (the climbable
    // block tag of both versions).
    bool climbing(const Player& p) const {
        static constexpr std::array<std::string_view, 9> climbable{"ladder", "vine", "scaffolding", "weeping_vines", "weeping_vines_plant",
                                                                   "twisting_vines", "twisting_vines_plant", "cave_vines", "cave_vines_plant"};
        const BlockPos feet{static_cast<std::int32_t>(std::floor(p.pos.x)), static_cast<std::int32_t>(std::floor(p.pos.y)),
                            static_cast<std::int32_t>(std::floor(p.pos.z))};
        return std::ranges::find(climbable, block_name(block_now(feet))) != climbable.end();
    }
    // A jump's velocity (client/combat: standing, (0, 0.42, 0)): 0.1 more a
    // level of jump boost, and sprinting 0.2 more along the facing
    // (minecraft.wiki "Jump Boost", "Sprinting"; not recorded).
    void jumped(Player& p) {
        static const auto jump_boost = gameplay::effects::id("jump_boost");
        auto v = velocity_of(p);
        // The target versions' floats, widened (client/combat: 3359 in 1/8000).
        float power = 0.42f;
        if (const auto level = jump_boost ? effect_amplifier(p, *jump_boost) : std::nullopt) power += 0.1f * static_cast<float>(*level + 1);
        v.y = static_cast<double>(power);
        if (p.sprinting) {
            const double yaw = p.yaw * std::numbers::pi / 180.0;
            v.x += -std::sin(yaw) * 0.2;
            v.z += std::cos(yaw) * 0.2;
        }
        p.impulse = v;
    }
    // A player's melee attack on another (client/combat, combat-timing; both
    // versions alike but for the sweep's order):
    //  - the attack damage, kept by the charge (0.2 plus 0.8 times its
    //    square), starts the charge again; none left (weakness), nothing more;
    //  - a strong attack (charge above 0.9) sprinting knocks: its sound
    //    first; a strong one falling (not on the ground, not climbing, in
    //    water, blind or sprinting) is critical, half as much again; a strong
    //    one on the ground with a sword and neither is a sweep (also walking:
    //    client/combat, about 0.2 a tick backwards);
    //  - the target hurt (the damage event naming the attacker, the hurt
    //    animation, its knockback away from the attacker); if not hurt (the
    //    hurt cooldown, creative mode), the nodamage sound and nothing more;
    //  - a sprint's 0.5 knockback along the attacker's facing, whose sprint
    //    ends; the sweep: each other player in survival within the target's
    //    box grown by 1, 0.25, 1 and 3 blocks of the attacker hurt for 1 and
    //    knocked 0.4 along the facing (1.21.8 the hurt first, the knock if it
    //    hurt; 1.21.4 the knock first), the sweep sound and particle;
    //  - the target's velocity told to the target, the server keeping its
    //    velocity from before the attack;
    //  - the critical hit's sound and animation, or the strong or weak sound
    //    (a sweep's own); the damage indicator particles (half the health the
    //    target lost); the weapon's wear outside creative mode; exhaustion.
    void attack(Player& p, Player& t) {
        using gameplay::attributes::Attribute;
        using Sound = Wire::PlayerSound;
        namespace damage = gameplay::damage;
        const float charge = damage::attack_charge(p.attack_ticks, attribute_value(p, Attribute::attack_speed));
        float amount = damage::charged_damage(static_cast<float>(attribute_value(p, Attribute::attack_damage)), charge);
        p.attack_ticks = 0;
        if (amount <= 0.0f) return;
        const auto own_sound = [&](Wire::PlayerSound sound) {
            sound_near(p, wire.player_sound(sound, p.pos, 1.0f, 1.0f, static_cast<std::int64_t>(sound_seed++)), true);
        };
        static const auto blindness = gameplay::effects::id("blindness");
        const bool strong = charge > damage::strong_charge;
        const bool knocks = strong && p.sprinting;
        if (knocks) own_sound(Sound::attack_knockback);
        const bool critical = strong && p.fall_distance > 0.0 && !p.on_ground && !climbing(p) && !in_water(p)
                              && !(blindness && effect_amplifier(p, *blindness)) && !p.sprinting;
        if (critical) amount *= 1.5f;
        const auto held = slot_of(p, main_hand(p));
        const bool sweep = strong && !knocks && !critical && p.on_ground && !held.empty() && gameplay::equipment::sweeps(held.item);
        const float health_before = t.health;
        const auto velocity_before = velocity_of(t);
        if (!hurt(t, amount, Harm::player_attack, 0.0, &p)) {
            own_sound(Sound::attack_nodamage);
            return;
        }
        const double yaw = p.yaw * std::numbers::pi / 180.0;
        const double facing_x = -std::sin(yaw), facing_z = std::cos(yaw);
        if (knocks) {
            knock(t, 0.5, facing_x, facing_z);
            p.sprinting = false;
            remove_modifier(p, Attribute::movement_speed, gameplay::attributes::sprinting.id);
        }
        if (sweep) {
            const auto around = box_of(t, t.pos).inflate(1.0, 0.25, 1.0);
            for (auto& [oid, o] : players) {
                if (&o == &p || &o == &t || o.dead || o.game_mode != survival || !box_of(o, o.pos).intersects(around)) continue;
                const double dx = o.pos.x - p.pos.x, dy = o.pos.y - p.pos.y, dz = o.pos.z - p.pos.z;
                if (dx * dx + dy * dy + dz * dz >= 9.0) continue;
                if (wire.protocol() >= 772) {
                    if (hurt(o, 1.0f, Harm::player_attack, 0.0, &p)) knock(o, 0.4, facing_x, facing_z);
                } else {
                    knock(o, 0.4, facing_x, facing_z);
                    hurt(o, 1.0f, Harm::player_attack, 0.0, &p);
                }
            }
            own_sound(Sound::attack_sweep);
            const auto packet = wire.particles(Wire::Particle::sweep_attack, {p.pos.x + facing_x, p.pos.y + body_height(p) * 0.5, p.pos.z + facing_z},
                                               {facing_x, 0.0, facing_z}, 0.0f, 0);
            particles_near(packet, p.pos);
        }
        if (t.hurt_marked) {
            t.peer->send(wire.entity_motion(t.entity_id, velocity_of(t)));
            t.hurt_marked = false;
            t.impulse = velocity_before;
        }
        if (critical) {
            own_sound(Sound::attack_crit);
            const auto packet = wire.animate(t.entity_id, 4);
            t.peer->send(packet);
            for (const auto viewer : t.seen_by)
                if (const auto o = players.find(viewer); o != players.end()) o->second.peer->send(packet);
        } else if (!sweep) {
            own_sound(strong ? Sound::attack_strong : Sound::attack_weak);
        }
        if (const auto count = damage::damage_indicators(health_before - t.health); count > 0) {
            const Vec3 middle{t.pos.x, t.pos.y + body_height(t) * 0.5, t.pos.z};
            particles_near(wire.particles(Wire::Particle::damage_indicator, middle, {0.1, 0.0, 0.1}, 0.2f, count), middle);
        }
        if (p.game_mode != creative && !held.empty())
            if (const auto wear = gameplay::equipment::attack_wear(held.item); wear > 0) {
                admit(contracts::WearHeld{key_of(p), main_hand(p), held.item, wear});
                hold_wear(p, std::uint64_t{1} << main_hand(p), false);
            }
        exhaust(p, 0.1f);
    }
    // Particles for the players within 32 blocks of them (not recorded:
    // everyone in client/combat stands within 3).
    void particles_near(const Bytes& packet, Vec3 at) {
        for (auto& [oid, o] : players) {
            const double dx = o.pos.x - at.x, dy = o.pos.y - at.y, dz = o.pos.z - at.z;
            if (dx * dx + dy * dy + dz * dz < 32.0 * 32.0) o.peer->send(packet);
        }
    }
    // The spectator menu's teleport to a player (client/spectators: to the
    // player's position; other entities are not shown in it here).
    void handle(Player& p, intent::TeleportToEntity& t) {
        if (p.dead || p.game_mode != spectator) return;
        for (const auto& [id, o] : players)
            if (o.uuid == t.target && &o != &p) {
                const auto at = o.pos;
                if (p.camera) set_camera(p, std::nullopt); // its own view first
                place_spectator(p, at);
                teleport(p, at);
                return;
            }
    }

    // The client's settings: the skin parts and main hand its entity data
    // shows, told at the next entity update (client/player_data).
    void handle(Player& p, intent::ClientInformation& c) {
        p.skin_parts = c.skin_parts;
        p.main_arm = c.main_hand;
    }

    void handle(Player& p, intent::SetBeacon& s) {
        if (!p.window || p.game_mode == spectator || !at(p.window->pos).is(BlockType::beacon)) return;
        // An id that is not a beacon's power counts as none, as the rules
        // take it; so the log keeps only the powers.
        const auto power = [](std::optional<std::int32_t> e) -> std::optional<std::int32_t> {
            return e && *e >= 0 && *e <= contracts::max_beacon_effect && gameplay::effects::is_beacon_power(static_cast<std::uint8_t>(*e))
                       ? e : std::nullopt;
        };
        admit(contracts::SetBeacon{key_of(p), p.window->pos, power(s.primary), power(s.secondary)});
    }

    void handle(Player& p, intent::RenameItem& r) {
        if (!p.window || p.game_mode == spectator || !gameplay::is_anvil(at(p.window->pos))) return;
        std::string name;
        std::size_t units = 0;
        for (std::size_t i = 0; i < r.name.size();) {
            const auto lead = static_cast<unsigned char>(r.name[i]);
            const std::size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
            if (i + length > r.name.size()) return;
            const auto ch = std::string_view(r.name).substr(i, length);
            i += length;
            if ((length == 1 && (lead < 0x20 || lead == 0x7F)) || ch == "\xC2\xA7") continue;
            name += ch;
            units += length == 4 ? 2 : 1;
        }
        if (units > contracts::max_item_name) return;
        admit(contracts::RenameItem{key_of(p), std::move(name), p.game_mode == creative});
    }

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
            message(p, "Commands: /help, /players, /tp [<own name>] <x> <y> <z>, /gamemode <survival|creative|spectator>, "
                       "/forceload add|remove <x> <z> [<x> <z>], /forceload remove all, /forceload query [<x> <z>], /gamerule <rule> [<value>], "
                       "/time set|add <time>, /time query daytime|gametime|day, /weather clear|rain|thunder [<duration>], /kill [<own name>], /summon <entity> <x> <y> <z> [{...}], "
                       "/setblock <x> <y> <z> <block>, /xp add|set <player> <amount> [levels|points], /xp query <player> levels|points, "
                       "/effect give <targets> <effect> [<seconds>|infinite] [<amplifier>] [<hideParticles>], /effect clear [<targets>] [<effect>], /tps" + extra);
        } else if (args[0] == "players") {
            std::string names;
            for (const auto& [id, o] : players) names += (names.empty() ? "" : ", ") + o.name;
            message(p, std::format("{} online: {}", players.size(), names));
        } else if (args[0] == "tp" && (args.size() == 4 || ((args.size() == 5 || args.size() == 7) && args[1] == p.name))) {
            // `/tp <x> <y> <z>`, or `/tp <own name> <x> <y> <z> [<yaw> <pitch>]`.
            // A whole number for x or z is the middle of its block, as on the
            // official servers (client/maps).
            const std::size_t first = args.size() == 7 ? 2 : args.size() - 3;
            const auto coordinate = [](const std::string& text, bool centered) {
                const auto v = std::stod(text);
                return centered && text.find('.') == std::string::npos ? v + 0.5 : v;
            };
            Vec3 to;
            std::optional<std::pair<float, float>> rotation;
            try {
                to = {coordinate(args[first], true), coordinate(args[first + 1], false), coordinate(args[first + 2], true)};
                if (args.size() == 7) rotation = std::pair{std::stof(args[5]), std::stof(args[6])};
            } catch (const std::exception&) {
                message(p, "Usage: /tp <x> <y> <z>", "red");
                return;
            }
            if (rotation && (!std::isfinite(rotation->first) || !std::isfinite(rotation->second))) {
                message(p, "Usage: /tp <x> <y> <z>", "red");
                return;
            }
            if (rotation) {
                p.yaw = std::fmod(rotation->first, 360.0f);
                p.pitch = std::clamp(rotation->second, -90.0f, 90.0f);
            }
            if (!finite(to) || std::abs(to.x) >= 3.0e7 || std::abs(to.z) >= 3.0e7 || std::abs(to.y) > 4096.0) {
                message(p, "Destination out of range", "red");
                return;
            }
            const auto chunk = chunk_at(to);
            teleport(p, to);
            p.fall_distance = 0.0;
            p.center = chunk;
            p.peer->send(wire.chunk_center(chunk.x, chunk.z));
            move_tickets(p, chunk);
        } else if (args[0] == "gamemode" && args.size() == 2
                   && (args[1] == "survival" || args[1] == "creative" || args[1] == "spectator")) {
            const auto mode = args[1] == "survival" ? survival : args[1] == "creative" ? creative : spectator;
            set_game_mode(p, mode);
        } else if (args[0] == "forceload") {
            forceload(p, std::span(args).subspan(1));
        } else if (args[0] == "gamerule" && c.text.starts_with("gamerule")) {
            gamerule(p, c.text);
        } else if (args[0] == "effect" && c.text.starts_with("effect")) {
            effect(p, c.text);
        } else if (args[0] == "time" && c.text.starts_with("time")) {
            time(p, c.text);
        } else if (args[0] == "weather" && c.text.starts_with("weather")) {
            weather(p, c.text);
        } else if (args[0] == "kill" && (args.size() == 1 || (args.size() == 2 && args[1] == p.name))) {
            // `/kill` or `/kill <own name>`: the generic kill, whatever the
            // game mode.
            if (!p.dead) hurt(p, std::numeric_limits<float>::max(), Harm::kill);
            send_translation(p, "commands.kill.success.single", string_arguments({p.name}));
        } else if (args[0] == "summon" && c.text.starts_with("summon")) {
            // The entities the rules summon, at absolute coordinates.
            auto summon = evidence::parse_summon(c.text);
            if (!summon) {
                message(p, summon.error(), "red");
                return;
            }
            const auto type = summon->type;
            admit(std::move(*summon), overworld);
            message(p, std::format("Summoned new {}", type));
        } else if (args[0] == "setblock" && args.size() == 5) {
            setblock(p, args);
        } else if (args[0] == "fill" && args.size() == 8) {
            fill(p, args);
        } else if (args[0] == "recipe") {
            recipe_command(p, args);
        } else if ((args[0] == "xp" || args[0] == "experience") && c.text.starts_with(args[0])) {
            experience(p, c.text, args[0]);
        } else if (args[0] == "tps") {
            message(p, std::format("tick {}", engine->time()));
        } else if (const auto reply = extensions->command(p.name, args[0], std::span(args).subspan(1))) {
            if (!reply->empty()) message(p, *reply, "white");
        } else {
            message(p, std::format("Unknown command: /{}", args[0]), "red");
        }
    }

    // The spawn chunks: a start ticket of level 32 - spawnChunkRadius at the
    // spawn chunk, none at radius 0 (observed on both official servers).
    void add_start_ticket() {
        const auto radius = gameplay::game_rule_value(bundle, rules, "spawnChunkRadius");
        if (radius > 0)
            admit(contracts::AddTicket{spawn_ticket, contracts::chunk_of(meta.spawn), 0, contracts::TicketType::start,
                                       static_cast<std::uint8_t>(32 - radius), 0},
                  overworld);
    }

    // `/gamerule <rule> [<value>]`: queries or sets a game rule of the
    // version: an integer (only spawnChunkRadius is bounded, 0-32) or a
    // boolean, read as both official servers read them (command_reader.hpp).
    void gamerule(Player& p, std::string_view input) {
        CommandReader r(input, std::string_view("gamerule").size());
        if (r.done()) return syntax_error(p, input, r.incomplete());
        if (auto e = r.child()) return syntax_error(p, input, *e);
        const auto name = r.word();
        const auto* rule = bundle.schema.game_rule(name);
        if (name.empty() || !rule || rule->experimental) return syntax_error(p, input, r.error("command.unknown.argument"));
        r.skip(name.size());
        const bool boolean = rule->type == contracts::GameRuleType::boolean;
        const auto text = [&](std::int32_t v) { return boolean ? std::string(v != 0 ? "true" : "false") : std::to_string(v); };
        if (r.done())
            return send_translation(p, "commands.gamerule.query",
                                    string_arguments({std::string(name), text(gameplay::game_rule_value(bundle, rules, name))}));
        if (auto e = r.child()) return syntax_error(p, input, *e);
        std::int32_t value{};
        if (boolean) {
            const auto b = r.boolean();
            if (!b) return syntax_error(p, input, b.error());
            value = *b ? 1 : 0;
        } else {
            const auto range = gameplay::game_rule_range(name);
            const auto v = r.integer(range.min, range.max);
            if (!v) return syntax_error(p, input, v.error());
            value = *v;
        }
        if (auto e = r.end()) return syntax_error(p, input, *e);
        const contracts::SetGameRule set{std::string(name), value};
        if (!gameplay::check_game_rule(bundle, set)) return; // the reading above refuses every invalid value
        admit(set);
        const auto before = gameplay::game_rule_value(bundle, rules, name);
        gameplay::set_game_rule(bundle, rules, set);
        // The spawn chunks follow their radius in the same tick.
        if (name == "spawnChunkRadius" && value != before) {
            if (before > 0) admit(contracts::RemoveTicket{spawn_ticket}, overworld);
            add_start_ticket();
        }
        send_translation(p, "commands.gamerule.set", string_arguments({set.name, text(value)}));
    }

    // `/weather clear|rain|thunder [<duration>]`: a duration of at least one
    // tick, or the kind's default drawn by the rules. The weather changes in
    // the coming tick; its levels reach players as they change.
    void weather(Player& p, std::string_view input) {
        CommandReader r(input, std::string_view("weather").size());
        if (r.done()) return syntax_error(p, input, r.incomplete());
        if (auto e = r.child()) return syntax_error(p, input, *e);
        const auto name = r.word();
        contracts::WeatherKind kind{};
        if (name == "clear") kind = contracts::WeatherKind::clear;
        else if (name == "rain") kind = contracts::WeatherKind::rain;
        else if (name == "thunder") kind = contracts::WeatherKind::thunder;
        else return syntax_error(p, input, r.error("command.unknown.argument"));
        r.skip(name.size());
        std::int32_t duration = 0;
        if (!r.done()) {
            if (auto e = r.child()) return syntax_error(p, input, *e);
            const auto ticks = r.time(1);
            if (!ticks) return syntax_error(p, input, ticks.error());
            duration = *ticks;
            if (auto e = r.end()) return syntax_error(p, input, *e);
        }
        admit(contracts::SetWeather{kind, duration});
        send_translation(p, std::format("commands.weather.set.{}", name), nbt::Tag::list(nbt::TagType::string));
    }

    // `/setblock <x> <y> <z> <block>`: a block state in full notation (every
    // property, as the fixtures' notation writes them) at whole or `~`
    // coordinates, placed as a set command places it (its block entity
    // empty).
    // A block position from three arguments, absolute or relative (~) to the
    // player's block.
    std::optional<BlockPos> block_argument(const Player& p, std::span<const std::string> args) const {
        std::array<std::int32_t, 3> coords{};
        const std::array<double, 3> here{p.pos.x, p.pos.y, p.pos.z};
        for (std::size_t k = 0; k < 3; ++k) {
            std::string_view a = args[k];
            const bool relative = a.starts_with('~');
            if (relative) a.remove_prefix(1);
            std::int32_t v = 0;
            if (!a.empty() && std::from_chars(a.data(), a.data() + a.size(), v).ec != std::errc{}) return std::nullopt;
            coords[k] = relative ? static_cast<std::int32_t>(std::floor(here[k])) + v : v;
        }
        return BlockPos{coords[0], coords[1], coords[2]};
    }
    void setblock(Player& p, const std::vector<std::string>& args) {
        const auto at_arg = block_argument(p, std::span(args).subspan(1, 3));
        if (!at_arg) return message(p, "Usage: /setblock <x> <y> <z> <block>", "red");
        const BlockPos pos = *at_arg;
        auto state = mapping.block(args[4].find(':') == std::string::npos ? "minecraft:" + args[4] : args[4]);
        if (!state) return message(p, state.error(), "red");
        if (!bundle.world.contains_y(pos.y)) return message(p, "Cannot place blocks outside of the world", "red");
        const auto pending = pending_setblocks.find(pos);
        if ((pending != pending_setblocks.end() ? pending->second : at(pos)) == *state)
            return send_translation(p, "commands.setblock.failed", nbt::Tag::list(nbt::TagType::string));
        admit(contracts::SetBlock{pos, state->raw(), std::nullopt});
        pending_setblocks[pos] = *state;
        send_translation(p, "commands.setblock.success", int_arguments({pos.x, pos.y, pos.z}));
    }
    // `/fill <from> <to> <block>`, replacing (the default): every block of the
    // box that differs is set as /setblock sets one, x fastest, then z, then
    // y; at most 32768 blocks, with both official servers' feedback.
    void fill(Player& p, const std::vector<std::string>& args) {
        const auto a = block_argument(p, std::span(args).subspan(1, 3));
        const auto b = block_argument(p, std::span(args).subspan(4, 3));
        if (!a || !b) return message(p, "Usage: /fill <x1> <y1> <z1> <x2> <y2> <z2> <block>", "red");
        const BlockPos low{std::min(a->x, b->x), std::min(a->y, b->y), std::min(a->z, b->z)};
        const BlockPos high{std::max(a->x, b->x), std::max(a->y, b->y), std::max(a->z, b->z)};
        const auto volume = (std::int64_t{high.x} - low.x + 1) * (std::int64_t{high.y} - low.y + 1) * (std::int64_t{high.z} - low.z + 1);
        constexpr std::int64_t limit = 32768;
        if (volume > limit) return send_translation(p, "commands.fill.toobig", int_arguments({static_cast<std::int32_t>(limit), static_cast<std::int32_t>(std::min<std::int64_t>(volume, INT32_MAX))}));
        auto state = mapping.block(args[7].find(':') == std::string::npos ? "minecraft:" + args[7] : args[7]);
        if (!state) return message(p, state.error(), "red");
        if (!bundle.world.contains_y(low.y) || !bundle.world.contains_y(high.y)) return message(p, "Cannot place blocks outside of the world", "red");
        std::int32_t count = 0;
        for (auto y = low.y; y <= high.y; ++y)
            for (auto z = low.z; z <= high.z; ++z)
                for (auto x = low.x; x <= high.x; ++x) {
                    const BlockPos pos{x, y, z};
                    const auto pending = pending_setblocks.find(pos);
                    if ((pending != pending_setblocks.end() ? pending->second : at(pos)) == *state) continue;
                    admit(contracts::SetBlock{pos, state->raw(), std::nullopt});
                    pending_setblocks[pos] = *state;
                    ++count;
                }
        if (count == 0) return send_translation(p, "commands.fill.failed", nbt::Tag::list(nbt::TagType::string));
        send_translation(p, "commands.fill.success", int_arguments({count}));
    }

    // `/xp` (or `/experience`) `add|set <player> <amount> [levels|points]`
    // and `query <player> levels|points`: a player by name, or @s or @p for
    // the player themself. Setting points stays below what the level needs.
    void experience(Player& p, std::string_view input, std::string_view literal) {
        CommandReader r(input, literal.size());
        if (r.done()) return syntax_error(p, input, r.incomplete());
        if (auto e = r.child()) return syntax_error(p, input, *e);
        const auto action = r.word();
        if (action != "add" && action != "set" && action != "query") return syntax_error(p, input, r.error("command.unknown.argument"));
        r.skip(action.size());
        if (r.done()) return syntax_error(p, input, r.incomplete());
        if (auto e = r.child()) return syntax_error(p, input, *e);
        const auto name = std::string(r.word());
        r.skip(name.size());
        Player* target = name == "@s" || name == "@p" ? &p : nullptr;
        for (auto& [id, o] : players)
            if (!target && o.name == name) target = &o;
        if (r.done()) return syntax_error(p, input, r.incomplete());
        if (auto e = r.child()) return syntax_error(p, input, *e);
        std::int32_t amount = 0;
        if (action != "query") {
            const auto value = r.integer(action == "set" ? 0 : std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max());
            if (!value) return syntax_error(p, input, value.error());
            amount = *value;
        }
        bool levels = false;
        if (action == "query" || !r.done()) {
            if (action != "query")
                if (auto e = r.child()) return syntax_error(p, input, *e);
            const auto unit = r.word();
            if (unit != "levels" && unit != "points") return syntax_error(p, input, r.error("command.unknown.argument"));
            levels = unit == "levels";
            r.skip(unit.size());
        }
        if (auto e = r.end()) return syntax_error(p, input, *e);
        if (!target) return message(p, "No player was found", "red");
        const auto view = engine->view();
        const auto inv = view.player(key_of(*target));
        const auto level = inv && *inv ? (*inv)->xp_level : 0;
        const auto unit = levels ? "levels" : "points";
        if (action == "query") {
            const auto value = levels ? level
                                      : inv && *inv ? static_cast<std::int32_t>((*inv)->xp_progress * static_cast<float>(gameplay::experience::needed(level)))
                                                    : 0;
            return send_translation(p, std::format("commands.experience.query.{}", unit), string_arguments({target->name, std::to_string(value)}));
        }
        if (action == "set" && !levels && amount >= gameplay::experience::needed(level))
            return message(p, "Cannot set experience points above the maximum points for the player's current level", "red");
        admit(contracts::ChangeExperience{key_of(*target), amount, levels, action == "set"});
        send_translation(p, std::format("commands.experience.{}.{}.success.single", action, unit),
                         string_arguments({std::to_string(amount), target->name}));
    }

    // `/effect give <targets> <effect> [<seconds>|infinite] [<amplifier>]
    // [<hideParticles>]` and `/effect clear [<targets>] [<effect>]`, as both
    // official servers answer them (client/effects). Targets are players by
    // name, or @s and @p for the player themself, @a and @e for every player
    // (the only living entities here). An instant effect's seconds count
    // ticks (one by default); the others last 30 seconds by default.
    void effect(Player& p, std::string_view input) {
        CommandReader r(input, std::string_view("effect").size());
        if (r.done()) return syntax_error(p, input, r.incomplete());
        if (auto e = r.child()) return syntax_error(p, input, *e);
        const auto action = r.word();
        if (action != "give" && action != "clear") return syntax_error(p, input, r.error("command.unknown.argument"));
        r.skip(action.size());
        const bool give = action == "give";
        std::string targets = "@s";
        std::optional<std::uint8_t> which;
        std::int32_t seconds = 0, amplifier = 0;
        bool timed = false, forever = false, hidden = false;
        if (!r.done()) {
            if (auto e = r.child()) return syntax_error(p, input, *e);
            targets = std::string(r.word());
            r.skip(targets.size());
        } else if (give) {
            return syntax_error(p, input, r.incomplete());
        }
        if (!r.done()) {
            if (auto e = r.child()) return syntax_error(p, input, *e);
            auto name = std::string(r.word());
            r.skip(name.size());
            if (name.find(':') == std::string::npos) name = "minecraft:" + name;
            which = gameplay::effects::id(name);
            if (!which) return syntax_error(p, input, r.error("argument.resource.not_found", string_arguments({name, "minecraft:mob_effect"})));
        } else if (give) {
            return syntax_error(p, input, r.incomplete());
        }
        if (give && !r.done()) {
            if (auto e = r.child()) return syntax_error(p, input, *e);
            if (r.word() == "infinite") {
                r.skip(std::string_view("infinite").size());
                forever = true;
            } else {
                const auto value = r.integer(1, 1000000);
                if (!value) return syntax_error(p, input, value.error());
                seconds = *value;
                timed = true;
            }
            if (!r.done()) {
                if (auto e = r.child()) return syntax_error(p, input, *e);
                const auto value = r.integer(0, 255);
                if (!value) return syntax_error(p, input, value.error());
                amplifier = *value;
            }
            if (!r.done()) {
                if (auto e = r.child()) return syntax_error(p, input, *e);
                const auto value = r.boolean();
                if (!value) return syntax_error(p, input, value.error());
                hidden = *value;
            }
        }
        if (auto e = r.end()) return syntax_error(p, input, *e);
        std::vector<Player*> chosen;
        if (targets == "@s" || targets == "@p") chosen.push_back(&p);
        for (auto& [id, o] : players)
            if (targets == "@a" || targets == "@e" || (!targets.starts_with('@') && o.name == targets)) chosen.push_back(&o);
        if (chosen.empty()) return feedback(p, "argument.entity.notfound.entity", {}, true);
        // The effect's name as the client translates it, and a target's name.
        const auto text = [](std::string value) {
            auto t = nbt::Tag::compound();
            t.set("text", nbt::Tag::string(std::move(value)));
            return t;
        };
        const auto reply = [&](std::string_view key, bool named, std::string target) {
            auto with = nbt::Tag::list(nbt::TagType::compound);
            if (named) {
                auto name = nbt::Tag::compound();
                name.set("translate", nbt::Tag::string(std::format("effect.minecraft.{}", gameplay::effects::names()[*which])));
                with.push(std::move(name));
            }
            with.push(text(std::move(target)));
            send_translation(p, key, std::move(with));
        };
        const auto many = chosen.size() > 1;
        const auto target = many ? std::to_string(chosen.size()) : chosen.front()->name;
        std::size_t done = 0;
        if (give) {
            const bool instant = gameplay::effects::instant(*which);
            const auto duration = forever ? gameplay::effects::infinite : timed ? (instant ? seconds : seconds * 20) : instant ? 1 : 600;
            for (auto* t : chosen)
                if (give_effect(*t, *which, {amplifier, duration, false, !hidden, !hidden})) ++done;
            if (done == 0) return feedback(p, "commands.effect.give.failed", {}, true);
            return reply(many ? "commands.effect.give.success.multiple" : "commands.effect.give.success.single", true, target);
        }
        for (auto* t : chosen)
            if (clear_effects(*t, which)) ++done;
        const std::string_view kind = which ? "specific" : "everything";
        if (done == 0) return feedback(p, std::format("commands.effect.clear.{}.failed", kind), {}, true);
        reply(std::format("commands.effect.clear.{}.success.{}", kind, many ? "multiple" : "single"), which.has_value(), target);
    }

    // The day time with the commands of the coming tick applied.
    std::int64_t day_time() const { return pending_day_time.value_or(engine->day_time()); }
    bool daylight_cycle() const { return gameplay::game_rule_value(bundle, rules, "doDaylightCycle") != 0; }
    Bytes time_packet() const { return wire.set_time(static_cast<std::int64_t>(engine->time()), day_time(), daylight_cycle()); }

    // `/time set <time>|day|noon|night|midnight`, `/time add <time>` and
    // `/time query daytime|gametime|day`, as both official servers answer
    // them. A change applies before the coming tick advances the day and is
    // sent to every player at once.
    void time(Player& p, std::string_view input) {
        CommandReader r(input, std::string_view("time").size());
        if (r.done()) return syntax_error(p, input, r.incomplete());
        if (auto e = r.child()) return syntax_error(p, input, *e);
        const auto action = r.word();
        if (action != "set" && action != "add" && action != "query") return syntax_error(p, input, r.error("command.unknown.argument"));
        r.skip(action.size());
        if (r.done()) return syntax_error(p, input, r.incomplete());
        if (auto e = r.child()) return syntax_error(p, input, *e);
        constexpr std::int64_t day = 24000, int_max = INT32_MAX;
        if (action == "query") {
            const auto what = r.word();
            std::int64_t value{};
            if (what == "daytime") value = day_time() % day;
            else if (what == "gametime") value = static_cast<std::int64_t>(engine->time() % int_max);
            else if (what == "day") value = day_time() / day % int_max;
            else return syntax_error(p, input, r.error("command.unknown.argument"));
            r.skip(what.size());
            if (auto e = r.end()) return syntax_error(p, input, *e);
            return send_translation(p, "commands.time.query", int_arguments({static_cast<std::int32_t>(value)}));
        }
        std::int64_t value{};
        std::int32_t reply{};
        static constexpr std::pair<std::string_view, std::int32_t> markers[]{{"day", 1000}, {"noon", 6000}, {"night", 13000}, {"midnight", 18000}};
        const auto marker = std::ranges::find(markers, r.word(), &std::pair<std::string_view, std::int32_t>::first);
        if (action == "set" && marker != std::end(markers)) {
            r.skip(marker->first.size());
            value = reply = marker->second;
        } else {
            const auto ticks = r.time();
            if (!ticks) return syntax_error(p, input, ticks.error());
            // Setting replies with the time set, adding with the new time of day.
            value = action == "set" ? *ticks : day_time() + *ticks;
            reply = action == "set" ? *ticks : static_cast<std::int32_t>(value % day);
        }
        if (auto e = r.end()) return syntax_error(p, input, *e);
        admit(contracts::SetDayTime{value});
        pending_day_time = value;
        broadcast(time_packet());
        send_translation(p, "commands.time.set", int_arguments({reply}));
    }

    // A translated message whose arguments are plain strings or numbers.
    static nbt::Tag translation(std::string_view key, nbt::Tag with) {
        auto t = nbt::Tag::compound();
        t.set("translate", nbt::Tag::string(std::string(key)));
        if (!with.items().empty()) t.set("with", std::move(with));
        return t;
    }

    void send_translation(Player& p, std::string_view key, nbt::Tag with) { p.peer->send(wire.system_chat(translation(key, std::move(with)))); }

    // A command the parser refused, as both official servers report it: the
    // error in red, then the input up to where reading stopped (its last ten
    // characters, after "..." when there are more), the rest underlined and
    // "<--[HERE]", suggesting the command again when clicked.
    void syntax_error(Player& p, std::string_view input, CommandError error) {
        const auto cursor = error.cursor;
        const auto plain = [](std::string_view text) {
            auto t = nbt::Tag::compound();
            t.set("text", nbt::Tag::string(std::string(text)));
            return t;
        };
        const auto wrapped = [&](nbt::Tag inner, std::string_view color) {
            auto t = plain("");
            t.set("color", nbt::Tag::string(std::string(color)));
            auto extra = nbt::Tag::list(nbt::TagType::compound);
            extra.push(std::move(inner));
            t.set("extra", std::move(extra));
            return t;
        };
        p.peer->send(wire.system_chat(wrapped(translation(error.key, std::move(error.with)), "red")));
        auto parts = nbt::Tag::list(nbt::TagType::compound);
        if (cursor > 10) parts.push(plain("..."));
        parts.push(plain(input.substr(cursor > 10 ? cursor - 10 : 0, std::min<std::size_t>(cursor, 10))));
        if (cursor < input.size()) {
            auto rest = plain(input.substr(cursor));
            rest.set("color", nbt::Tag::string("red"));
            rest.set("underlined", nbt::Tag::boolean(true));
            parts.push(std::move(rest));
        }
        auto here = translation("command.context.here", nbt::Tag::list(nbt::TagType::string));
        here.set("color", nbt::Tag::string("red"));
        here.set("italic", nbt::Tag::boolean(true));
        parts.push(std::move(here));
        auto context = plain("");
        context.set("color", nbt::Tag::string("gray"));
        context.set("extra", std::move(parts));
        // Since 1.21.5 (protocol 770) click events are snake_case and name their command.
        auto click = nbt::Tag::compound();
        click.set("action", nbt::Tag::string("suggest_command"));
        click.set(wire.protocol() >= 770 ? "command" : "value", nbt::Tag::string("/" + std::string(input)));
        context.set(wire.protocol() >= 770 ? "click_event" : "clickEvent", std::move(click));
        auto line = plain("");
        line.set("color", nbt::Tag::string("red"));
        auto extra = nbt::Tag::list(nbt::TagType::compound);
        extra.push(std::move(context));
        line.set("extra", std::move(extra));
        p.peer->send(wire.system_chat(line));
    }

    // A translated message, as the target versions word their command
    // feedback (failures in red).
    void feedback(Player& p, std::string_view key, std::vector<std::string> args, bool failure = false) {
        auto with = nbt::Tag::list(nbt::TagType::compound);
        for (auto& a : args) {
            auto part = nbt::Tag::compound();
            part.set("text", nbt::Tag::string(std::move(a)));
            with.push(std::move(part));
        }
        auto text = nbt::Tag::compound();
        text.set("translate", nbt::Tag::string(std::string(key)));
        if (!args.empty()) text.set("with", std::move(with));
        if (failure) text.set("color", nbt::Tag::string("red"));
        p.peer->send(wire.system_chat(text));
    }

    // `/forceload` as in the target versions: block columns name chunks;
    // each force-loaded chunk is a forced ticket of level 31 that persists.
    void forceload(Player& p, std::span<const std::string> a) {
        static constexpr std::string_view level = "minecraft:overworld";
        const auto column = [&](std::string_view text, double own) -> std::optional<std::int32_t> {
            double base = 0.0;
            if (!text.empty() && text.front() == '~') {
                base = own;
                text.remove_prefix(1);
                if (text.empty()) return static_cast<std::int32_t>(std::floor(base));
            }
            std::int64_t v{};
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
            if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
            const double block = std::floor(base) + static_cast<double>(v);
            if (block <= -3.0e7 || block >= 3.0e7) return std::nullopt;
            return static_cast<std::int32_t>(block);
        };
        const auto chunk = [&](std::span<const std::string> xz) -> std::optional<ChunkPos> {
            const auto x = column(xz[0], p.pos.x), z = column(xz[1], p.pos.z);
            if (!x || !z) return std::nullopt;
            return contracts::chunk_of({*x, 0, *z});
        };
        const auto name = [](ChunkPos c) { return std::format("[{}, {}]", c.x, c.z); };
        const auto usage = [&] {
            message(p, "Usage: /forceload add|remove <x> <z> [<x> <z>], /forceload remove all, /forceload query [<x> <z>]", "red");
        };
        if (a.empty()) return usage();
        if (a[0] == "query") {
            if (a.size() == 1) {
                if (forced.empty()) return feedback(p, "commands.forceload.added.none", {std::string(level)});
                std::string list;
                for (const auto& [c, id] : forced) list += (list.empty() ? "" : ", ") + name(c);
                if (forced.size() == 1) return feedback(p, "commands.forceload.list.single", {std::string(level), list});
                return feedback(p, "commands.forceload.list.multiple", {std::to_string(forced.size()), std::string(level), list});
            }
            if (a.size() != 3) return usage();
            const auto c = chunk(a.subspan(1, 2));
            if (!c) return usage();
            if (forced.contains(*c)) return feedback(p, "commands.forceload.query.success", {name(*c), std::string(level)});
            return feedback(p, "commands.forceload.query.failure", {name(*c), std::string(level)}, true);
        }
        const bool add = a[0] == "add";
        if (!add && a[0] != "remove") return usage();
        if (!add && a.size() == 2 && a[1] == "all") {
            for (const auto& [c, id] : forced) admit(contracts::RemoveTicket{id}, overworld);
            forced.clear();
            return feedback(p, "commands.forceload.removed.all", {std::string(level)});
        }
        if (a.size() != 3 && a.size() != 5) return usage();
        const auto from = chunk(a.subspan(1, 2));
        const auto to = a.size() == 5 ? chunk(a.subspan(3, 2)) : from;
        if (!from || !to) return usage();
        const ChunkPos lo{std::min(from->x, to->x), std::min(from->z, to->z)}, hi{std::max(from->x, to->x), std::max(from->z, to->z)};
        const std::int64_t count = (std::int64_t{hi.x} - lo.x + 1) * (std::int64_t{hi.z} - lo.z + 1);
        if (count > 256) return feedback(p, "commands.forceload.toobig", {"256", std::to_string(count)}, true);
        std::size_t changed = 0;
        ChunkPos first{};
        for (auto x = lo.x; x <= hi.x; ++x)
            for (auto z = lo.z; z <= hi.z; ++z) {
                const ChunkPos c{x, z};
                if (add == forced.contains(c)) continue;
                if (changed++ == 0) first = c;
                if (add) {
                    forced[c] = next_ticket;
                    admit(contracts::AddTicket{next_ticket++, c, 0, contracts::TicketType::forced, 31, 0}, overworld);
                } else {
                    admit(contracts::RemoveTicket{forced.at(c)}, overworld);
                    forced.erase(c);
                }
            }
        const std::string verb = add ? "added" : "removed";
        if (changed == 0) return feedback(p, std::format("commands.forceload.{}.failure", verb), {}, true);
        if (changed == 1) return feedback(p, std::format("commands.forceload.{}.single", verb), {name(first), std::string(level)});
        feedback(p, std::format("commands.forceload.{}.multiple", verb), {std::to_string(changed), std::string(level), name(lo), name(hi)});
    }

    // ---- publishing ---------------------------------------------------------

    // The rain and thunder levels when they change, and the start or end of
    // rain (a rain level above 0.2) with both levels, as both official
    // servers send them in the tick.
    void publish_weather() {
        const auto& w = engine->weather();
        if (w.rain_level != sent_weather.rain_level) broadcast(wire.game_event(7, w.rain_level));
        if (w.thunder_level != sent_weather.thunder_level) broadcast(wire.game_event(8, w.thunder_level));
        if (w.rain_visible() != sent_weather.rain_visible()) {
            broadcast(wire.game_event(w.rain_visible() ? 1 : 2, 0.0f));
            broadcast(wire.game_event(7, w.rain_level));
            broadcast(wire.game_event(8, w.thunder_level));
        }
        sent_weather = w;
    }

    void publish_blocks() {
        std::set<BlockPos> changed, beacons;
        std::set<ChunkPos> unloaded;
        for (const auto& r : engine->trace()) {
            if (r.dimension != overworld) continue; // no player is in another level
            if (r.kind == contracts::TraceKind::block_changed) changed.insert(r.pos);
            // A beacon set where there was none (its block entity is new).
            constexpr auto beacon = static_cast<std::uint64_t>(BlockType::beacon); // a raw state's low byte is its type
            if (r.kind == contracts::TraceKind::block_changed && (r.b & 0xFF) == beacon && (r.a & 0xFF) != beacon) beacons.insert(r.pos);
            if (r.kind == contracts::TraceKind::chunk_unloaded) unloaded.insert(contracts::chunk_of(r.pos));
        }
        // Every displayed property is rule state: the changes are what clients need.
        const auto look_up = lookup();
        for (const auto b : changed) {
            const auto c = contracts::chunk_of(b);
            std::optional<Bytes> packet;
            for (auto& [id, p] : players) {
                if (!p.sent.contains(c)) continue;
                if (!packet) packet = wire.block_update(wire_pos(b), display_state(palette, look_up, b, at(b)));
                p.peer->send(*packet);
            }
        }
        // A new beacon's block entity data follows its block, as both
        // official servers send it: its levels, none yet (client/beacon; the
        // beacon block entity type, 15 on both protocols).
        for (const auto b : beacons) {
            if (!at(b).is(BlockType::beacon)) continue;
            auto tag = protocol::nbt::Tag::compound();
            tag.set("Levels", protocol::nbt::Tag::int_(0));
            const auto packet = wire.block_entity_data(wire_pos(b), 15, tag);
            for (auto& [id, p] : players)
                if (p.sent.contains(contracts::chunk_of(b))) p.peer->send(packet);
        }
        for (auto& [id, p] : players)
            for (const auto c : unloaded)
                if (p.sent.erase(c)) forget(p, c);
    }

    // The level events the rules played, to players within 64 blocks, after
    // the entities that appeared in the tick (client/armor2: an item a
    // dispenser ejects appears before its sound and smoke).
    void publish_level_events() {
        for (const auto& r : engine->trace()) {
            if (r.dimension != overworld || r.kind != contracts::TraceKind::level_event) continue;
            const auto packet = wire.level_event(static_cast<std::int32_t>(r.a), wire_pos(r.pos), static_cast<std::int32_t>(r.b));
            for (auto& [id, p] : players) {
                const double dx = p.pos.x - (r.pos.x + 0.5), dy = p.pos.y - (r.pos.y + 0.5), dz = p.pos.z - (r.pos.z + 0.5);
                if (dx * dx + dy * dy + dz * dz < 4096.0) p.peer->send(packet);
            }
        }
    }
    // Block events that ran: chests' lids, to players within 64 blocks that
    // have the chunk, as the official servers send them.
    void publish_block_events() {
        for (const auto& r : engine->trace()) {
            if (r.dimension != overworld) continue;
            // Sounds the rules played, to players within 16 blocks (a sound
            // of volume 1 carries that far).
            if (r.kind == contracts::TraceKind::sound_played) {
                const auto packet = wire.sound(static_cast<contracts::SoundEvent>(r.a), r.pos, static_cast<std::int64_t>(sound_seed++));
                for (auto& [id, p] : players) {
                    const double dx = p.pos.x - (r.pos.x + 0.5), dy = p.pos.y - (r.pos.y + 0.5), dz = p.pos.z - (r.pos.z + 0.5);
                    if (dx * dx + dy * dy + dz * dz < 256.0) p.peer->send(packet);
                }
                continue;
            }
            if (r.kind == contracts::TraceKind::beacon_effects) {
                give_beacon_effects(r.pos, static_cast<std::uint8_t>(r.a), static_cast<std::uint8_t>(r.b), static_cast<std::uint8_t>(r.c));
                continue;
            }
            if (r.kind == contracts::TraceKind::block_event_queued) {
                auto& params = queued_events[{r.pos, r.a, r.b}];
                params.push_back(static_cast<std::uint8_t>(r.c));
                continue;
            }
            if (r.kind != contracts::TraceKind::block_event_executed) continue;
            const auto it = queued_events.find({r.pos, r.a, r.b});
            if (it == queued_events.end()) continue;
            const auto param = it->second.front();
            it->second.pop_front();
            if (it->second.empty()) queued_events.erase(it);
            const bool ender = r.a == static_cast<std::uint64_t>(BlockType::ender_chest);
            const bool shulker = r.a == static_cast<std::uint64_t>(BlockType::shulker_box);
            if (r.c == 0 || (r.a != static_cast<std::uint64_t>(BlockType::chest) && !ender && !shulker)) continue;
            const auto block = shulker ? shulker_blocks.at(at(r.pos).variant() % shulker_blocks.size())
                             : ender  ? ender_chest_block : at(r.pos).variant() == 1 ? trapped_chest_block : chest_block;
            const auto packet = wire.block_event(wire_pos(r.pos), static_cast<std::uint8_t>(r.b), param, block);
            const auto c = contracts::chunk_of(r.pos);
            for (auto& [id, p] : players) {
                const double dx = p.pos.x - (r.pos.x + 0.5), dy = p.pos.y - (r.pos.y + 0.5), dz = p.pos.z - (r.pos.z + 0.5);
                if (p.sent.contains(c) && dx * dx + dy * dy + dz * dz < 4096.0) p.peer->send(packet);
            }
        }
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
            t.last = e.position;
        }
        if (!t.viewers.insert(p.peer->id()).second) return;
        if (e.kind == gameplay::EntityKind::experience_orb) {
            p.peer->send(wire.experience_orb(t.protocol_id, entity_uuid(engine_id), orb_entity_type, t.last, e.experience));
            if (wire.orbs_are_entities()) p.peer->send(wire.orb_metadata(t.protocol_id, e.experience));
            return;
        }
        if (e.kind == gameplay::EntityKind::falling_block) {
            // The spawn data of a falling block is its block state.
            p.peer->send(wire.spawn_entity(t.protocol_id, entity_uuid(engine_id), falling_block_type, t.last, 0, 0, 0,
                                           palette.wire(e.carried), e.velocity));
            return;
        }
        if (e.kind == gameplay::EntityKind::minecart || e.kind == gameplay::EntityKind::boat) {
            const auto type = e.kind == gameplay::EntityKind::minecart ? minecart_entity_type : boat_entity_types.at(e.variant);
            p.peer->send(wire.spawn_entity(t.protocol_id, entity_uuid(engine_id), type, t.last, 0, 0, 0, 0, e.velocity));
            return;
        }
        if (e.kind == gameplay::EntityKind::arrow || e.kind == gameplay::EntityKind::snowball) {
            // No owner (spawn data 0); clients fly them themselves.
            p.peer->send(wire.spawn_entity(t.protocol_id, entity_uuid(engine_id),
                                           e.kind == gameplay::EntityKind::arrow ? arrow_entity_type : snowball_entity_type, t.last, 0, 0, 0, 0,
                                           e.velocity));
            return;
        }
        if (e.kind == gameplay::EntityKind::primed_tnt) {
            // Clients count the fuse down themselves from the value they get
            // (the official servers send it only when it is not 80, and again
            // each tick).
            p.peer->send(wire.spawn_entity(t.protocol_id, entity_uuid(engine_id), tnt_entity_type, t.last, 0, 0, 0, 0, e.velocity));
            p.peer->send(wire.tnt_metadata(t.protocol_id, e.fuse));
            return;
        }
        p.peer->send(wire.spawn_entity(t.protocol_id, entity_uuid(engine_id), item_entity_type, t.last, 0, 0, 0, 0, e.velocity));
        p.peer->send(wire.item_metadata(t.protocol_id, wire_stack(e.stack)));
    }

    void publish_entities() {
        const auto view = engine->view();
        std::set<std::uint64_t> touched;
        for (const auto& r : engine->trace()) {
            if (r.dimension != overworld) continue;
            switch (r.kind) {
            case contracts::TraceKind::entity_spawned: case contracts::TraceKind::entity_changed_chunk:
            case contracts::TraceKind::entity_stack_changed: case contracts::TraceKind::entity_loaded:
                touched.insert(r.a);
                break;
            case contracts::TraceKind::explosion: {
                // Players within 64 blocks of it see and hear it, as on the
                // official servers.
                const contracts::Vec3 center{contracts::bits_double(r.a), contracts::bits_double(r.b), contracts::bits_double(r.c)};
                const auto packet = wire.explosion(center);
                for (auto& [pid, p] : players) {
                    const double dx = p.pos.x - center.x, dy = p.pos.y - center.y, dz = p.pos.z - center.z;
                    if (dx * dx + dy * dy + dz * dz < 4096.0) p.peer->send(packet);
                }
                break;
            }
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
            const auto e = view.entity(overworld, EntityId{id});
            if (!e || !*e) continue;
            const auto c = contracts::chunk_of(contracts::block_of((*e)->position));
            for (auto& [pid, p] : players)
                if (p.sent.contains(c)) show_entity(p, id, **e);
            if (const auto it = tracked.find(id); it != tracked.end() && (*e)->kind == gameplay::EntityKind::item) {
                // Stack changes are sent again to everyone who sees the entity.
                const auto meta = wire.item_metadata(it->second.protocol_id, wire_stack((*e)->stack));
                for (const auto viewer : it->second.viewers)
                    if (const auto p = players.find(viewer); p != players.end()) p->second.peer->send(meta);
            }
        }
        // Position sync for moving entities.
        for (auto& [id, t] : tracked) {
            const auto e = view.entity(overworld, EntityId{id});
            if (!e || !*e) continue;
            const auto now = (*e)->position;
            if (now.x == t.last.x && now.y == t.last.y && now.z == t.last.z) continue;
            t.last = now;
            const auto packet = wire.entity_position(t.protocol_id, now, 0, 0, (*e)->on_ground);
            for (const auto viewer : t.viewers)
                if (const auto p = players.find(viewer); p != players.end()) p->second.peer->send(packet);
        }
    }

    // Windows: containers players opened this tick open; windows whose
    // container is gone, changed size or is out of reach close; then every
    // client is told how its window differs from what it shows.
    void publish_inventories() {
        std::vector<std::pair<contracts::PlayerKey, BlockPos>> opened;
        for (const auto& r : engine->trace())
            if (r.kind == contracts::TraceKind::container_opened && r.dimension == overworld) opened.push_back({{r.a, r.b}, r.pos});
        for (auto& [id, p] : players) {
            // A window closed in the tick another opens: what the close put
            // back and the own window's differences go first, as they do a
            // tick apart.
            const bool opens = p.game_mode != spectator && std::ranges::any_of(opened, [&](const auto& o) { return o.first == key_of(p); });
            if (opens && !p.window && p.closing) {
                send_put_back(p);
                sync_inventory(p);
            }
            for (const auto& [key, pos] : opened)
                if (key == key_of(p) && p.game_mode != spectator) open_window(p, pos);
            if (p.window) {
                const auto size = container_slot_count(p.window->pos);
                const bool near = in_reach_plus(p, p.window->pos, 4.0);
                // A lectern's window closes with its book gone.
                const auto block = at(p.window->pos);
                const bool bookless = block.is(BlockType::lectern) && block.extra() == 0;
                if (size != p.window->container_slots || !near || bookless) close_window(p, true);
            }
            send_put_back(p);
            send_ghosts(p);
            sync_inventory(p);
            if (!p.vitals_pending) sync_experience(p);
            sync_maps(p);
            send_joining_data(p); // before the first tick's recipe book entries (client/player_data)
            update_recipe_book(p, engine->view());
            send_joining_vitals(p);
        }
    }

    // A joining player's health, then experience: after the inventory, the
    // position and the first tick's recipe book unlocks, as both official
    // servers send them in the player's first tick (client/join_order).
    // Clients that count from the first health packet have read the
    // inventory and the first tick's recipes.
    // A joining player's effects, then their first entity data (with a
    // returning player's absorption), before the first tick's recipe book
    // entries (client/effects, client/attribute_join, client/player_data).
    void send_joining_data(Player& p) {
        if (!p.vitals_pending || !p.fresh) return;
        for (const auto& e : p.effects) send_effect(p, e, false);
        send_fresh_data(p);
    }
    void send_joining_vitals(Player& p) {
        if (!p.vitals_pending) return;
        send_joining_data(p); // unless sent already (a frozen world has no recipe book triggers)
        p.vitals_pending = false;
        // Then a returning player's attributes, the health and experience.
        if (p.arrival == Player::Arrival::again) send_attributes(p, p.attributes, true, false);
        send_vitals(p);
        sync_experience(p);
    }

    // The filled maps a player's inventory holds (not the cursor), by id.
    std::set<std::int32_t> carried_maps(const runtime::WorldView& view, const Player& p) const {
        std::set<std::int32_t> ids;
        const auto inv = view.player(key_of(p));
        if (!inv || !*inv) return ids;
        for (const auto& s : (*inv)->slots)
            if (s.item == gameplay::items::filled_map && !s.empty())
                if (const auto id = wire.components().varint_of(s.components, "map_id")) ids.insert(*id);
        return ids;
    }
    // Map data for the filled maps a player carries (client/maps and
    // client/maps_explore, both versions identical): the whole map the first
    // time, then each tick the markers when they changed and the rectangle of
    // pixels that changed. A map's markers are those of the players carrying
    // it in its dimension.
    void sync_maps(Player& p) {
        const auto view = engine->view();
        for (const auto id : carried_maps(view, p)) {
            const auto map = view.map(id);
            if (!map || !*map) continue;
            const auto& data = **map;
            std::vector<gameplay::MapMarker> markers;
            for (const auto& [other_id, other] : players)
                if (!other.dead && data.dimension == overworld && carried_maps(view, other).contains(id))
                    if (const auto marker = gameplay::player_marker_on(data, other.pos.x, other.pos.z, other.yaw)) markers.push_back(*marker);
            const auto side = static_cast<std::uint8_t>(gameplay::map_side);
            const auto sent = p.maps_sent.find(id);
            if (sent == p.maps_sent.end()) {
                p.peer->send(wire.map_item_data(id, data.scale, data.locked, std::span<const gameplay::MapMarker>(markers),
                                                {0, 0, side, side, data.colors}));
                p.maps_sent.emplace(id, Player::SentMap{std::move(markers), data.colors});
                continue;
            }
            Wire::MapPatch patch;
            std::size_t x0 = side, x1 = 0, z0 = side, z1 = 0;
            for (std::size_t z = 0; z < side; ++z)
                for (std::size_t x = 0; x < side; ++x)
                    if (data.colors[x + z * side] != sent->second.colors[x + z * side]) {
                        x0 = std::min(x0, x); x1 = std::max(x1, x);
                        z0 = std::min(z0, z); z1 = std::max(z1, z);
                    }
            if (x0 <= x1) {
                patch = {static_cast<std::uint8_t>(x0), static_cast<std::uint8_t>(z0), static_cast<std::uint8_t>(x1 - x0 + 1),
                         static_cast<std::uint8_t>(z1 - z0 + 1), {}};
                for (auto z = z0; z <= z1; ++z)
                    for (auto x = x0; x <= x1; ++x) patch.colors.push_back(data.colors[x + z * side]);
                sent->second.colors = data.colors;
            }
            const bool moved = sent->second.markers != markers;
            if (!moved && patch.columns == 0) continue;
            p.peer->send(wire.map_item_data(id, data.scale, data.locked,
                                            moved ? std::optional(std::span<const gameplay::MapMarker>(markers)) : std::nullopt, patch));
            if (moved) sent->second.markers = std::move(markers);
        }
    }
    // A player's experience, when it changed (and once on joining).
    void sync_experience(Player& p) {
        const auto view = engine->view();
        const auto inv = view.player(key_of(p));
        const std::tuple<float, std::int32_t, std::int32_t> now =
            inv && *inv ? std::tuple{(*inv)->xp_progress, (*inv)->xp_level, (*inv)->xp_total} : std::tuple{0.0f, 0, 0};
        if (p.experience == now) return;
        p.experience = now;
        p.peer->send(wire.set_experience(std::get<0>(now), std::get<1>(now), std::get<2>(now)));
    }

    // Light that changed in chunks players have, as light updates; light
    // follows the blocks of the previous tick, as on the official servers.
    void publish_light() {
        if (players.empty()) return;
        std::map<ChunkPos, std::vector<std::int32_t>> changed;
        for (const auto& [c, s] : engine->light_changes(overworld)) changed[c].push_back(s);
        const auto shape = WorldShape{bundle.world.min_y, bundle.world.height};
        for (auto& [c, sections] : changed) {
            std::optional<Bytes> packet;
            for (auto& [id, p] : players) {
                if (!p.sent.contains(c)) continue;
                if (!packet) {
                    const auto light = engine->column_light({overworld, c});
                    if (!light) break;
                    std::ranges::sort(sections);
                    protocol::Writer w;
                    write_light(w, &*light, shape, sections);
                    packet = wire.light_update(c.x, c.z, w.take());
                }
                p.peer->send(*packet);
            }
        }
    }

    void publish_chunks() {
        const auto shape = WorldShape{bundle.world.min_y, bundle.world.height};
        std::map<ChunkPos, ChunkLevel> loaded;
        for (const auto& [c, level] : engine->loaded_columns())
            if (c.dimension == overworld) loaded.emplace(c.pos, level);
        const auto look_up = lookup();
        std::vector<ChunkJob> jobs;
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
            for (const auto c : wanted)
                if (auto column = engine->column({overworld, c})) jobs.push_back({id, std::move(*column), {}, engine->column_light({overworld, c})});
        }
        encode_chunks(jobs, look_up, shape);
        // Packets go out per player in the order chosen above.
        std::size_t next = 0;
        for (auto& [id, p] : players) {
            if (next == jobs.size() || jobs[next].player != id) continue;
            p.peer->send(wire.chunk_batch_start());
            std::int32_t count = 0;
            for (; next < jobs.size() && jobs[next].player == id; ++next) {
                auto& job = jobs[next];
                p.peer->send(wire.chunk(std::move(job.fields)));
                p.sent.insert(job.column.pos);
                for (const auto& e : job.column.entities) show_entity(p, e.id.value, e);
                ++count;
            }
            p.peer->send(wire.chunk_batch_finished(count));
        }
    }

    struct ChunkJob {
        std::uint64_t player;
        runtime::ColumnSnapshot column;
        Bytes fields;
        std::optional<ColumnLight> light;
    };
    // Chunk packets are encoded in parallel: the engine does not run meanwhile,
    // so the encoders only read its published state.
    void encode_chunks(std::vector<ChunkJob>& jobs, const NeighborLookup& look_up, WorldShape shape) {
        const auto encode = [&](ChunkJob& job) {
            job.fields = encode_chunk(job.column, palette, look_up, shape, biome, wire.protocol(), job.light ? &*job.light : nullptr);
        };
        if (jobs.size() < 4 || !encode_pool) {
            for (auto& job : jobs) encode(job);
            return;
        }
        std::atomic<std::size_t> index{0};
        const auto take = [&] {
            for (std::size_t i; (i = index.fetch_add(1, std::memory_order_relaxed)) < jobs.size();) encode(jobs[i]);
        };
        const auto helpers = std::min<std::size_t>(config.workers - 1, jobs.size() - 1);
        std::latch done(static_cast<std::ptrdiff_t>(helpers));
        std::exception_ptr failure;
        std::mutex failure_mutex;
        for (std::size_t i = 0; i < helpers; ++i)
            asio::post(*encode_pool, [&] {
                try {
                    take();
                } catch (...) {
                    std::lock_guard lock(failure_mutex);
                    if (!failure) failure = std::current_exception();
                }
                done.count_down();
            });
        take();
        done.wait();
        if (failure) std::rethrow_exception(failure);
    }

    void publish_players() {
        for (auto& [id, p] : players) {
            if (!p.moved) continue;
            p.moved = false;
            const auto pos = wire.entity_position(p.entity_id, p.pos, p.yaw, p.pitch, p.on_ground);
            const auto head = wire.head_rotation(p.entity_id, p.yaw);
            // To those whose clients have the player (a dead player whose
            // body went has none, and sees no one).
            for (auto& [oid, o] : players) {
                if (oid == id || !p.seen_by.contains(oid)) continue;
                o.peer->send(pos);
                o.peer->send(head);
            }
        }
        // Players appear to others while their chunk is visible to them.
        for (auto& [id, p] : players)
            for (auto& [oid, o] : players) {
                if (oid == id) continue;
                // A dead player stays until they respawn or the body goes
                // (client/effects_heal, client/player_data), and one whose
                // body went sees no one.
                // A spectator only to spectators (client/spectators).
                const bool visible = o.sent.contains(chunk_at(p.pos)) && !(p.dead && p.dead_ticks >= 20) && !(o.dead && o.dead_ticks >= 20)
                                  && (p.game_mode != spectator || o.game_mode == spectator);
                if (visible && p.seen_by.insert(oid).second) {
                    // The entity, its data, then its attributes (client/attribute_join),
                    // and a new entity's first data again (client/player_data).
                    o.peer->send(wire.spawn_entity(p.entity_id, p.uuid, player_entity_type, p.pos, p.pitch, p.yaw, p.yaw, 0));
                    o.peer->send(shown_data(p));
                    if (std::ranges::any_of(p.attributes, [](const auto& i) { return gameplay::attributes::synced(i.attribute); }))
                        o.peer->send(wire.update_attributes(p.entity_id, p.attributes));
                    // Then what it holds and wears, each slot that holds something (client/armor).
                    std::vector<std::pair<std::uint8_t, WireStack>> held;
                    for (std::size_t k = 0; k < p.equipped.size(); ++k)
                        if (!p.equipped[k].empty()) held.emplace_back(static_cast<std::uint8_t>(k), wire_stack(p.equipped[k]));
                    if (!held.empty()) o.peer->send(wire.set_equipment(p.entity_id, held));
                    if (p.fresh_data && p.fresh_data_tick == tick_count) o.peer->send(*p.fresh_data);
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
            // Every start counts as the levels' creation (idle rule), and the
            // spawn chunks are loaded again.
            admit(contracts::LevelsStarted{});
            add_start_ticket();
            // The world takes the configured difficulty at every start.
            if (engine->difficulty() != config.difficulty) admit(contracts::SetDifficulty{config.difficulty});
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
        tell_impulses();
        send_decided_effects();
        send_decided_attributes();
        if (extensions) extensions->before_tick();
        // An empty server pauses after `pause-when-empty-seconds`, as the
        // target versions do, and resumes when a player joins.
        empty_ticks = players.empty() ? empty_ticks + 1 : 0;
        const bool pause = config.pause_when_empty_seconds > 0
                        && empty_ticks > static_cast<std::uint64_t>(config.pause_when_empty_seconds) * 20;
        if (pause != paused) {
            paused = pause;
            log_info(paused ? std::format("Server empty for {} seconds, pausing", config.pause_when_empty_seconds) : "Resuming");
        }
        // While frozen or paused, players move and chunks are sent, but the world waits.
        const bool run = (!frozen || pending_steps > 0) && !paused;
        update_vitals(run);
        if (run) {
            // Player-driven inputs that follow from the world as last published.
            continue_digging();
            detect_pickups();
            send_presences();
            // Every 20 ticks the time goes to every player, as on both
            // official servers: before a tick whose number is a multiple of
            // 20, after the commands that come with it.
            if ((engine->time() + 1) % 20 == 0) broadcast(time_packet());
            advance();
        }
        // A frozen world has no triggers: those who joined get them now (a
        // paused one resumes when a player joins).
        for (auto& [id, p] : players) send_joining_vitals(p);
        if (!run) { // the hotbar slot held changes without the world
            equipment_sounds();
            look_at_equipment();
        }
        publish_light();
        publish_chunks();
        publish_players();
        for (auto& [id, p] : players) finish_arrival(p);
        decide_attributes();
        decide_effects();
        for (auto& [id, p] : players) {
            for (const auto seq : p.acks) p.peer->send(wire.block_changed_ack(seq));
            p.acks.clear();
        }
        ++tick_count;
        healthy = true;
        if (!run) return;
        if (extensions) extensions->after_tick();
        if (world) {
            try {
                collect_save(false);
                if (engine->time() % config.autosave_ticks == 0) start_background_save();
            } catch (...) {
                healthy = false;
                throw;
            }
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
        pending_day_time.reset();
        pending_setblocks.clear();
        publish_weather();
        rain_on_players();
        equipment_sounds(); // before the level events (client/armor2)
        publish_blocks();
        publish_block_events();
        publish_pickups(commands); // before the picked-up entities are removed
        publish_entities();
        publish_level_events();
        finish_uses();
        look_at_equipment();
        publish_inventories();
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
           const protocol::ContentTables& content, const evidence::ContentMapping& mapping, const ServerData& data)
    : impl_(std::make_unique<Impl>(config, bundle, wire, content, mapping, data)) {}
Game::~Game() {
    if (impl_) impl_->stop_writer();
}

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
    i.stop_writer();
    // Nothing writes the world any more: release its directory for other owners.
    i.world.reset();
    if (i.extensions) i.extensions->unload();
    for (auto& [id, p] : impl_->players) p.peer->kick(std::string(reason));
    impl_->players.clear();
    impl_->player_count = 0;
}
std::size_t Game::players() const noexcept { return impl_->player_count.load(); }
std::uint64_t Game::ticks() const noexcept { return impl_->tick_count.load(); }
const runtime::Engine& Game::engine() const noexcept { return *impl_->engine; }

} // namespace circuit::server
