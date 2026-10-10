// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/contracts/time.hpp>
#include <cstdint>
#include <initializer_list>

namespace circuit::gameplay {

// SplitMix64 stream. Deterministic, cheap to derive, and value-copyable so a
// retried computation can restart from the same state. It is Circuit's own
// generator, not a vanilla algorithm.
class Rng {
public:
    explicit constexpr Rng(std::uint64_t seed) noexcept : state_(seed) {}

    constexpr std::uint64_t next() noexcept {
        ++draws_;
        state_ += 0x9E3779B97F4A7C15ULL;
        return mix(state_);
    }
    // Unbiased value in [0, bound), bound > 0.
    constexpr std::uint32_t bounded(std::uint32_t bound) noexcept {
        const std::uint64_t limit = UINT64_MAX - UINT64_MAX % bound;
        std::uint64_t x = next();
        while (x >= limit) x = next();
        return static_cast<std::uint32_t>(x % bound);
    }
    [[nodiscard]] constexpr std::uint64_t draws() const noexcept { return draws_; }

    static constexpr Rng derive(std::initializer_list<std::uint64_t> parts) noexcept {
        std::uint64_t h = 0x6A09E667F3BCC909ULL;
        for (auto p : parts) h = mix(h ^ mix(p + 0x9E3779B97F4A7C15ULL));
        return Rng(h);
    }

private:
    static constexpr std::uint64_t mix(std::uint64_t z) noexcept {
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    std::uint64_t state_;
    std::uint64_t draws_{};
};

// Random-tick stream of one section in one tick (RngPolicy::per_section_derived_stream).
constexpr Rng random_tick_stream(std::uint64_t seed, contracts::Tick tick, contracts::ChunkPos chunk,
                                 std::int32_t section) noexcept {
    return Rng::derive({seed, tick, static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.x)),
                        static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.z)),
                        static_cast<std::uint64_t>(static_cast<std::uint32_t>(section))});
}
// In another level: the level joins the derivation, so equal coordinates in
// two levels draw independent streams. The overworld's streams are the ones above.
constexpr Rng random_tick_stream(std::uint64_t seed, contracts::Tick tick, contracts::DimensionId level,
                                 contracts::ChunkPos chunk, std::int32_t section) noexcept {
    if (level == contracts::DimensionId::overworld) return random_tick_stream(seed, tick, chunk, section);
    return Rng::derive({seed, tick, static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.x)),
                        static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.z)),
                        static_cast<std::uint64_t>(static_cast<std::uint32_t>(section)),
                        static_cast<std::uint64_t>(level)});
}

} // namespace circuit::gameplay
