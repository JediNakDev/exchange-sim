#ifndef EXSIM_MD_RANDOM_HPP
#define EXSIM_MD_RANDOM_HPP

// Small, fast, fully deterministic PRNG for packet fault injection. Same
// seed => same sequence on every platform; never touches global state.

#include <cstdint>

namespace exsim::md {

// xoshiro256** seeded through splitmix64.
class Rng {
public:
    explicit constexpr Rng(std::uint64_t seed = 0x5EEDull) noexcept { reseed(seed); }

    constexpr void reseed(std::uint64_t seed) noexcept {
        for (auto& w : s_) {
            seed += 0x9E3779B97F4A7C15ull;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            w = z ^ (z >> 31);
        }
    }

    constexpr std::uint64_t next() noexcept {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Uniform in [0, n) (Lemire multiply-shift; bias < n / 2^32, fine for
    // simulation). n == 0 returns 0.
    constexpr std::uint32_t below(std::uint32_t n) noexcept {
        return static_cast<std::uint32_t>(((next() >> 32) * n) >> 32);
    }

    // Uniform in [lo, hi] (inclusive), hi >= lo.
    constexpr std::int64_t between(std::int64_t lo, std::int64_t hi) noexcept {
        const auto span = static_cast<std::uint64_t>(hi - lo) + 1;
        if (span == 0) {  // full 64-bit range
            return static_cast<std::int64_t>(next());
        }
        return lo + static_cast<std::int64_t>(next() % span);
    }

    // True with probability ppm / 1'000'000.
    constexpr bool chance_ppm(std::uint32_t ppm) noexcept { return ppm != 0 && below(1'000'000) < ppm; }

private:
    static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }

    std::uint64_t s_[4]{};
};

}  // namespace exsim::md

#endif
