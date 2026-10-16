// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/ids.hpp>
#include <circuit/gameplay/rng.hpp>
#include <cstdint>
#include <optional>

// A level's random sequence: the 48-bit linear congruential generator of
// java.util.Random, with the methods its API documentation specifies. Both
// official servers draw from such a sequence for their level's randomness
// (with the seed 0 the reference harness sets, the official traces of primed
// TNT start with Random(0)'s first nextDouble). Circuit's rules draw from it
// where the target versions do, in the same order, so the same seed gives
// the same values; draws count raw next(bits) calls, as the harness does.
namespace circuit::gameplay {

class LevelRandom {
public:
    static constexpr std::uint64_t multiplier = 0x5DEECE66DULL;
    static constexpr std::uint64_t mask = (std::uint64_t{1} << 48) - 1;

    constexpr LevelRandom() noexcept { set_seed(0); }
    explicit constexpr LevelRandom(std::int64_t seed) noexcept { set_seed(seed); }
    // The generator in a saved state (state()), with its draw count reset.
    static constexpr LevelRandom from_state(std::uint64_t state) noexcept {
        LevelRandom r;
        r.state_ = state & mask;
        return r;
    }

    constexpr void set_seed(std::int64_t seed) noexcept { state_ = (static_cast<std::uint64_t>(seed) ^ multiplier) & mask; }
    [[nodiscard]] constexpr std::uint64_t state() const noexcept { return state_; }
    [[nodiscard]] constexpr std::uint64_t draws() const noexcept { return draws_; }

    // The top `bits` bits of the next state, as a signed 32-bit value.
    constexpr std::int32_t next(int bits) noexcept {
        ++draws_;
        state_ = (state_ * multiplier + 0xBULL) & mask;
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(state_ >> (48 - bits)));
    }
    constexpr std::int32_t next_int() noexcept { return next(32); }
    // Uniform in [0, bound); bound > 0.
    constexpr std::int32_t next_int(std::int32_t bound) noexcept {
        if ((bound & -bound) == bound) return static_cast<std::int32_t>((std::int64_t{bound} * next(31)) >> 31);
        std::int32_t bits = 0, value = 0;
        do {
            bits = next(31);
            value = bits % bound;
        } while (static_cast<std::int64_t>(bits) - value + (bound - 1) > INT32_MAX);
        return value;
    }
    constexpr std::int64_t next_long() noexcept {
        const auto high = static_cast<std::uint64_t>(static_cast<std::int64_t>(next(32))) << 32;
        return static_cast<std::int64_t>(high + static_cast<std::uint64_t>(static_cast<std::int64_t>(next(32))));
    }
    constexpr bool next_boolean() noexcept { return next(1) != 0; }
    constexpr float next_float() noexcept { return static_cast<float>(next(24)) / static_cast<float>(1 << 24); }
    constexpr double next_double() noexcept {
        const auto high = static_cast<std::int64_t>(next(26)) << 27;
        return static_cast<double>(high + next(27)) * 0x1.0p-53;
    }

    bool operator==(const LevelRandom& o) const noexcept { return state_ == o.state_; }

private:
    std::uint64_t state_{};
    std::uint64_t draws_{};
};

// A new world's random sequence of a level: seeded with `seed` when given
// (the reference harness seeds the overworld's with 0), else derived from the
// world seed and the level.
inline LevelRandom initial_level_random(std::uint64_t world_seed, contracts::DimensionId level, std::optional<std::int64_t> seed) {
    if (seed) return LevelRandom(*seed);
    return LevelRandom(static_cast<std::int64_t>(Rng::derive({world_seed, static_cast<std::uint64_t>(level), 0x4C4556454C52414EULL}).next()));
}

} // namespace circuit::gameplay
