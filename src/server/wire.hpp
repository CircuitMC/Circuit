// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "recipe_book.hpp"
#include "registries.hpp"
#include <circuit/contracts/trace.hpp>
#include <circuit/gameplay/attributes.hpp>
#include <circuit/gameplay/effects.hpp>
#include <circuit/gameplay/maps.hpp>
#include <circuit/protocol/codec.hpp>
#include <circuit/protocol/components.hpp>
#include <circuit/protocol/nbt.hpp>
#include <circuit/protocol/packets.hpp>
#include <array>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <string>
#include <variant>
#include <vector>

// Version adapter for the Java Edition wire protocol: resolved packet ids,
// builders for clientbound packets and parsers for serverbound play packets.
// Layouts follow the cited revision of minecraft.wiki "Java Edition
// protocol/Packets" for the adapter's protocol number.
namespace circuit::server {

using protocol::Bytes;
using protocol::Uuid;
using protocol::WirePosition;

using Vec3 = contracts::Vec3;

struct ProfileProperty {
    std::string name, value;
    std::optional<std::string> signature;
};

struct PlayerListEntry {
    Uuid uuid;
    std::string name;
    std::vector<ProfileProperty> properties;
    std::int32_t game_mode{};
    bool listed{true};
    std::int32_t latency{};
};

// Serverbound play packets the server acts on.
// An item stack as the protocol sends it: wire item id, count, damage and
// the other data components (canonical, see protocol/components.hpp).
struct WireStack {
    std::int32_t item{};
    std::int32_t count{};
    std::int32_t damage{};
    contracts::ComponentPatch components{};
    bool operator==(const WireStack&) const = default;
};

namespace intent {
struct Move {
    std::optional<Vec3> position;
    std::optional<std::pair<float, float>> rotation; // yaw, pitch
    bool on_ground{};
};
struct ConfirmTeleport {
    std::int32_t id{};
};
struct Dig {
    std::int32_t status{}; // 0 started, 1 cancelled, 2 finished, 3/4 drop, 5 release, 6 swap
    WirePosition pos;
    std::int8_t face{};
    std::int32_t sequence{};
};
struct UseOn {
    std::int32_t hand{};
    WirePosition pos;
    std::int32_t face{}; // 0 down, 1 up, 2 north, 3 south, 4 west, 5 east
    float cursor_x{}, cursor_y{}, cursor_z{};
    bool inside{};
    std::int32_t sequence{};
};
struct Use {
    std::int32_t hand{};
    std::int32_t sequence{};
    float yaw{}, pitch{}; // the view the item is used along
};
struct HeldSlot {
    std::int16_t slot{};
};
struct CreativeSlot {
    std::int16_t slot{};
    std::int32_t count{};
    std::int32_t item{};
    std::int32_t damage{};
    contracts::ComponentPatch components{};
    std::string refused{}; // why the stack's components were refused; empty when read
};
struct ContainerClick {
    std::int32_t window{};
    std::int32_t state{};
    std::int16_t slot{};
    std::int8_t button{};
    std::int32_t mode{};
    // The client's predicted slots and carried stack. A stack is nullopt when
    // the prediction names data components the server cannot compare (hashed
    // components, or refused ones; it is then sent again); `unknown` when the
    // prediction could not be read at all.
    std::vector<std::pair<std::int16_t, std::optional<WireStack>>> changed;
    std::optional<WireStack> carried;
    bool unknown{};
};
struct ContainerClose {
    std::int32_t window{};
};
// A button of the open window: a stonecutter's recipe.
struct ContainerButton {
    std::int32_t window{};
    std::int32_t button{};
};
// The powers chosen in the open beacon window, by effect id (set_beacon).
struct SetBeacon {
    std::optional<std::int32_t> primary, secondary;
};
// The client's settings changed (client_information): the skin parts it
// shows (cape, jacket, sleeves, trouser legs, hat: bits 0 to 6) and its main
// hand (0 left, 1 right).
struct ClientInformation {
    std::uint8_t skin_parts{};
    std::uint8_t main_hand{1};
};
// An attack on an entity (interact, its attack action), by its entity id.
struct Attack {
    std::int32_t entity{};
};
// A spectator's teleport to an entity, from the spectator menu (teleport_to_entity).
struct TeleportToEntity {
    Uuid target;
};
// The player swings an arm (0 the main hand, 1 the off hand).
struct Swing {
    std::int32_t hand{};
};
// The name typed into the open anvil window.
struct RenameItem {
    std::string name;
};
// A slot of the open window enabled or disabled (a crafter's grid).
struct SlotState {
    std::int32_t window{};
    std::int32_t slot{};
    bool enabled{};
};
struct Command {
    std::string text;
};
struct Chat {
    std::string text;
};
struct Sneak {
    bool sneaking{};
};
struct Flying {
    bool flying{};
};
struct Loaded {};
// Client status: 0 asks to respawn after death.
struct ClientCommand {
    std::int32_t action{};
};
struct Sprint {
    bool sprinting{};
};
// A recipe the player saw in the recipe book (by display id), and a book's
// settings (0 crafting, 1 furnace, 2 blast furnace, 3 smoker).
struct RecipeSeen {
    std::int32_t display{};
};
struct RecipeBookSettings {
    std::int32_t book{};
    bool open{};
    bool filtering{};
};
// A book and quill edited (hand: a hotbar slot 0-8 or the off hand 40): its
// pages, and a title when it is signed.
struct EditBook {
    std::int32_t hand{};
    std::vector<std::string> pages;
    std::optional<std::string> title;
};
// A recipe of the book placed into a window (by display id), once more or
// as many times as fit.
struct PlaceRecipe {
    std::int32_t window{};
    std::int32_t display{};
    bool all{};
};
} // namespace intent
using Intent = std::variant<intent::Move, intent::ConfirmTeleport, intent::Dig, intent::UseOn, intent::Use,
                            intent::HeldSlot, intent::CreativeSlot, intent::Command, intent::Chat, intent::Sneak,
                            intent::Flying, intent::Loaded, intent::ContainerClick, intent::ContainerClose,
                            intent::ClientCommand, intent::Sprint, intent::ContainerButton, intent::SlotState, intent::RenameItem,
                            intent::RecipeSeen, intent::RecipeBookSettings, intent::PlaceRecipe, intent::EditBook,
                            intent::SetBeacon, intent::ClientInformation, intent::Attack, intent::TeleportToEntity, intent::Swing>;

class Wire {
public:
    explicit Wire(std::int32_t protocol);
    // With what item components may refer to (the registries the server sends).
    Wire(std::int32_t protocol, protocol::ComponentContext components);

    [[nodiscard]] std::int32_t protocol() const noexcept { return protocol_; }
    [[nodiscard]] const protocol::PacketIds& ids() const noexcept { return ids_; }
    // Id of a serverbound packet name, for dispatch.
    [[nodiscard]] std::int32_t serverbound(protocol::State state, std::string_view name) const {
        return ids_.id(state, protocol::Bound::serverbound, name);
    }

    // Status and login.
    Bytes status_response(std::string_view json) const;
    Bytes pong_response(std::int64_t payload) const;
    Bytes login_disconnect(std::string_view reason) const; // reason as plain text
    Bytes encryption_request(std::span<const std::byte> public_key, std::span<const std::byte> verify_token,
                             bool should_authenticate) const;
    Bytes set_compression(std::int32_t threshold) const;
    Bytes login_success(Uuid uuid, std::string_view name, const std::vector<ProfileProperty>& properties) const;

    // Configuration.
    Bytes config_plugin_message(std::string_view channel, std::span<const std::byte> data) const;
    Bytes config_disconnect(std::string_view reason) const;
    Bytes config_keep_alive(std::int64_t id) const;
    Bytes feature_flags(const std::vector<std::string_view>& flags) const;
    Bytes known_packs(std::string_view version) const;
    Bytes registry_data(std::string_view registry, const std::vector<std::string_view>& entries) const;
    Bytes registry_data(std::string_view registry,
                        const std::vector<std::pair<std::string_view, protocol::nbt::Tag>>& entries) const;
    Bytes finish_configuration() const;
    // Update Tags (configuration): tag entries are numeric ids in `registries`.
    Bytes update_tags(const std::vector<NumberedTags>& tags) const;

    // Play.
    struct JoinGame {
        std::int32_t entity_id{};
        std::int32_t max_players{};
        std::int32_t view_distance{};
        std::int32_t simulation_distance{};
        std::int32_t dimension_type{};
        std::uint8_t game_mode{};
    };
    Bytes join_game(const JoinGame& j) const;
    // Respawning in the overworld after death, keeping no client data.
    Bytes respawn(const JoinGame& j) const;
    // The death screen, with the death message.
    Bytes combat_kill(std::int32_t entity_id, const protocol::nbt::Tag& message) const;
    // An entity was hurt (red flash and sound), with the damage type's
    // registry id and the entities that caused and dealt it (none: -1).
    Bytes damage_event(std::int32_t entity_id, std::int32_t damage_type, std::int32_t cause = -1, std::int32_t direct = -1) const;
    // A hurt's animation for the one hurt (hurt_animation): the angle to
    // what hurt them, less their yaw (client/combat).
    Bytes hurt_animation(std::int32_t entity_id, float angle) const;
    // Particles at a place (level_particles): `count` of them spread by
    // `offset` at `speed`, or with a count of 0 one moving along `offset`.
    enum class Particle : std::uint8_t { damage_indicator, sweep_attack };
    Bytes particles(Particle particle, Vec3 at, Vec3 offset, float speed, std::int32_t count) const;
    Bytes game_event(std::uint8_t event, float value) const;
    Bytes player_abilities(std::uint8_t flags, float flying_speed, float fov_modifier) const;
    Bytes set_held_slot(std::int32_t slot) const;
    Bytes set_health(float health, std::int32_t food, float saturation) const;
    Bytes spawn_position(WirePosition pos, float angle) const;
    Bytes set_time(std::int64_t world_age, std::int64_t time_of_day, bool advancing) const;
    Bytes chunk_center(std::int32_t x, std::int32_t z) const;
    Bytes chunk_batch_start() const;
    Bytes chunk_batch_finished(std::int32_t count) const;
    Bytes chunk(Bytes fields) const; // fields from encode_chunk
    Bytes light_update(std::int32_t x, std::int32_t z, Bytes light) const; // light from write_light
    Bytes forget_chunk(std::int32_t x, std::int32_t z) const;
    Bytes block_update(WirePosition pos, std::int32_t state) const;
    // A block event (a chest's lid: event 1 with its viewers), with the
    // block's registry id.
    Bytes block_event(WirePosition pos, std::uint8_t event, std::uint8_t param, std::int32_t block) const;
    // A map's data for a player carrying it: its scale and lock, its markers
    // (when they changed) and a patch of its colors (`columns` by `rows`
    // pixels from `x`, `z`, row by row; none when `columns` is 0).
    struct MapPatch {
        std::uint8_t x{}, z{}, columns{}, rows{};
        std::vector<std::uint8_t> colors;
    };
    Bytes map_item_data(std::int32_t map, std::uint8_t scale, bool locked, std::optional<std::span<const gameplay::MapMarker>> markers,
                        const MapPatch& patch) const;
    // A level event (sounds and particles clients make themselves; 1500 a
    // composter taking an item), heard within its volume.
    Bytes level_event(std::int32_t event, WirePosition pos, std::int32_t data) const;
    Bytes block_changed_ack(std::int32_t sequence) const;
    Bytes player_position(std::int32_t teleport_id, Vec3 pos, float yaw, float pitch) const;
    Bytes keep_alive(std::int64_t id) const;
    Bytes system_chat(const protocol::nbt::Tag& text, bool overlay = false) const;
    Bytes player_info_add(const std::vector<PlayerListEntry>& players) const;
    // A player's new game mode for the player list (player_info_update, that action alone).
    Bytes player_info_game_mode(Uuid player, std::int32_t game_mode) const;
    // What the client's view follows (set_camera): an entity, or its own player.
    Bytes set_camera(std::int32_t entity) const;
    // An item cooldown group's cooldown (an item's id on both protocols), in ticks.
    Bytes cooldown(std::string_view group, std::int32_t ticks) const;
    // What an entity wears and holds, by equipment slot (0 the main hand, 1
    // the off hand, 2 the feet to 5 the head), the slots given in order.
    Bytes set_equipment(std::int32_t entity, std::span<const std::pair<std::uint8_t, WireStack>> slots) const;
    // An entity's animation (animate): 0 swinging the main arm, 3 the off
    // arm, 4 a critical hit on it.
    Bytes animate(std::int32_t entity, std::uint8_t animation) const;
    Bytes player_info_remove(const std::vector<Uuid>& players) const;
    Bytes spawn_entity(std::int32_t id, Uuid uuid, std::int32_t type, Vec3 pos, float pitch, float yaw, float head_yaw,
                       std::int32_t data, Vec3 velocity = {}) const;
    Bytes take_item_entity(std::int32_t collected, std::int32_t collector, std::int32_t count) const;
    Bytes item_metadata(std::int32_t id, const WireStack& stack) const;
    Bytes tnt_metadata(std::int32_t id, std::int32_t fuse) const;
    // An experience orb appearing: on 1.21.4 its own packet, on 1.21.5 and
    // later an entity of `type` whose value follows in orb_metadata.
    Bytes experience_orb(std::int32_t id, Uuid uuid, std::int32_t type, Vec3 pos, std::int32_t value) const;
    Bytes orb_metadata(std::int32_t id, std::int32_t value) const;
    [[nodiscard]] bool orbs_are_entities() const noexcept;
    // A player's experience: the progress toward the next level, the level and the total.
    Bytes set_experience(float progress, std::int32_t level, std::int32_t total) const;
    Bytes explosion(Vec3 center) const;
    // A sound the rules played, at a block's center (category block, volume
    // and pitch 1, as both official servers send a beacon's; the seed varies
    // on them).
    Bytes sound(contracts::SoundEvent sound, contracts::BlockPos at, std::int64_t seed) const;
    // A player's own sounds, at the player's position: hurt, death, and the
    // falls beyond and within 4 blocks of damage (category player); eating,
    // drinking and drinking honey, a burp, an ominous bottle's disposal, a
    // chorus fruit's teleport and the omens' coming, in the category given
    // (client/eat); a piece of armor put on (client/armor).
    enum class PlayerSound : std::uint8_t {
        hurt, death, big_fall, small_fall, eat, drink, honey_drink, burp, ominous_bottle, chorus_teleport, bad_omen, trial_omen, raid_omen,
        equip_generic, equip_leather, equip_chain, equip_iron, equip_gold, equip_diamond, equip_netherite, equip_turtle, equip_elytra,
        hurt_on_fire, generic_burn, extinguish_fire, hurt_drown, hurt_freeze, hurt_sweet_berry_bush,
        attack_crit, attack_knockback, attack_nodamage, attack_strong, attack_sweep, attack_weak, shield_block, shield_break
    };
    enum class SoundCategory : std::int32_t { neutral = 6, player = 7 };
    Bytes player_sound(PlayerSound sound, Vec3 at, float volume, float pitch, std::int64_t seed,
                       SoundCategory category = SoundCategory::player) const;
    // An entity event (entity_event): 3 a living entity's death, 9 a player's
    // use of an item finished (to the player alone).
    Bytes entity_event(std::int32_t entity_id, std::int8_t event) const;
    // An entity's velocity (set_entity_motion).
    Bytes entity_motion(std::int32_t entity_id, Vec3 velocity) const;
    // A block entity's data for clients (block_entity_data): its type by the
    // protocol's id, and its tag.
    Bytes block_entity_data(WirePosition at, std::int32_t type, const protocol::nbt::Tag& tag) const;
    Bytes remove_entities(const std::vector<std::int32_t>& ids) const;
    Bytes entity_position(std::int32_t id, Vec3 pos, float yaw, float pitch, bool on_ground) const;
    Bytes head_rotation(std::int32_t id, float head_yaw) const;
    Bytes container_content(std::int32_t window, std::int32_t state_id, const std::vector<WireStack>& slots, const WireStack& carried) const;
    Bytes container_slot(std::int32_t window, std::int32_t state_id, std::int16_t slot, const WireStack& stack) const;
    // One property of a window (a furnace's burn and cooking times).
    Bytes container_data(std::int32_t window, std::int16_t property, std::int16_t value) const;
    Bytes cursor_item(const WireStack& stack) const;
    // A slot of the player's inventory by its inventory index (hotbar 0-8,
    // main 9-35, armor 36-39 from the feet, off hand 40), outside any window.
    Bytes player_inventory(std::int32_t index, const WireStack& stack) const;
    // The recipe data clients need: item property sets (furnace inputs,
    // smithing slots) and the stonecutter's recipes in their button order.
    struct PropertySet {
        std::string id;
        std::vector<std::int32_t> items;
    };
    struct StonecutterEntry {
        std::vector<std::int32_t> input;
        WireStack result;
    };
    Bytes update_recipes(const std::vector<PropertySet>& property_sets, const std::vector<StonecutterEntry>& stonecutter) const;
    // The recipe book (client/recipes, both protocols): the four books'
    // settings; recipes added, or all a player knows replacing the book, by
    // display id with their flags (1 a notification, 2 highlighted); and
    // recipes removed. `item` gives an item's wire id.
    struct BookEntry {
        std::int32_t display{};
        const BookRecipe* recipe{};
        std::uint8_t flags{};
    };
    Bytes recipe_book_settings(const std::array<bool, 8>& settings) const;
    Bytes recipe_book_add(std::span<const BookEntry> entries, bool replace, const std::function<std::int32_t(std::uint16_t)>& item) const;
    Bytes recipe_book_remove(std::span<const std::int32_t> ids) const;
    // A written book opened for reading (hand 0 the main hand, 1 the off hand).
    Bytes open_book(std::int32_t hand) const;
    // A status effect of the player's own entity added or changed (flags: 1
    // ambient, 2 particles, 4 icon, 8 a new effect blending in), or removed.
    Bytes update_mob_effect(std::int32_t entity, std::int32_t effect, std::int32_t amplifier, std::int32_t duration,
                            std::uint8_t flags) const;
    Bytes remove_mob_effect(std::int32_t entity, std::int32_t effect) const;
    // A player's entity data, each entry when given, in index order: its
    // shared flags (index 0), its air supply (1), its pose (6), its frozen
    // ticks (7), its living flags (8), its health (9), its effect particles
    // (10), whether they are ambient (11), its absorption (15), the skin
    // parts it shows (17) and its main hand (18).
    struct PlayerData {
        std::optional<std::uint8_t> flags;
        std::optional<std::int32_t> pose; // index 6: 0 standing, 3 crawling, 5 crouching
        std::optional<float> health;
        std::optional<std::vector<gameplay::effects::Particle>> particles;
        std::optional<bool> ambient;
        std::optional<float> absorption;
        std::optional<std::uint8_t> skin_parts;
        std::optional<std::uint8_t> main_hand;
        std::optional<std::uint8_t> living; // index 8: 1 using an item, 2 in the off hand
        std::optional<std::int32_t> air;    // index 1
        std::optional<std::int32_t> frozen; // index 7
    };
    Bytes player_data(std::int32_t id, const PlayerData& d) const;
    // The entries status effects decide.
    Bytes effect_metadata(std::int32_t id, std::optional<std::uint8_t> flags,
                          const std::optional<std::vector<gameplay::effects::Particle>>& particles, std::optional<bool> ambient,
                          std::optional<float> absorption = std::nullopt) const {
        return player_data(id, {flags, std::nullopt, std::nullopt, particles, ambient, absorption, std::nullopt, std::nullopt, std::nullopt,
                                std::nullopt, std::nullopt});
    }
    // A player's attributes (client/attributes), in the order given: each
    // with a player's base and its modifiers; those the official servers
    // keep to themselves are left out.
    Bytes update_attributes(std::int32_t entity, std::span<const gameplay::attributes::Instance> attributes) const;
    // The ghost of a recipe that could not be placed, shown in a window's grid.
    Bytes place_ghost_recipe(std::int32_t window, const BookRecipe& recipe, const std::function<std::int32_t(std::uint16_t)>& item) const;
    // What item components may refer to on this protocol.
    [[nodiscard]] const protocol::ComponentContext& component_context() const noexcept { return components_ctx_; }
    [[nodiscard]] const protocol::ComponentCodec& components() const noexcept { return *components_; }
    Bytes open_screen(std::int32_t window, std::int32_t menu_type, const protocol::nbt::Tag& title) const;
    Bytes close_screen(std::int32_t window) const;
    Bytes play_disconnect(std::string_view reason) const;
    Bytes empty_commands() const;

    // Parses a serverbound play packet; nullopt for packets the server ignores.
    std::expected<std::optional<Intent>, protocol::CodecError> parse_play(std::int32_t id, protocol::Reader& r) const;

private:
    struct Ids;
    Bytes packet(std::int32_t id, const protocol::Writer& fields) const;

    void slot(protocol::Writer& w, const WireStack& s) const;

    std::int32_t protocol_;
    const protocol::PacketIds& ids_;
    std::shared_ptr<const Ids> id_;
    const protocol::ComponentCodec* components_;
    protocol::ComponentContext components_ctx_;
};

// Escapes text for a JSON string literal.
std::string json_escape(std::string_view text);

} // namespace circuit::server
