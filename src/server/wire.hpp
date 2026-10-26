// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "registries.hpp"
#include <circuit/protocol/codec.hpp>
#include <circuit/protocol/components.hpp>
#include <circuit/protocol/nbt.hpp>
#include <circuit/protocol/packets.hpp>
#include <expected>
#include <memory>
#include <optional>
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
} // namespace intent
using Intent = std::variant<intent::Move, intent::ConfirmTeleport, intent::Dig, intent::UseOn, intent::Use,
                            intent::HeldSlot, intent::CreativeSlot, intent::Command, intent::Chat, intent::Sneak,
                            intent::Flying, intent::Loaded, intent::ContainerClick, intent::ContainerClose,
                            intent::ClientCommand, intent::Sprint, intent::ContainerButton>;

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
    // An entity was hurt (red flash and sound), with the damage type's registry id.
    Bytes damage_event(std::int32_t entity_id, std::int32_t damage_type) const;
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
    Bytes block_changed_ack(std::int32_t sequence) const;
    Bytes player_position(std::int32_t teleport_id, Vec3 pos, float yaw, float pitch) const;
    Bytes keep_alive(std::int64_t id) const;
    Bytes system_chat(const protocol::nbt::Tag& text, bool overlay = false) const;
    Bytes player_info_add(const std::vector<PlayerListEntry>& players) const;
    Bytes player_info_remove(const std::vector<Uuid>& players) const;
    Bytes spawn_entity(std::int32_t id, Uuid uuid, std::int32_t type, Vec3 pos, float pitch, float yaw, float head_yaw,
                       std::int32_t data, Vec3 velocity = {}) const;
    Bytes take_item_entity(std::int32_t collected, std::int32_t collector, std::int32_t count) const;
    Bytes item_metadata(std::int32_t id, const WireStack& stack) const;
    Bytes tnt_metadata(std::int32_t id, std::int32_t fuse) const;
    Bytes explosion(Vec3 center) const;
    Bytes remove_entities(const std::vector<std::int32_t>& ids) const;
    Bytes entity_position(std::int32_t id, Vec3 pos, float yaw, float pitch, bool on_ground) const;
    Bytes head_rotation(std::int32_t id, float head_yaw) const;
    Bytes container_content(std::int32_t window, std::int32_t state_id, const std::vector<WireStack>& slots, const WireStack& carried) const;
    Bytes container_slot(std::int32_t window, std::int32_t state_id, std::int16_t slot, const WireStack& stack) const;
    // One property of a window (a furnace's burn and cooking times).
    Bytes container_data(std::int32_t window, std::int16_t property, std::int16_t value) const;
    Bytes cursor_item(const WireStack& stack) const;
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
