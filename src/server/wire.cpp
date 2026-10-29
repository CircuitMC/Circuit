// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/contracts/commands.hpp>
#include "wire.hpp"
#include <circuit/protocol/content.hpp>
#include <algorithm>
#include <format>
#include <stdexcept>

namespace circuit::server {
using protocol::Bound;
using protocol::CodecError;
using protocol::State;
using protocol::Writer;
namespace nbt = protocol::nbt;

struct Wire::Ids {
    std::int32_t status_response, pong_response;
    std::int32_t login_disconnect, hello, login_compression, login_finished;
    std::int32_t cfg_custom_payload, cfg_disconnect, cfg_finish, cfg_keep_alive, cfg_registry_data, cfg_features,
        cfg_known_packs, cfg_update_tags;
    std::int32_t login, set_health, game_event, player_abilities, set_held_slot, spawn_position, set_time, chunk_center,
        chunk_batch_start, chunk_batch_finished, level_chunk, light_update, forget_chunk, block_update, block_event, level_event, block_changed_ack,
        player_position, keep_alive, system_chat, player_info_update, player_info_remove, add_entity,
        set_entity_data, remove_entities, entity_position_sync, rotate_head, container_set_content, disconnect, take_item_entity,
        open_screen, container_close, container_set_slot, container_set_data, set_cursor_item, set_player_inventory, respawn, combat_kill, damage_event, sb_container_click,
        sb_container_close, sb_container_button_click, sb_container_slot_state_changed, sb_rename_item, update_recipes,
        commands, explode, set_experience, map_item_data, recipe_book_add, recipe_book_remove, recipe_book_settings, place_ghost_recipe, open_book;
    // The orb spawn packet of protocols before 1.21.5's (-1: none; orbs are
    // entities with their value as data).
    std::int32_t add_experience_orb{-1};
    // Serverbound play.
    std::int32_t sb_accept_teleportation, sb_move_pos, sb_move_pos_rot, sb_move_rot, sb_move_status,
        sb_player_action, sb_use_item_on, sb_use_item, sb_set_carried_item, sb_set_creative_slot, sb_chat_command,
        sb_chat_command_signed, sb_chat, sb_player_input, sb_player_abilities, sb_player_loaded, sb_client_command, sb_player_command,
        sb_recipe_book_seen_recipe, sb_recipe_book_change_settings, sb_place_recipe, sb_edit_book;
};

Wire::Wire(std::int32_t protocol) : Wire(protocol, server::component_context(protocol)) {}

Wire::Wire(std::int32_t protocol, protocol::ComponentContext components) : protocol_(protocol), ids_([&]() -> const protocol::PacketIds& {
    const auto* ids = protocol::PacketIds::for_protocol(protocol);
    if (!ids) throw std::invalid_argument(std::format("server: no packet table for protocol {}", protocol));
    return *ids;
}()), components_(protocol::ComponentCodec::for_protocol(protocol)), components_ctx_(std::move(components)) {
    if (!components_) throw std::invalid_argument(std::format("server: no component schema for protocol {}", protocol));
    const auto cb = [&](State s, std::string_view n) { return ids_.id(s, Bound::clientbound, n); };
    const auto sb = [&](State s, std::string_view n) { return ids_.id(s, Bound::serverbound, n); };
    auto ids = std::make_shared<Ids>();
    ids->status_response = cb(State::status, "status_response");
    ids->pong_response = cb(State::status, "pong_response");
    ids->login_disconnect = cb(State::login, "login_disconnect");
    ids->hello = cb(State::login, "hello");
    ids->login_compression = cb(State::login, "login_compression");
    ids->login_finished = cb(State::login, "login_finished");
    ids->cfg_custom_payload = cb(State::configuration, "custom_payload");
    ids->cfg_disconnect = cb(State::configuration, "disconnect");
    ids->cfg_finish = cb(State::configuration, "finish_configuration");
    ids->cfg_keep_alive = cb(State::configuration, "keep_alive");
    ids->cfg_registry_data = cb(State::configuration, "registry_data");
    ids->cfg_features = cb(State::configuration, "update_enabled_features");
    ids->cfg_known_packs = cb(State::configuration, "select_known_packs");
    ids->cfg_update_tags = cb(State::configuration, "update_tags");
    ids->login = cb(State::play, "login");
    ids->game_event = cb(State::play, "game_event");
    ids->set_health = cb(State::play, "set_health");
    ids->player_abilities = cb(State::play, "player_abilities");
    ids->set_held_slot = cb(State::play, "set_held_slot");
    ids->spawn_position = cb(State::play, "set_default_spawn_position");
    ids->set_time = cb(State::play, "set_time");
    ids->chunk_center = cb(State::play, "set_chunk_cache_center");
    ids->chunk_batch_start = cb(State::play, "chunk_batch_start");
    ids->chunk_batch_finished = cb(State::play, "chunk_batch_finished");
    ids->level_chunk = cb(State::play, "level_chunk_with_light");
    ids->light_update = cb(State::play, "light_update");
    ids->forget_chunk = cb(State::play, "forget_level_chunk");
    ids->block_update = cb(State::play, "block_update");
    ids->block_event = cb(State::play, "block_event");
    ids->map_item_data = cb(State::play, "map_item_data");
    ids->level_event = cb(State::play, "level_event");
    ids->block_changed_ack = cb(State::play, "block_changed_ack");
    ids->player_position = cb(State::play, "player_position");
    ids->keep_alive = cb(State::play, "keep_alive");
    ids->system_chat = cb(State::play, "system_chat");
    ids->player_info_update = cb(State::play, "player_info_update");
    ids->player_info_remove = cb(State::play, "player_info_remove");
    ids->add_entity = cb(State::play, "add_entity");
    ids->set_entity_data = cb(State::play, "set_entity_data");
    ids->remove_entities = cb(State::play, "remove_entities");
    ids->entity_position_sync = cb(State::play, "entity_position_sync");
    ids->rotate_head = cb(State::play, "rotate_head");
    ids->container_set_content = cb(State::play, "container_set_content");
    ids->take_item_entity = cb(State::play, "take_item_entity");
    ids->open_screen = cb(State::play, "open_screen");
    ids->container_close = cb(State::play, "container_close");
    ids->container_set_slot = cb(State::play, "container_set_slot");
    ids->container_set_data = cb(State::play, "container_set_data");
    ids->set_cursor_item = cb(State::play, "set_cursor_item");
    ids->set_player_inventory = cb(State::play, "set_player_inventory");
    ids->respawn = cb(State::play, "respawn");
    ids->combat_kill = cb(State::play, "player_combat_kill");
    ids->damage_event = cb(State::play, "damage_event");
    ids->sb_container_click = sb(State::play, "container_click");
    ids->sb_container_close = sb(State::play, "container_close");
    ids->sb_container_button_click = sb(State::play, "container_button_click");
    ids->sb_container_slot_state_changed = sb(State::play, "container_slot_state_changed");
    ids->sb_rename_item = sb(State::play, "rename_item");
    ids->sb_recipe_book_seen_recipe = sb(State::play, "recipe_book_seen_recipe");
    ids->sb_recipe_book_change_settings = sb(State::play, "recipe_book_change_settings");
    ids->sb_place_recipe = sb(State::play, "place_recipe");
    ids->sb_edit_book = sb(State::play, "edit_book");
    ids->update_recipes = cb(State::play, "update_recipes");
    ids->recipe_book_add = cb(State::play, "recipe_book_add");
    ids->recipe_book_remove = cb(State::play, "recipe_book_remove");
    ids->recipe_book_settings = cb(State::play, "recipe_book_settings");
    ids->place_ghost_recipe = cb(State::play, "place_ghost_recipe");
    ids->open_book = cb(State::play, "open_book");
    ids->disconnect = cb(State::play, "disconnect");
    ids->commands = cb(State::play, "commands");
    ids->explode = cb(State::play, "explode");
    ids->set_experience = cb(State::play, "set_experience");
    ids->add_experience_orb = ids_.find(State::play, Bound::clientbound, "add_experience_orb").value_or(-1);
    ids->sb_accept_teleportation = sb(State::play, "accept_teleportation");
    ids->sb_move_pos = sb(State::play, "move_player_pos");
    ids->sb_move_pos_rot = sb(State::play, "move_player_pos_rot");
    ids->sb_move_rot = sb(State::play, "move_player_rot");
    ids->sb_move_status = sb(State::play, "move_player_status_only");
    ids->sb_player_action = sb(State::play, "player_action");
    ids->sb_use_item_on = sb(State::play, "use_item_on");
    ids->sb_use_item = sb(State::play, "use_item");
    ids->sb_set_carried_item = sb(State::play, "set_carried_item");
    ids->sb_set_creative_slot = sb(State::play, "set_creative_mode_slot");
    ids->sb_chat_command = sb(State::play, "chat_command");
    ids->sb_chat_command_signed = sb(State::play, "chat_command_signed");
    ids->sb_chat = sb(State::play, "chat");
    ids->sb_player_input = sb(State::play, "player_input");
    ids->sb_player_abilities = sb(State::play, "player_abilities");
    ids->sb_player_loaded = sb(State::play, "player_loaded");
    ids->sb_client_command = sb(State::play, "client_command");
    ids->sb_player_command = sb(State::play, "player_command");
    id_ = std::move(ids);
}

Bytes Wire::packet(std::int32_t id, const Writer& fields) const {
    Writer w;
    w.varint(id);
    w.raw(fields.bytes());
    return w.take();
}

std::string json_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            else out += c;
        }
    }
    return out;
}

Bytes Wire::status_response(std::string_view json) const { return packet(id_->status_response, Writer().string(json)); }
Bytes Wire::pong_response(std::int64_t payload) const { return packet(id_->pong_response, Writer().i64(payload)); }
Bytes Wire::login_disconnect(std::string_view reason) const {
    // The login state carries reasons as JSON text.
    return packet(id_->login_disconnect, Writer().string(std::format("{{\"text\":\"{}\"}}", json_escape(reason))));
}
Bytes Wire::encryption_request(std::span<const std::byte> public_key, std::span<const std::byte> verify_token,
                               bool should_authenticate) const {
    return packet(id_->hello, Writer().string("", 20).prefixed_bytes(public_key).prefixed_bytes(verify_token).boolean(should_authenticate));
}
Bytes Wire::set_compression(std::int32_t threshold) const { return packet(id_->login_compression, Writer().varint(threshold)); }
Bytes Wire::login_success(Uuid uuid, std::string_view name, const std::vector<ProfileProperty>& properties) const {
    Writer w;
    w.uuid(uuid).string(name, 16).varint(static_cast<std::int32_t>(properties.size()));
    for (const auto& p : properties) {
        w.string(p.name, 64).string(p.value).boolean(p.signature.has_value());
        if (p.signature) w.string(*p.signature, 1024);
    }
    return packet(id_->login_finished, w);
}

Bytes Wire::config_plugin_message(std::string_view channel, std::span<const std::byte> data) const {
    return packet(id_->cfg_custom_payload, Writer().string(channel).raw(data));
}
Bytes Wire::config_disconnect(std::string_view reason) const {
    Writer w;
    nbt::write_network(w, nbt::text(std::string(reason)));
    return packet(id_->cfg_disconnect, w);
}
Bytes Wire::config_keep_alive(std::int64_t id) const { return packet(id_->cfg_keep_alive, Writer().i64(id)); }
Bytes Wire::feature_flags(const std::vector<std::string_view>& flags) const {
    Writer w;
    w.varint(static_cast<std::int32_t>(flags.size()));
    for (const auto f : flags) w.string(f);
    return packet(id_->cfg_features, w);
}
Bytes Wire::known_packs(std::string_view version) const {
    return packet(id_->cfg_known_packs, Writer().varint(1).string("minecraft").string("core").string(version));
}
Bytes Wire::registry_data(std::string_view registry, const std::vector<std::string_view>& entries) const {
    Writer w;
    w.string(registry).varint(static_cast<std::int32_t>(entries.size()));
    for (const auto e : entries) w.string(e).boolean(false); // data comes from the shared known pack
    return packet(id_->cfg_registry_data, w);
}
Bytes Wire::registry_data(std::string_view registry,
                          const std::vector<std::pair<std::string_view, nbt::Tag>>& entries) const {
    Writer w;
    w.string(registry).varint(static_cast<std::int32_t>(entries.size()));
    for (const auto& [name, data] : entries) {
        w.string(name).boolean(true);
        nbt::write_network(w, data);
    }
    return packet(id_->cfg_registry_data, w);
}
Bytes Wire::finish_configuration() const { return packet(id_->cfg_finish, Writer()); }

Bytes Wire::update_tags(const std::vector<NumberedTags>& tags) const {
    Writer w;
    w.varint(static_cast<std::int32_t>(tags.size()));
    for (const auto& r : tags) {
        w.string(r.registry).varint(static_cast<std::int32_t>(r.tags.size()));
        for (const auto& [name, ids] : r.tags) {
            w.string(name).varint(static_cast<std::int32_t>(ids.size()));
            for (const auto id : ids) w.varint(id);
        }
    }
    return packet(id_->cfg_update_tags, w);
}

namespace {
// The spawn information that login and respawn share.
void spawn_info(Writer& w, const Wire::JoinGame& j) {
    w.varint(j.dimension_type).string("minecraft:overworld");
    w.i64(0);                                    // hashed seed
    w.u8(j.game_mode).i8(-1);                    // game mode, previous game mode
    w.boolean(false).boolean(true);              // debug, flat
    w.boolean(false);                            // no death location
    w.varint(0);                                 // portal cooldown
    w.varint(-63);                               // sea level of the flat world
}
} // namespace

Bytes Wire::join_game(const JoinGame& j) const {
    Writer w;
    w.i32(j.entity_id).boolean(false);           // hardcore
    w.varint(1).string("minecraft:overworld");   // dimension names
    w.varint(j.max_players).varint(j.view_distance).varint(j.simulation_distance);
    w.boolean(false).boolean(true).boolean(false); // reduced debug info, respawn screen, limited crafting
    spawn_info(w, j);
    w.boolean(false);                            // enforces secure chat
    return packet(id_->login, w);
}
Bytes Wire::respawn(const JoinGame& j) const {
    Writer w;
    spawn_info(w, j);
    w.u8(0); // keep neither attributes nor entity data
    return packet(id_->respawn, w);
}
Bytes Wire::combat_kill(std::int32_t entity_id, const nbt::Tag& message) const {
    Writer w;
    w.varint(entity_id);
    nbt::write_network(w, message);
    return packet(id_->combat_kill, w);
}
Bytes Wire::damage_event(std::int32_t entity_id, std::int32_t damage_type) const {
    // No causing or direct entity (ids + 1, 0 for none) and no source position.
    return packet(id_->damage_event, Writer().varint(entity_id).varint(damage_type).varint(0).varint(0).boolean(false));
}
Bytes Wire::game_event(std::uint8_t event, float value) const { return packet(id_->game_event, Writer().u8(event).f32(value)); }
Bytes Wire::player_abilities(std::uint8_t flags, float flying_speed, float fov_modifier) const {
    return packet(id_->player_abilities, Writer().u8(flags).f32(flying_speed).f32(fov_modifier));
}
Bytes Wire::set_health(float health, std::int32_t food, float saturation) const {
    return packet(id_->set_health, Writer().f32(health).varint(food).f32(saturation));
}
Bytes Wire::set_held_slot(std::int32_t slot) const { return packet(id_->set_held_slot, Writer().varint(slot)); }
Bytes Wire::spawn_position(WirePosition pos, float angle) const {
    return packet(id_->spawn_position, Writer().position(pos).f32(angle));
}
Bytes Wire::set_time(std::int64_t world_age, std::int64_t time_of_day, bool advancing) const {
    return packet(id_->set_time, Writer().i64(world_age).i64(time_of_day).boolean(advancing));
}
Bytes Wire::chunk_center(std::int32_t x, std::int32_t z) const { return packet(id_->chunk_center, Writer().varint(x).varint(z)); }
Bytes Wire::chunk_batch_start() const { return packet(id_->chunk_batch_start, Writer()); }
Bytes Wire::chunk_batch_finished(std::int32_t count) const { return packet(id_->chunk_batch_finished, Writer().varint(count)); }
Bytes Wire::chunk(Bytes fields) const { return packet(id_->level_chunk, Writer().raw(fields)); }
Bytes Wire::light_update(std::int32_t x, std::int32_t z, Bytes light) const {
    return packet(id_->light_update, Writer().varint(x).varint(z).raw(light));
}
Bytes Wire::forget_chunk(std::int32_t x, std::int32_t z) const { return packet(id_->forget_chunk, Writer().i32(z).i32(x)); }
Bytes Wire::block_update(WirePosition pos, std::int32_t state) const {
    return packet(id_->block_update, Writer().position(pos).varint(state));
}
Bytes Wire::map_item_data(std::int32_t map, std::uint8_t scale, bool locked, std::optional<std::span<const gameplay::MapMarker>> markers,
                          const MapPatch& patch) const {
    Writer w;
    w.varint(map).u8(scale).boolean(locked).boolean(markers.has_value());
    if (markers) {
        w.varint(static_cast<std::int32_t>(markers->size()));
        for (const auto& m : *markers)
            w.varint(m.type).u8(static_cast<std::uint8_t>(m.x)).u8(static_cast<std::uint8_t>(m.z)).u8(m.direction).boolean(false);
    }
    w.u8(patch.columns);
    if (patch.columns > 0) {
        w.u8(patch.rows).u8(patch.x).u8(patch.z);
        w.prefixed_bytes(std::as_bytes(std::span(patch.colors)));
    }
    return packet(id_->map_item_data, w);
}

Bytes Wire::block_event(WirePosition pos, std::uint8_t event, std::uint8_t param, std::int32_t block) const {
    return packet(id_->block_event, Writer().position(pos).u8(event).u8(param).varint(block));
}
Bytes Wire::level_event(std::int32_t event, WirePosition pos, std::int32_t data) const {
    return packet(id_->level_event, Writer().i32(event).position(pos).i32(data).boolean(false));
}
Bytes Wire::block_changed_ack(std::int32_t sequence) const { return packet(id_->block_changed_ack, Writer().varint(sequence)); }
Bytes Wire::player_position(std::int32_t teleport_id, Vec3 pos, float yaw, float pitch) const {
    Writer w;
    w.varint(teleport_id).f64(pos.x).f64(pos.y).f64(pos.z).f64(0).f64(0).f64(0).f32(yaw).f32(pitch);
    w.i32(0); // teleport flags: every value absolute
    return packet(id_->player_position, w);
}
Bytes Wire::keep_alive(std::int64_t id) const { return packet(id_->keep_alive, Writer().i64(id)); }
Bytes Wire::system_chat(const nbt::Tag& text, bool overlay) const {
    Writer w;
    nbt::write_network(w, text);
    w.boolean(overlay);
    return packet(id_->system_chat, w);
}
Bytes Wire::player_info_add(const std::vector<PlayerListEntry>& players) const {
    Writer w;
    w.u8(0x01 | 0x04 | 0x08 | 0x10); // add player, game mode, listed, latency
    w.varint(static_cast<std::int32_t>(players.size()));
    for (const auto& p : players) {
        w.uuid(p.uuid).string(p.name, 16).varint(static_cast<std::int32_t>(p.properties.size()));
        for (const auto& prop : p.properties) {
            w.string(prop.name, 64).string(prop.value).boolean(prop.signature.has_value());
            if (prop.signature) w.string(*prop.signature, 1024);
        }
        w.varint(p.game_mode).boolean(p.listed).varint(p.latency);
    }
    return packet(id_->player_info_update, w);
}
Bytes Wire::player_info_remove(const std::vector<Uuid>& players) const {
    Writer w;
    w.varint(static_cast<std::int32_t>(players.size()));
    for (const auto& u : players) w.uuid(u);
    return packet(id_->player_info_remove, w);
}
Bytes Wire::spawn_entity(std::int32_t id, Uuid uuid, std::int32_t type, Vec3 pos, float pitch, float yaw, float head_yaw,
                         std::int32_t data, Vec3 velocity) const {
    // Velocity in 1/8000 blocks per tick, clamped as the target version does.
    const auto v = [](double d) { return static_cast<std::int16_t>(std::clamp(d, -3.9, 3.9) * 8000.0); };
    Writer w;
    w.varint(id).uuid(uuid).varint(type).f64(pos.x).f64(pos.y).f64(pos.z);
    w.angle(pitch).angle(yaw).angle(head_yaw).varint(data).i16(v(velocity.x)).i16(v(velocity.y)).i16(v(velocity.z));
    return packet(id_->add_entity, w);
}
Bytes Wire::take_item_entity(std::int32_t collected, std::int32_t collector, std::int32_t count) const {
    return packet(id_->take_item_entity, Writer().varint(collected).varint(collector).varint(count));
}
Bytes Wire::item_metadata(std::int32_t id, const WireStack& stack) const {
    Writer w;
    w.varint(id);
    w.u8(8).varint(7); // index 8 (item entity: item stack), type Slot
    slot(w, stack);
    w.u8(0xFF);
    return packet(id_->set_entity_data, w);
}
Bytes Wire::experience_orb(std::int32_t id, Uuid uuid, std::int32_t type, Vec3 pos, std::int32_t value) const {
    // 1.21.4: the orb's own packet with its value as a short; 1.21.5 and
    // later: an entity with its value at data index 8 (a VarInt).
    if (id_->add_experience_orb >= 0) {
        Writer w;
        w.varint(id).f64(pos.x).f64(pos.y).f64(pos.z).i16(static_cast<std::int16_t>(std::clamp(value, 0, 32767)));
        return packet(id_->add_experience_orb, w);
    }
    return spawn_entity(id, uuid, type, pos, 0, 0, 0, 0, {});
}
Bytes Wire::orb_metadata(std::int32_t id, std::int32_t value) const {
    Writer w;
    w.varint(id);
    w.u8(8).varint(1).varint(value); // index 8 (experience orb: value), type VarInt
    w.u8(0xFF);
    return packet(id_->set_entity_data, w);
}
bool Wire::orbs_are_entities() const noexcept { return id_->add_experience_orb < 0; }
Bytes Wire::set_experience(float progress, std::int32_t level, std::int32_t total) const {
    return packet(id_->set_experience, Writer().f32(progress).varint(level).varint(total));
}
Bytes Wire::tnt_metadata(std::int32_t id, std::int32_t fuse) const {
    Writer w;
    w.varint(id);
    w.u8(8).varint(1).varint(fuse); // index 8 (primed TNT: fuse), type VarInt
    w.u8(0xFF);
    return packet(id_->set_entity_data, w);
}
Bytes Wire::explosion(Vec3 center) const {
    // The explosion_emitter particle (21 on both protocols) and the sound
    // entity.generic.explode by its registry id plus one (PrismarineJS
    // minecraft-data, MIT: 602 on 769, 615 on 772); no player knockback.
    const std::int32_t sound = protocol_ == 769 ? 602 : 615;
    Writer w;
    w.f64(center.x).f64(center.y).f64(center.z).boolean(false).varint(21).varint(sound + 1);
    return packet(id_->explode, w);
}
Bytes Wire::remove_entities(const std::vector<std::int32_t>& ids) const {
    Writer w;
    w.varint(static_cast<std::int32_t>(ids.size()));
    for (const auto id : ids) w.varint(id);
    return packet(id_->remove_entities, w);
}
Bytes Wire::entity_position(std::int32_t id, Vec3 pos, float yaw, float pitch, bool on_ground) const {
    Writer w;
    w.varint(id).f64(pos.x).f64(pos.y).f64(pos.z).f64(0).f64(0).f64(0).f32(yaw).f32(pitch).boolean(on_ground);
    return packet(id_->entity_position_sync, w);
}
Bytes Wire::head_rotation(std::int32_t id, float head_yaw) const {
    return packet(id_->rotate_head, Writer().varint(id).angle(head_yaw));
}
// Slot data: count, then item id and the components added (the damage among
// them) and removed.
void Wire::slot(Writer& w, const WireStack& s) const {
    w.varint(s.count);
    if (s.count <= 0) return;
    w.varint(s.item);
    components_->write(w, s.damage > 0 ? std::optional(s.damage) : std::nullopt, s.components);
}

Bytes Wire::container_content(std::int32_t window, std::int32_t state_id, const std::vector<WireStack>& slots, const WireStack& carried) const {
    Writer w;
    w.varint(window).varint(state_id).varint(static_cast<std::int32_t>(slots.size()));
    for (const auto& s : slots) slot(w, s);
    slot(w, carried);
    return packet(id_->container_set_content, w);
}
Bytes Wire::container_slot(std::int32_t window, std::int32_t state_id, std::int16_t index, const WireStack& stack) const {
    Writer w;
    w.varint(window).varint(state_id).i16(index);
    slot(w, stack);
    return packet(id_->container_set_slot, w);
}
Bytes Wire::container_data(std::int32_t window, std::int16_t property, std::int16_t value) const {
    Writer w;
    w.varint(window).i16(property).i16(value);
    return packet(id_->container_set_data, w);
}
namespace {
void slot_display(Writer& w, const SlotDisplay& d, const std::function<std::int32_t(std::uint16_t)>& item,
                  const std::function<void(Writer&, const WireStack&)>& stack) {
    w.varint(static_cast<std::int32_t>(d.kind));
    switch (d.kind) {
    case SlotDisplay::Kind::empty:
    case SlotDisplay::Kind::any_fuel: return;
    case SlotDisplay::Kind::item: w.varint(item(d.item)); return;
    case SlotDisplay::Kind::item_stack: stack(w, {item(d.stack.item), d.stack.count, d.stack.damage, d.stack.components}); return;
    case SlotDisplay::Kind::tag: w.string(d.tag); return;
    case SlotDisplay::Kind::smithing_trim:
        slot_display(w, d.parts[0], item, stack);
        slot_display(w, d.parts[1], item, stack);
        // A trim pattern by its registry entry (holder: index + 1), or before
        // 1.21.5 its template's display.
        if (d.pattern >= 0) w.varint(d.pattern + 1);
        else slot_display(w, d.parts[2], item, stack);
        return;
    case SlotDisplay::Kind::with_remainder:
        slot_display(w, d.parts[0], item, stack);
        slot_display(w, d.parts[1], item, stack);
        return;
    case SlotDisplay::Kind::composite:
        w.varint(static_cast<std::int32_t>(d.parts.size()));
        for (const auto& p : d.parts) slot_display(w, p, item, stack);
        return;
    }
}
} // namespace

Bytes Wire::recipe_book_settings(const std::array<bool, 8>& settings) const {
    Writer w;
    for (const bool b : settings) w.boolean(b);
    return packet(id_->recipe_book_settings, w);
}

namespace {
// A recipe as clients show it (RecipeDisplay): its kind, then its slots.
void recipe_display(Writer& w, const RecipeDisplay& d, const std::function<std::int32_t(std::uint16_t)>& item,
                    const std::function<void(Writer&, const WireStack&)>& stack) {
    const auto display = [&](const SlotDisplay& s) { slot_display(w, s, item, stack); };
    w.varint(static_cast<std::int32_t>(d.kind));
    switch (d.kind) {
    case RecipeDisplay::Kind::shaped:
        w.varint(d.width).varint(d.height);
        [[fallthrough]];
    case RecipeDisplay::Kind::shapeless:
        w.varint(static_cast<std::int32_t>(d.slots.size()));
        for (const auto& s : d.slots) display(s);
        display(d.result);
        display(d.station);
        break;
    case RecipeDisplay::Kind::furnace:
        display(d.slots.at(0));
        display({.kind = SlotDisplay::Kind::any_fuel});
        display(d.result);
        display(d.station);
        w.varint(d.duration).f32(d.experience);
        break;
    case RecipeDisplay::Kind::stonecutter:
        display(d.slots.at(0));
        display(d.result);
        display(d.station);
        break;
    case RecipeDisplay::Kind::smithing:
        for (std::size_t k = 0; k < 3; ++k) display(d.slots.at(k));
        display(d.result);
        display(d.station);
        break;
    }
}
} // namespace

Bytes Wire::recipe_book_add(std::span<const BookEntry> entries, bool replace, const std::function<std::int32_t(std::uint16_t)>& item) const {
    Writer w;
    const auto stack = [this](Writer& out, const WireStack& s) { slot(out, s); };
    w.varint(static_cast<std::int32_t>(entries.size()));
    for (const auto& e : entries) {
        w.varint(e.display);
        recipe_display(w, e.recipe->display, item, stack);
        w.varint(e.recipe->group).varint(e.recipe->category);
        w.boolean(true).varint(static_cast<std::int32_t>(e.recipe->requirements.size()));
        for (const auto& r : e.recipe->requirements) {
            if (!r.tag.empty()) {
                w.varint(0).string(r.tag);
            } else {
                w.varint(static_cast<std::int32_t>(r.items.size()) + 1);
                for (const auto i : r.items) w.varint(item(i));
            }
        }
        w.u8(e.flags);
    }
    w.boolean(replace);
    return packet(id_->recipe_book_add, w);
}

Bytes Wire::open_book(std::int32_t hand) const {
    Writer w;
    w.varint(hand);
    return packet(id_->open_book, w);
}

Bytes Wire::place_ghost_recipe(std::int32_t window, const BookRecipe& recipe, const std::function<std::int32_t(std::uint16_t)>& item) const {
    Writer w;
    const auto stack = [this](Writer& out, const WireStack& s) { slot(out, s); };
    w.varint(window);
    recipe_display(w, recipe.display, item, stack);
    return packet(id_->place_ghost_recipe, w);
}

Bytes Wire::recipe_book_remove(std::span<const std::int32_t> ids) const {
    Writer w;
    w.varint(static_cast<std::int32_t>(ids.size()));
    for (const auto id : ids) w.varint(id);
    return packet(id_->recipe_book_remove, w);
}

Bytes Wire::update_recipes(const std::vector<PropertySet>& property_sets, const std::vector<StonecutterEntry>& stonecutter) const {
    Writer w;
    w.varint(static_cast<std::int32_t>(property_sets.size()));
    for (const auto& set : property_sets) {
        w.string(set.id);
        w.varint(static_cast<std::int32_t>(set.items.size()));
        for (const auto item : set.items) w.varint(item);
    }
    w.varint(static_cast<std::int32_t>(stonecutter.size()));
    for (const auto& e : stonecutter) {
        // The input as a list of item ids (an id set of n + 1), then the
        // result shown as a stack (slot display 3, item_stack).
        w.varint(static_cast<std::int32_t>(e.input.size()) + 1);
        for (const auto item : e.input) w.varint(item);
        w.varint(3);
        slot(w, e.result);
    }
    return packet(id_->update_recipes, w);
}
Bytes Wire::cursor_item(const WireStack& stack) const {
    Writer w;
    slot(w, stack);
    return packet(id_->set_cursor_item, w);
}
Bytes Wire::player_inventory(std::int32_t index, const WireStack& stack) const {
    Writer w;
    w.varint(index);
    slot(w, stack);
    return packet(id_->set_player_inventory, w);
}
Bytes Wire::open_screen(std::int32_t window, std::int32_t menu_type, const protocol::nbt::Tag& title) const {
    Writer w;
    w.varint(window).varint(menu_type);
    protocol::nbt::write_network(w, title);
    return packet(id_->open_screen, w);
}
Bytes Wire::close_screen(std::int32_t window) const { return packet(id_->container_close, Writer().varint(window)); }
Bytes Wire::play_disconnect(std::string_view reason) const {
    Writer w;
    if (reason.starts_with("translate:")) {
        // A key the client translates (see Peer::kick).
        auto t = nbt::Tag::compound();
        t.set("translate", nbt::Tag::string(std::string(reason.substr(10))));
        nbt::write_network(w, t);
    } else {
        nbt::write_network(w, nbt::text(std::string(reason)));
    }
    return packet(id_->disconnect, w);
}
Bytes Wire::empty_commands() const {
    // One root node (type 0, no children) as the whole command graph.
    return packet(id_->commands, Writer().varint(1).u8(0).varint(0).varint(0));
}

namespace {
template <class T>
std::expected<std::optional<Intent>, CodecError> done(T value) {
    return std::optional<Intent>{Intent{std::move(value)}};
}
#define TRY(var, expr)                                  \
    auto var = (expr);                                  \
    if (!var) return std::unexpected(var.error());
} // namespace

std::expected<std::optional<Intent>, CodecError> Wire::parse_play(std::int32_t id, protocol::Reader& r) const {
    const auto& i = *id_;
    if (id == i.sb_accept_teleportation) {
        TRY(t, r.varint());
        return done(intent::ConfirmTeleport{*t});
    }
    if (id == i.sb_move_pos || id == i.sb_move_pos_rot || id == i.sb_move_rot || id == i.sb_move_status) {
        intent::Move m;
        if (id == i.sb_move_pos || id == i.sb_move_pos_rot) {
            TRY(x, r.f64());
            TRY(y, r.f64());
            TRY(z, r.f64());
            m.position = Vec3{*x, *y, *z};
        }
        if (id == i.sb_move_pos_rot || id == i.sb_move_rot) {
            TRY(yaw, r.f32());
            TRY(pitch, r.f32());
            m.rotation = std::pair{*yaw, *pitch};
        }
        TRY(flags, r.u8());
        m.on_ground = (*flags & 1) != 0;
        return done(m);
    }
    if (id == i.sb_player_action) {
        TRY(status, r.varint());
        TRY(pos, r.position());
        TRY(face, r.i8());
        TRY(seq, r.varint());
        return done(intent::Dig{*status, *pos, *face, *seq});
    }
    if (id == i.sb_use_item_on) {
        TRY(hand, r.varint());
        TRY(pos, r.position());
        TRY(face, r.varint());
        TRY(cx, r.f32());
        TRY(cy, r.f32());
        TRY(cz, r.f32());
        TRY(inside, r.boolean());
        TRY(border, r.boolean());
        TRY(seq, r.varint());
        return done(intent::UseOn{*hand, *pos, *face, *cx, *cy, *cz, *inside, *seq});
    }
    if (id == i.sb_use_item) {
        TRY(hand, r.varint());
        TRY(seq, r.varint());
        TRY(yaw, r.f32());
        TRY(pitch, r.f32());
        return done(intent::Use{*hand, *seq, *yaw, *pitch});
    }
    if (id == i.sb_set_carried_item) {
        TRY(slot, r.i16());
        return done(intent::HeldSlot{*slot});
    }
    if (id == i.sb_set_creative_slot) {
        TRY(slot, r.i16());
        TRY(count, r.varint());
        intent::CreativeSlot c{*slot, *count, 0, 0, {}, {}};
        if (*count > 0) {
            TRY(it, r.varint());
            c.item = *it;
            // 1.21.5 and later prefix each component value with its length.
            auto patch = components_->read(r, components_ctx_, protocol_ >= 770);
            if (!patch) {
                c.refused = patch.error();
                return done(std::move(c)); // the rest of the packet is not read
            }
            c.damage = patch->damage.value_or(0);
            c.components = std::move(patch->rest);
        }
        return done(std::move(c));
    }
    if (id == i.sb_container_click) {
        // Window, state id, slot, button and click type, then the slots the
        // client predicted to change and its predicted carried stack. The
        // server applies the click itself; the prediction only tells it which
        // slots the client may now show wrongly. Protocol 772 sends hashed
        // stacks, whose components the server cannot compare; 769 sends whole
        // stacks with their components.
        TRY(window, r.varint());
        TRY(state, r.varint());
        TRY(slot_index, r.i16());
        TRY(button, r.i8());
        TRY(mode, r.varint());
        intent::ContainerClick click;
        click.window = *window;
        click.state = *state;
        click.slot = *slot_index;
        click.button = *button;
        click.mode = *mode;
        // A predicted stack: nullopt when unreadable; a nullopt inside when
        // its components are hashed.
        using Predicted = std::optional<std::optional<WireStack>>;
        const auto predicted = [&]() -> Predicted {
            if (protocol_ >= 770) {
                const auto present = r.boolean();
                if (!present) return std::nullopt;
                if (!*present) return std::optional<WireStack>{WireStack{}};
                const auto item = r.varint();
                const auto count = r.varint();
                const auto added = r.varint();
                if (!item || !count || !added || *added < 0 || *added > 256) return std::nullopt;
                for (std::int32_t k = 0; k < *added; ++k)
                    if (!r.varint() || !r.i32()) return std::nullopt;
                const auto removed = r.varint();
                if (!removed || *removed < 0 || *removed > 256) return std::nullopt;
                for (std::int32_t k = 0; k < *removed; ++k)
                    if (!r.varint()) return std::nullopt;
                // Component values are hashed: only a stack without any is known.
                if (*added != 0 || *removed != 0) return std::optional<WireStack>{};
                return std::optional<WireStack>{WireStack{*item, *count, 0}};
            }
            const auto count = r.varint();
            if (!count) return std::nullopt;
            if (*count <= 0) return std::optional<WireStack>{WireStack{}};
            const auto item = r.varint();
            if (!item) return std::nullopt;
            auto patch = components_->read(r, components_ctx_);
            if (!patch) return std::nullopt; // refused components leave the rest unreadable
            return std::optional<WireStack>{WireStack{*item, *count, patch->damage.value_or(0), std::move(patch->rest)}};
        };
        const auto n = r.varint();
        if (!n || *n < 0 || *n > 128) {
            click.unknown = true;
        } else {
            for (std::int32_t k = 0; k < *n && !click.unknown; ++k) {
                const auto at = r.i16();
                const auto stack = at ? predicted() : Predicted{};
                if (!stack) click.unknown = true;
                else click.changed.emplace_back(*at, *stack);
            }
            if (!click.unknown) {
                if (const auto carried = predicted()) click.carried = *carried;
                else click.unknown = true;
            }
        }
        return done(std::move(click));
    }
    if (id == i.sb_client_command) {
        TRY(action, r.varint());
        return done(intent::ClientCommand{*action});
    }
    if (id == i.sb_container_close) {
        TRY(window, r.varint());
        return done(intent::ContainerClose{*window});
    }
    if (id == i.sb_container_button_click) {
        TRY(window, r.varint());
        TRY(button, r.varint());
        return done(intent::ContainerButton{*window, *button});
    }
    if (id == i.sb_rename_item) {
        TRY(name, r.string(32767));
        return done(intent::RenameItem{std::move(*name)});
    }
    if (id == i.sb_recipe_book_seen_recipe) {
        TRY(display, r.varint());
        return done(intent::RecipeSeen{*display});
    }
    // A book's edit: at most 100 pages of 1024 UTF-16 code units and a title
    // of 32; more fails to decode, and the connection closes (client/books).
    if (id == i.sb_edit_book) {
        TRY(hand, r.varint());
        TRY(count, r.varint());
        if (*count < 0 || *count > static_cast<std::int32_t>(contracts::max_book_pages)) return std::unexpected(CodecError::too_large);
        intent::EditBook e{*hand, {}, std::nullopt};
        for (std::int32_t k = 0; k < *count; ++k) {
            TRY(page, r.string(contracts::max_page_units));
            e.pages.push_back(std::move(*page));
        }
        TRY(titled, r.boolean());
        if (*titled) {
            TRY(title, r.string(contracts::max_title_units));
            e.title = std::move(*title);
        }
        return done(std::move(e));
    }
    if (id == i.sb_place_recipe) {
        TRY(window, r.varint());
        TRY(display, r.varint());
        TRY(all, r.boolean());
        return done(intent::PlaceRecipe{*window, *display, *all});
    }
    if (id == i.sb_recipe_book_change_settings) {
        TRY(book, r.varint());
        TRY(open, r.boolean());
        TRY(filtering, r.boolean());
        return done(intent::RecipeBookSettings{*book, *open, *filtering});
    }
    if (id == i.sb_container_slot_state_changed) {
        TRY(slot_index, r.varint());
        TRY(window, r.varint());
        TRY(state, r.boolean());
        return done(intent::SlotState{*window, *slot_index, *state});
    }
    if (id == i.sb_chat_command || id == i.sb_chat_command_signed) {
        TRY(text, r.string(32767));
        return done(intent::Command{std::move(*text)});
    }
    if (id == i.sb_chat) {
        TRY(text, r.string(256));
        return done(intent::Chat{std::move(*text)});
    }
    if (id == i.sb_player_input) {
        TRY(flags, r.u8());
        return done(intent::Sneak{(*flags & 0x20) != 0});
    }
    if (id == i.sb_player_abilities) {
        TRY(flags, r.u8());
        return done(intent::Flying{(*flags & 0x02) != 0});
    }
    if (id == i.sb_player_command) {
        // Entity id, action and jump boost; only sprinting is used. Protocol
        // 769 still lists the sneaking actions first.
        TRY(entity, r.varint());
        TRY(action, r.varint());
        const std::int32_t start = protocol_ >= 770 ? 1 : 3;
        if (*action == start) return done(intent::Sprint{true});
        if (*action == start + 1) return done(intent::Sprint{false});
        return std::optional<Intent>{};
    }
    if (id == i.sb_player_loaded) return done(intent::Loaded{});
    return std::optional<Intent>{};
}
#undef TRY

} // namespace circuit::server
