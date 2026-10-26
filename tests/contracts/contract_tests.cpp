// SPDX-License-Identifier: AGPL-3.0-only
#include "../support/check.hpp"
#include <circuit/contracts/commands.hpp>
#include <circuit/contracts/replay.hpp>
#include <circuit/contracts/trace.hpp>
#include <circuit/contracts/version.hpp>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <set>

using namespace circuit::contracts;
using circuit::test::require;

int main() {
    circuit::test::Suite suite("contract_tests");

    suite.run("negative coordinates", [] {
        require(chunk_of({-1, 0, -1}) == ChunkPos{-1, -1}, "-1 belongs to chunk -1");
        require(chunk_of({-16, 0, -17}) == ChunkPos{-1, -2}, "floor division at -16/-17");
        require(chunk_of({15, 0, 16}) == ChunkPos{0, 1}, "positive boundary");
        require(section_local_index({-1, -1, -1}) == 4095, "negative local index wraps to 15");
        require(section_local_index({16, 17, 18}) == (1u << 8 | 2u << 4 | 0u), "local index layout");
        require(block_of(Vec3{-0.5, 3.0, -1e-9}) == BlockPos{-1, 3, -1} && block_of(Vec3{0.999, -64.0, 15.5}) == BlockPos{0, -64, 15},
                "block_of floors");
        require(!within_border(BlockPos{world_border, 0, 0}) && within_border(BlockPos{-world_border, 0, 0}), "border");
        // Entity positions are checked before narrowing to a block.
        const double border = world_border;
        require(within_border(Vec3{-border, 0, std::nextafter(border, 0.0)}) && !within_border(Vec3{border, 0, 0})
                    && !within_border(Vec3{std::nextafter(-border, -1e9), 0, 0}) && !within_border(Vec3{0, 0, border}),
                "border endpoints");
        require(!within_border(Vec3{std::numeric_limits<double>::infinity(), 0, 0}) && !within_border(Vec3{0, 0, std::nan("")})
                    && !within_border(Vec3{0, std::numeric_limits<double>::quiet_NaN(), 0}) && !within_border(Vec3{1e300, 0, 0}),
                "infinite, NaN and huge coordinates");
        const WorldShape shape{-64, 384};
        require(shape.contains_position_y(-64.0) && shape.contains_position_y(std::nextafter(320.0, 0.0))
                    && !shape.contains_position_y(std::nextafter(-64.0, -100.0)) && !shape.contains_position_y(320.0),
                "height endpoints");
    });

    suite.run("directions", [] {
        std::set<std::tuple<int, int, int>> seen;
        for (auto d : all_directions) {
            const auto o = offset(d);
            const auto r = offset(opposite(d));
            require(o.dx == -r.dx && o.dy == -r.dy && o.dz == -r.dz, "opposite offsets cancel");
            require(opposite(opposite(d)) == d, "opposite is an involution");
            seen.insert({o.dx, o.dy, o.dz});
        }
        require(seen.size() == 6, "six distinct unit offsets");
        require(BlockPos{0, 0, 0}.relative(Direction::west, 3) == BlockPos{-3, 0, 0}, "relative distance");
        require(chebyshev_distance({-2, 5}, {1, 4}) == 3, "chebyshev distance");
    });

    suite.run("generations and handles", [] {
        require(Generation{4}.next() == Generation{5}, "generation increments");
        require(!Generation{UINT32_MAX}.next(), "generation exhaustion is explicit");
        const ChunkHandle a{{1, 2}, {3}}, b{{1, 2}, {4}};
        require(a != b && a < b, "handles compare by key and generation");
    });

    suite.run("error codes are stable", [] {
        require(static_cast<int>(ErrorCode::invalid_argument) == 1, "first code");
        require(static_cast<int>(ErrorCode::stale_generation) == 4, "stale generation code");
        require(static_cast<int>(ErrorCode::unknown_command) == 21, "code 21");
        require(error_name(ErrorCode::capacity_exceeded) == "capacity_exceeded", "names");
    });

    suite.run("order keys", [] {
        const OrderKey a{5, Phase::block_ticks, 9, 1}, b{5, Phase::fluid_ticks, 0, 0}, c{6, Phase::commands, 0, 0};
        require(a < b && b < c, "tick, then phase, then root, then seq");
        require(phase_order.front() == Phase::commands && phase_order.back() == Phase::block_entities, "phase order");
    });

    suite.run("bundle selection", [] {
        const auto current = BundleRegistry::select_for_simulation("java-1.21.8");
        require(current.has_value(), "first target has an executable rule set");
        const auto& bundle = **current;
        require(bundle.play_supported && bundle.save_conversion && bundle.protocol == 772 && bundle.data_version == 4440,
                "1.21.8 plays and converts saves");
        const auto second = BundleRegistry::select_for_simulation("java-1.21.4");
        require(second && (*second)->play_supported && !(*second)->save_conversion && (*second)->protocol == 769 &&
                    (*second)->rules->id == bundle.rules->id,
                "1.21.4 plays with the same rules, without save conversion");
        require(bundle.world.sections() == 24 && bundle.world.contains_y(-64) && !bundle.world.contains_y(320), "world shape");
        require(BundleRegistry::select_for_simulation("java-1.20.4").error() == ErrorCode::unsupported_version,
                "candidate without rules cannot execute");
        require(BundleRegistry::select_for_simulation("java-9.9").error() == ErrorCode::unknown_version, "unknown");
        require(BundleRegistry::find("java-1.20.4").has_value(), "candidates remain listed");
    });

    suite.run("compatibility table claims", [] {
        const auto& rules = *(*BundleRegistry::select_for_simulation("java-1.21.8"))->rules;
        require(rules.mechanisms.size() >= 10, "table covers implemented mechanisms");
        std::set<std::string_view> ids;
        for (const auto& m : rules.mechanisms) {
            require(ids.insert(m.id).second, "unique mechanism ids");
            require(!m.limitation.empty(), "every row states its limitation");
            require(m.claimed != CompatibilityTier::l0_exact || m.evidence == EvidenceSource::harness_trace,
                    "exact claims need harness evidence");
            require(m.claimed != CompatibilityTier::l0_exact || m.limitation.find("Gap") == std::string_view::npos,
                    "exact claims list no gaps");
            require(m.claimed.has_value(), std::format("`{}` declares a claimed tier", m.id));
            require(m.claimed != CompatibilityTier::l2_declared_deviation || m.limitation.find("Gap") != std::string_view::npos,
                    std::format("`{}` lists the gaps behind its L2 claim", m.id));
        }
    });

    suite.run("command queue", [] {
        CommandQueue q(2);
        const auto a = q.admit(UseBlock{{0, 0, 0}});
        const auto b = q.admit(UseBlock{{1, 0, 0}});
        require(a && b && *a < *b, "admission ids increase");
        require(q.admit(UseBlock{{2, 0, 0}}).error() == ErrorCode::capacity_exceeded, "capacity");
        require(q.cancel(*a) && !q.cancel(*a), "cancel once before drain");
        const auto drained = q.drain();
        require(drained.size() == 1 && drained[0].admission == *b && q.size() == 0, "drain in admission order");
        require(!q.cancel(*b), "drained commands cannot be cancelled");
        CommandQueue rules(8);
        require(rules.admit(SetGameRule{"randomTickSpeed", 3}).has_value(), "a game rule is admitted");
        for (const auto* bad : {"", "random_tick_speed", "rule1", "aVeryLongGameRuleNameThatGoesOnAndOnPastTheSixtyFourCharacterLimit"})
            require(rules.admit(SetGameRule{bad, 0}).error() == ErrorCode::invalid_argument, std::format("rule name `{}` refused", bad));
        require(rules.admit(SetDayTime{0}).has_value() && rules.admit(SetDayTime{-1}).error() == ErrorCode::invalid_argument,
                "day times are not negative");
        require(rules.admit(SetWeather{WeatherKind::rain, 0}).has_value()
                    && rules.admit(SetWeather{WeatherKind::rain, -1}).error() == ErrorCode::invalid_argument
                    && rules.admit(SetWeather{static_cast<WeatherKind>(3), 1}).error() == ErrorCode::invalid_argument,
                "weather kinds and durations");
        require(rules.admit(SetDifficulty{3}).has_value() && rules.admit(SetDifficulty{4}).error() == ErrorCode::invalid_argument,
                "difficulties from peaceful to hard");
    });

    suite.run("trace divergence", [] {
        std::vector<TraceRecord> x(3), y(3);
        require(!first_divergence(x, y), "identical");
        y[1].a = 7;
        require(first_divergence(x, y) == 1, "first different index");
        y.pop_back();
        y[1].a = 0;
        require(first_divergence(x, y) == 2, "length difference");
        require(describe(x[0]).find("commands") != std::string::npos, "description names the phase");
    });

    suite.run("replay round trip and limits", [] {
        ReplayLog log{"java-1.21.8", "circuit-reference-rules-v5", 42, {}, "circuit-blocks-1"};
        log.ticks.push_back({1, {{1, SetBlock{{-5, 70, 9}, 0x1234, Generation{2}}},
                                 {2, SpawnItem{{-1, 2, 3}, {4, -5, 6}, {7, 8}}},
                                 {3, AddTicket{9, {-3, 4}, 2}}}});
        log.ticks.push_back({4, {{7, UseBlock{{1, 2, 3}}}, {8, InsertItem{{1, 1, 1}, 3, {2, 64}}},
                                 {9, RemoveEntity{{11}}}, {10, RemoveTicket{9}}, {11, SetBlock{{0, 0, 0}, 1, std::nullopt}}}});
        const PlayerKey alex{0x0123456789abcdefULL, 0xfedcba9876543210ULL};
        log.ticks.push_back({5, {{12, PlayerUse{alex, 36, {-1, -60, 2}, Direction::east, Direction::south, Direction::up, true, true, 2, 1}},
                                 {13, PlayerUse{alex, 45, {3, 4, 5}, Direction::down, Direction::west, Direction::west, false, false, 0, 3}},
                                 {14, PlayerBreak{alex, {-7, 8, -9}, true}},
                                 {15, SetPlayerSlot{alex, 40, {7, 1, 1561}}}}});
        log.ticks.push_back({6, {{16, PickUp{alex, {12}, 3, true}},
                                 {17, DropItem{alex, 36, true, {-4096, 8192, 5}, {1, -2, 3}}},
                                 {18, ContainerClick{alex, BlockPos{-2, 3, -4}, -999, 1, 5, false, {1, 2, 3}, {-1, 0, 1}}},
                                 {19, ContainerClick{alex, std::nullopt, 45, 40, 2, true, {}, {}}},
                                 {20, CloseContainer{alex, {7, 8, 9}, {0, 400, 0}}},
                                 {21, PlayerDeath{alex, {-40960, -245760, 81920}}}}});
        // Spawned items with a pickup delay or an age (tag 22); a closed
        // window whose items drop (tag 28); window buttons (tag 29); slot toggles (tag 30).
        log.ticks.push_back({7, {{22, SpawnItem{{0.5, 1, 0.5}, {0, 0.2, 0}, {7, 8}, 32767, -32768}}, {23, SpawnItem{{0.5, 1, 0.5}, {}, {7, 8}, 0, 5}},
                                 {24, CloseContainer{alex, {7, 8, 9}, {0, 0.3, 0}, true}}, {25, ContainerButton{alex, BlockPos{1, -2, 3}, 17}},
                                 {26, ContainerButton{alex, std::nullopt, -1}}, {27, SetSlotEnabled{alex, BlockPos{4, 5, -6}, 8, false}},
                                 {28, SetSlotEnabled{alex, BlockPos{4, 5, -6}, 0, true}}}});
        const auto bytes = encode_replay(log);
        require(bytes.has_value(), "encode");
        const auto decoded = decode_replay(*bytes);
        require(decoded.has_value() && *decoded == log, "round trip");

        auto truncated = *bytes;
        truncated.pop_back();
        require(decode_replay(truncated).error() == ErrorCode::malformed_record, "truncated input");
        auto trailing = *bytes;
        trailing.push_back(std::byte{0});
        require(decode_replay(trailing).error() == ErrorCode::malformed_record, "trailing bytes");
        auto bad_magic = *bytes;
        bad_magic[0] = std::byte{'X'};
        require(decode_replay(bad_magic).error() == ErrorCode::malformed_record, "magic");
        require(decode_replay(*bytes, {.max_bytes = 16}).error() == ErrorCode::record_too_large, "byte limit");
        require(decode_replay(*bytes, {.max_commands_per_tick = 2}).error() == ErrorCode::record_too_large,
                "per-tick command limit");

        auto unordered = log;
        unordered.ticks[1].tick = 1;
        require(encode_replay(unordered).error() == ErrorCode::malformed_record, "ticks must increase");
        auto readmitted = log;
        readmitted.ticks[1].commands[0].admission = 3;
        require(encode_replay(readmitted).error() == ErrorCode::malformed_record, "admissions must increase");

        // Corrupt the command tag of the first command (after magic, version, id, seed, counts).
        auto bad_tag = *bytes;
        const std::size_t first_tag = 4 + 2 + 2 + log.bundle_id.size() + 2 + log.rules_id.size() + 8 + 4 + 8 + 4 + 8;
        bad_tag[first_tag] = std::byte{99};
        require(decode_replay(bad_tag).error() == ErrorCode::malformed_record, "unknown command tag");

        // Player-use fields are range-checked: a face beyond the six, or reach bits beyond two.
        for (const auto& [face, allowed] : {std::pair<int, int>{6, 0}, {0, 4}}) {
            ReplayLog one{"java-1.21.8", "circuit-reference-rules-v5", 1, {}, "circuit-blocks-1"};
            one.ticks.push_back({1, {{1, PlayerUse{{}, 36, {0, 0, 0}, static_cast<Direction>(face), Direction::north, Direction::north,
                                                   false, false, static_cast<std::uint8_t>(allowed), 0}}}});
            const auto encoded = encode_replay(one);
            require(encoded && decode_replay(*encoded).error() == ErrorCode::malformed_record, "player use fields range-checked");
        }

        // Where a use clicked and the player's yaw (tag 25) round-trip; a tag
        // 24 use, which has no yaw, reads the yaw of its horizontal facing,
        // and a tag 7 use, which has neither, also a click in the middle; a
        // point outside the block is refused.
        {
            ReplayLog one{"java-1.21.8", "circuit-reference-rules-v5", 1, {}, "circuit-blocks-1"};
            PlayerUse use{alex, 36, {0, 0, 0}, Direction::north, Direction::west, Direction::west, false, false, 3, 3};
            use.cursor_x = 0.0f;
            use.cursor_y = 0.8125f;
            use.cursor_z = 1.0f;
            use.yaw = 37.5f;
            one.ticks.push_back({1, {{1, use}}});
            const auto current = encode_replay(one).value();
            require(decode_replay(current).value() == one, "a use with where it clicked and the yaw round-trips");
            const std::size_t tag =
                4 + 2 + 2 + one.bundle_id.size() + 2 + one.rules_id.size() + 2 + one.block_registry_id.size() + 8 + 4 + 8 + 4 + 8;
            require(current[tag] == std::byte{25}, "uses are written with tag 25");
            auto facing_yaw = one;
            std::get<PlayerUse>(facing_yaw.ticks[0].commands[0].body).yaw = 90.0f;
            auto twenty_four = current;
            twenty_four[tag] = std::byte{24};
            twenty_four.resize(twenty_four.size() - 4);
            require(decode_replay(twenty_four).value() == facing_yaw, "a tag 24 use has the yaw of its facing");
            auto seven = current;
            seven[tag] = std::byte{7};
            seven.resize(seven.size() - 16);
            auto middle = facing_yaw;
            std::get<PlayerUse>(middle.ticks[0].commands[0].body).cursor_x = 0.5f;
            std::get<PlayerUse>(middle.ticks[0].commands[0].body).cursor_y = 0.5f;
            std::get<PlayerUse>(middle.ticks[0].commands[0].body).cursor_z = 0.5f;
            require(decode_replay(seven).value() == middle, "a tag 7 use reads as a click in the middle");
            std::get<PlayerUse>(one.ticks[0].commands[0].body).cursor_y = 1.5f;
            require(decode_replay(encode_replay(one).value()).error() == ErrorCode::malformed_record, "a click outside the block is refused");
        }

        // A use of an item along the view (tag 26) round-trips with and
        // without the block it met; a view out of range is refused.
        {
            ReplayLog one{"java-1.21.8", "circuit-reference-rules-v5", 1, {}, "circuit-blocks-1"};
            PlayerUseItem use{alex, 37, {0.5, 2.62, 1.5}, -12.5f, 61.25f, 4.5, true, BlockHit{{0, -1, 0}, Direction::up, 0.5f, 1.0f, 0.3f}};
            one.ticks.push_back({1, {{1, use}, {2, PlayerUseItem{alex, 36, {3.0, 4.0, 5.0}, 180.0f, -90.0f, 5.0, false, std::nullopt}}}});
            const auto current = encode_replay(one).value();
            require(decode_replay(current).value() == one, "uses of items along the view round-trip");
            const std::size_t tag =
                4 + 2 + 2 + one.bundle_id.size() + 2 + one.rules_id.size() + 2 + one.block_registry_id.size() + 8 + 4 + 8 + 4 + 8;
            require(current[tag] == std::byte{26}, "uses of items are written with tag 26");
            for (const auto& bad : {PlayerUseItem{alex, 36, {0, 0, 0}, 0.0f, 91.0f, 4.5, false, std::nullopt},
                                    PlayerUseItem{alex, 36, {0, 0, 0}, 0.0f, 0.0f, 65.0, false, std::nullopt},
                                    PlayerUseItem{alex, 36, {0, 0, 0}, 0.0f, 0.0f, 4.5, false, BlockHit{{0, 0, 0}, Direction::up, 0.5f, 1.5f, 0.5f}}}) {
                ReplayLog wrong{"java-1.21.8", "circuit-reference-rules-v5", 1, {}, "circuit-blocks-1"};
                wrong.ticks.push_back({1, {{1, bad}}});
                require(decode_replay(encode_replay(wrong).value()).error() == ErrorCode::malformed_record, "a use out of range is refused");
            }
        }

        // Radius tickets of earlier records (tag 9 with a load radius, tag 5
        // without) read as forced squares; typed tickets round-trip.
        {
            ReplayLog one{"java-1.21.8", "circuit-reference-rules-v5", 1, {}, "circuit-blocks-1"};
            one.ticks.push_back({1, {{1, AddTicket{9, {-3, 4}, 2}}}});
            const auto current = encode_replay(one).value();
            const std::size_t tag =
                4 + 2 + 2 + one.bundle_id.size() + 2 + one.rules_id.size() + 2 + one.block_registry_id.size() + 8 + 4 + 8 + 4 + 8;
            require(current[tag] == std::byte{16}, "tickets are written with tag 16");
            // tag, level byte, ticket, x, z, radius, then type, level and lifetime (6 bytes)
            auto nine = current;
            nine[tag] = std::byte{9};
            nine.resize(nine.size() - 6);
            nine.push_back(std::byte{7}); // a load radius, not kept
            require(decode_replay(nine).value() == one, "a tag 9 ticket decodes as a forced square");
            auto five = nine;
            five[tag] = std::byte{5};
            five.pop_back();
            require(decode_replay(five).value() == one, "a tag 5 ticket decodes as a forced square");
            ReplayLog typed{"java-1.21.8", "circuit-reference-rules-v5", 1, {}, "circuit-blocks-1"};
            typed.ticks.push_back({1, {{1, AddTicket{3, {5, -6}, 10, TicketType::player_loading, 31, 0}},
                                       {2, AddTicket{4, {5, -6}, 0, TicketType::post_teleport, 32, 5}},
                                       {3, PlayerPresence{{1, 2}, {4096, -8192, 12}, true, true}},
                                       {4, PlayerPresence{{1, 2}, {}, false, false}},
                                       {5, LevelsStarted{}}}});
            require(decode_replay(encode_replay(typed).value()).value() == typed, "typed tickets, presences and starts round-trip");
            auto bad_type = encode_replay(one).value();
            bad_type[bad_type.size() - 6] = std::byte{9};
            require(decode_replay(bad_type).error() == ErrorCode::malformed_record, "an unknown ticket type is refused");
            ReplayLog rules{"java-1.21.8", "circuit-reference-rules-v12", 1, {}, "circuit-blocks-1"};
            rules.ticks.push_back({1, {{1, SetGameRule{"randomTickSpeed", -7}}, {2, SetGameRule{"keepInventory", 1}}}});
            require(decode_replay(encode_replay(rules).value()).value() == rules, "game rules round-trip");
            auto digit = encode_replay(rules).value();
            const std::string_view keep = "keepInventory";
            const auto at = std::ranges::search(digit, keep, {}, {}, [](char ch) { return static_cast<std::byte>(ch); });
            require(!at.empty(), "the rule name is written");
            at.front() = std::byte{'1'};
            require(decode_replay(digit).error() == ErrorCode::malformed_record, "a rule name of letters only");
            ReplayLog day{"java-1.21.8", "circuit-reference-rules-v12", 1, {}, "circuit-blocks-1"};
            day.ticks.push_back({1, {{1, SetDayTime{4294967318}}}});
            require(decode_replay(encode_replay(day).value()).value() == day, "day times round-trip");
            auto negative = encode_replay(day).value();
            negative[negative.size() - 1] = std::byte{0x80}; // the top byte of the little-endian time
            require(decode_replay(negative).error() == ErrorCode::malformed_record, "a negative day time is refused");
            ReplayLog weather{"java-1.21.8", "circuit-reference-rules-v12", 1, {}, "circuit-blocks-1"};
            weather.ticks.push_back({1, {{1, SetWeather{WeatherKind::thunder, 0}}, {2, SetWeather{WeatherKind::clear, 24000}}}});
            require(decode_replay(encode_replay(weather).value()).value() == weather, "weather round-trips");
            auto kind = encode_replay(weather).value();
            kind[kind.size() - 5] = std::byte{3}; // the second command's kind
            require(decode_replay(kind).error() == ErrorCode::malformed_record, "an unknown weather is refused");
            ReplayLog difficulty{"java-1.21.8", "circuit-reference-rules-v12", 1, {}, "circuit-blocks-1"};
            difficulty.ticks.push_back({1, {{1, SetDifficulty{0}}, {2, SetDifficulty{3}}}});
            require(decode_replay(encode_replay(difficulty).value()).value() == difficulty, "difficulties round-trip");
            auto hard = encode_replay(difficulty).value();
            hard[hard.size() - 1] = std::byte{4}; // the second command's difficulty
            require(decode_replay(hard).error() == ErrorCode::malformed_record, "a difficulty above hard is refused");
            ReplayLog summons{"java-1.21.8", "circuit-reference-rules-v12", 1, {}, "circuit-blocks-1"};
            summons.ticks.push_back({1, {{1, SummonEntity{"minecraft:arrow", {0.5, 3.0, 0.5}, {1.2, 0.25, 0.05}, 80, 300}},
                                         {2, SummonEntity{"minecraft:tnt", {1.5, 2.0, 0.5}, {}, 3, 0}}}});
            require(decode_replay(encode_replay(summons).value()).value() == summons, "summons round-trip");
            CommandQueue q2(4);
            require(q2.admit(SummonEntity{"minecraft:arrow", {}, {}, 80, 0}).has_value() && q2.admit(SummonEntity{"Minecraft:Arrow", {}, {}, 80, 0}).error() == ErrorCode::invalid_argument
                        && q2.admit(SummonEntity{"", {}, {}, 80, 0}).error() == ErrorCode::invalid_argument,
                    "entity types are lower-case resource ids");
        }

        auto version_one = *bytes;
        version_one[4] = std::byte{1};
        require(decode_replay(version_one).error() == ErrorCode::malformed_record, "version 1 lacks the rule-set id");
        // Version 5 commands have no level byte and are the overworld's;
        // version 4 records also have stacks without components; version 3
        // records also lack the block registry id and use the first registry.
        {
            ReplayLog old{"java-1.21.8", "circuit-reference-rules-v5", 3, {}, "circuit-blocks-1"};
            old.ticks.push_back({1, {{1, SetBlock{{1, 2, 3}, 0x1234, std::nullopt}}}});
            old.ticks.push_back({2, {{2, InsertItem{{1, 1, 1}, 3, {2, 64, 5}}}}});
            const auto current = encode_replay(old).value();
            // Each tick holds one command: tick, count, admission, tag, then the level byte.
            const auto header = static_cast<std::ptrdiff_t>(4 + 2 + 2 + old.bundle_id.size() + 2 + old.rules_id.size() + 2 +
                                                           old.block_registry_id.size() + 8 + 4);
            const std::ptrdiff_t first_level = header + 8 + 4 + 8 + 1;
            const std::ptrdiff_t second_level = first_level + 1 + 17 + 8 + 4 + 8 + 1; // after the SetBlock's 17 bytes
            require(current[static_cast<std::size_t>(first_level)] == std::byte{0} &&
                        current[static_cast<std::size_t>(second_level)] == std::byte{0},
                    "commands name their level");
            auto version_five = current;
            version_five.erase(version_five.begin() + second_level);
            version_five.erase(version_five.begin() + first_level);
            version_five[4] = std::byte{5};
            require(decode_replay(version_five).value() == old, "a version 5 record decodes as the overworld's");
            auto version_four = version_five;
            version_four[4] = std::byte{4};
            version_four.resize(version_four.size() - 4); // the stack's empty component patch, the record's last field
            require(decode_replay(version_four).value() == old, "a version 4 record decodes without components");
            auto version_three = version_four;
            version_three[4] = std::byte{3};
            const auto at = static_cast<std::ptrdiff_t>(4 + 2 + 2 + old.bundle_id.size() + 2 + old.rules_id.size());
            version_three.erase(version_three.begin() + at,
                                version_three.begin() + at + 2 + static_cast<std::ptrdiff_t>(old.block_registry_id.size()));
            require(decode_replay(version_three).value() == old, "a version 3 record decodes under circuit-blocks-1");
            // Commands of other levels round-trip; an unknown level is refused.
            auto levels = old;
            levels.ticks[0].commands[0].dimension = DimensionId::the_nether;
            levels.ticks[1].commands[0].dimension = DimensionId::the_end;
            const auto encoded = encode_replay(levels).value();
            require(decode_replay(encoded).value() == levels, "levels round-trip");
            auto unknown = encoded;
            unknown[static_cast<std::size_t>(first_level)] = std::byte{3};
            require(decode_replay(unknown).error() == ErrorCode::malformed_record, "an unknown level is refused");
        }
        // Stacks keep their components.
        {
            ReplayLog with{"java-1.21.8", "circuit-reference-rules-v5", 3, {}, "circuit-blocks-1"};
            const auto patch = ComponentPatch::make(772, {{1, {std::byte{16}}}, {5, {std::byte{8}, std::byte{0}, std::byte{1}, std::byte{'x'}}}},
                                                    {9}, {16, std::nullopt});
            with.ticks.push_back({1, {{1, InsertItem{{1, 1, 1}, 3, {2, 1, 0, patch}}}}});
            const auto encoded = encode_replay(with).value();
            const auto decoded = decode_replay(encoded).value();
            require(decoded == with, "components round-trip");
            const auto& got = std::get<InsertItem>(decoded.ticks[0].commands[0].body).stack.components;
            require(got.typed().max_stack_size == 16 && got.added().size() == 2 && got.removed()[0] == 9, "typed fields round-trip");
            for (std::size_t cut = 1; cut < 12; ++cut) {
                auto truncated = encoded;
                truncated.resize(truncated.size() - cut);
                require(!decode_replay(truncated).has_value(), "a truncated patch is refused");
            }
        }
        auto unregistered = log;
        unregistered.block_registry_id.clear();
        require(encode_replay(unregistered).error() == ErrorCode::malformed_record, "block registry id required");
        auto unnamed = log;
        unnamed.rules_id.clear();
        require(encode_replay(unnamed).error() == ErrorCode::malformed_record, "rule-set id required");
    });

    suite.run("replay records are bound to the rule set", [] {
        const auto& bundle = **BundleRegistry::select_for_simulation("java-1.21.8");
        ReplayLog log{"java-1.21.8", std::string(bundle.rules->id), 1, {}, std::string(bundle.rules->block_registry)};
        require(check_replay_rules(log, bundle).has_value(), "matching rules");
        log.block_registry_id = "circuit-blocks-0";
        require(check_replay_rules(log, bundle).error() == ErrorCode::rules_mismatch, "another block registry is refused");
        log.block_registry_id = bundle.rules->block_registry;
        log.rules_id = "circuit-reference-rules-v0";
        require(check_replay_rules(log, bundle).error() == ErrorCode::rules_mismatch, "older rules are refused");
        log.rules_id = bundle.rules->id;
        log.bundle_id = "java-1.20.4";
        require(check_replay_rules(log, bundle).error() == ErrorCode::rules_mismatch, "bundle must match");
        require(check_replay_rules(log, **BundleRegistry::find("java-1.20.4")).error() == ErrorCode::unsupported_version,
                "bundle without rules");
        require(static_cast<int>(ErrorCode::rules_mismatch) == 22, "stable code");
    });

    return suite.finish();
}
