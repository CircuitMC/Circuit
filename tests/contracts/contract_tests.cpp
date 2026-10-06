// SPDX-License-Identifier: AGPL-3.0-only
#include "../support/check.hpp"
#include <circuit/contracts/commands.hpp>
#include <circuit/contracts/replay.hpp>
#include <circuit/contracts/trace.hpp>
#include <circuit/contracts/version.hpp>
#include <format>
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
        require(floor_block(-1) == -1 && floor_block(-4096) == -1 && floor_block(-4097) == -2, "fixed floor");
        require(floor_block(4095) == 0 && floor_block(4096) == 1, "fixed floor positive");
        require(block_of({-1, 4096 * 3, 0}) == BlockPos{-1, 3, 0}, "block_of");
        require(!within_border({world_border, 0, 0}) && within_border({-world_border, 0, 0}), "border");
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
        require(!bundle.play_supported && !bundle.save_conversion, "no play or save claims");
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
        ReplayLog log{"java-1.21.8", "circuit-reference-rules-v4", 42, {}};
        log.ticks.push_back({1, {{1, SetBlock{{-5, 70, 9}, 0x1234, Generation{2}}},
                                 {2, SpawnItem{{-1, 2, 3}, {4, -5, 6}, {7, 8}}},
                                 {3, AddTicket{9, {-3, 4}, 2}}}});
        log.ticks.push_back({4, {{7, UseBlock{{1, 2, 3}}}, {8, InsertItem{{1, 1, 1}, 3, {2, 64}}},
                                 {9, RemoveEntity{{11}}}, {10, RemoveTicket{9}}, {11, SetBlock{{0, 0, 0}, 1, std::nullopt}}}});
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

        auto version_one = *bytes;
        version_one[4] = std::byte{1};
        require(decode_replay(version_one).error() == ErrorCode::malformed_record, "version 1 lacks the rule-set id");
        auto unnamed = log;
        unnamed.rules_id.clear();
        require(encode_replay(unnamed).error() == ErrorCode::malformed_record, "rule-set id required");
    });

    suite.run("replay records are bound to the rule set", [] {
        const auto& bundle = **BundleRegistry::select_for_simulation("java-1.21.8");
        ReplayLog log{"java-1.21.8", std::string(bundle.rules->id), 1, {}};
        require(check_replay_rules(log, bundle).has_value(), "matching rules");
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
