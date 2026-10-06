// SPDX-License-Identifier: AGPL-3.0-only
#include "wire.hpp"
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
        chunk_batch_start, chunk_batch_finished, level_chunk, forget_chunk, block_update, block_changed_ack,
        player_position, keep_alive, system_chat, player_info_update, player_info_remove, add_entity,
        set_entity_data, remove_entities, entity_position_sync, rotate_head, container_set_content, disconnect,
        commands;
    // Serverbound play.
    std::int32_t sb_accept_teleportation, sb_move_pos, sb_move_pos_rot, sb_move_rot, sb_move_status,
        sb_player_action, sb_use_item_on, sb_use_item, sb_set_carried_item, sb_set_creative_slot, sb_chat_command,
        sb_chat_command_signed, sb_chat, sb_player_input, sb_player_abilities, sb_player_loaded;
};

Wire::Wire(std::int32_t protocol) : protocol_(protocol), ids_([&]() -> const protocol::PacketIds& {
    const auto* ids = protocol::PacketIds::for_protocol(protocol);
    if (!ids) throw std::invalid_argument(std::format("server: no packet table for protocol {}", protocol));
    return *ids;
}()) {
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
    ids->forget_chunk = cb(State::play, "forget_level_chunk");
    ids->block_update = cb(State::play, "block_update");
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
    ids->disconnect = cb(State::play, "disconnect");
    ids->commands = cb(State::play, "commands");
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

Bytes Wire::update_tags(const std::vector<RegistryTags>& tags, const std::vector<RegistryEntries>& registries) const {
    Writer w;
    w.varint(static_cast<std::int32_t>(tags.size()));
    for (const auto& r : tags) {
        w.string(r.registry).varint(static_cast<std::int32_t>(r.tags.size()));
        for (const auto& [name, entries] : r.tags) {
            w.string(name).varint(static_cast<std::int32_t>(entries.size()));
            for (const auto e : entries) w.varint(registry_index(registries, r.registry, e).value());
        }
    }
    return packet(id_->cfg_update_tags, w);
}

Bytes Wire::join_game(const JoinGame& j) const {
    Writer w;
    w.i32(j.entity_id).boolean(false);           // hardcore
    w.varint(1).string("minecraft:overworld");   // dimension names
    w.varint(j.max_players).varint(j.view_distance).varint(j.simulation_distance);
    w.boolean(false).boolean(true).boolean(false); // reduced debug info, respawn screen, limited crafting
    w.varint(j.dimension_type).string("minecraft:overworld");
    w.i64(0);                                    // hashed seed
    w.u8(j.game_mode).i8(-1);                    // game mode, previous game mode
    w.boolean(false).boolean(true);              // debug, flat
    w.boolean(false);                            // no death location
    w.varint(0);                                 // portal cooldown
    w.varint(-63);                               // sea level of the flat world
    w.boolean(false);                            // enforces secure chat
    return packet(id_->login, w);
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
Bytes Wire::forget_chunk(std::int32_t x, std::int32_t z) const { return packet(id_->forget_chunk, Writer().i32(z).i32(x)); }
Bytes Wire::block_update(WirePosition pos, std::int32_t state) const {
    return packet(id_->block_update, Writer().position(pos).varint(state));
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
                         std::int32_t data) const {
    Writer w;
    w.varint(id).uuid(uuid).varint(type).f64(pos.x).f64(pos.y).f64(pos.z);
    w.angle(pitch).angle(yaw).angle(head_yaw).varint(data).i16(0).i16(0).i16(0);
    return packet(id_->add_entity, w);
}
Bytes Wire::item_metadata(std::int32_t id, std::int32_t item, std::int32_t count) const {
    Writer w;
    w.varint(id);
    w.u8(8).varint(7); // index 8 (item entity: item stack), type Slot
    w.varint(count);
    if (count > 0) w.varint(item).varint(0).varint(0); // no components added or removed
    w.u8(0xFF);
    return packet(id_->set_entity_data, w);
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
Bytes Wire::inventory_content(std::int32_t state_id, const std::vector<std::pair<std::int32_t, std::int32_t>>& slots) const {
    Writer w;
    w.varint(0).varint(state_id).varint(static_cast<std::int32_t>(slots.size()));
    for (const auto& [item, count] : slots) {
        w.varint(count);
        if (count > 0) w.varint(item).varint(0).varint(0);
    }
    w.varint(0); // carried item: empty
    return packet(id_->container_set_content, w);
}
Bytes Wire::play_disconnect(std::string_view reason) const {
    Writer w;
    nbt::write_network(w, nbt::text(std::string(reason)));
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
        return done(intent::Use{*hand, *seq});
    }
    if (id == i.sb_set_carried_item) {
        TRY(slot, r.i16());
        return done(intent::HeldSlot{*slot});
    }
    if (id == i.sb_set_creative_slot) {
        TRY(slot, r.i16());
        TRY(count, r.varint());
        std::int32_t item = 0;
        if (*count > 0) {
            TRY(it, r.varint());
            item = *it;
            // Data components follow; the server keeps only id and count, so the
            // rest of the packet is not interpreted.
        }
        return done(intent::CreativeSlot{*slot, *count, item});
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
    if (id == i.sb_player_loaded) return done(intent::Loaded{});
    return std::optional<Intent>{};
}
#undef TRY

} // namespace circuit::server
