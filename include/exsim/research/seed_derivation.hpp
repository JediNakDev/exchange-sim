#ifndef EXSIM_RESEARCH_SEED_DERIVATION_HPP
#define EXSIM_RESEARCH_SEED_DERIVATION_HPP

// Independent RNG stream seeds derived from one master seed.
//
//   label_hash  = FNV-1a 64 of the UTF-8 stream label
//   stream_seed = splitmix64_finalize(master_seed XOR label_hash
//                                     + 0x9E3779B97F4A7C15)   (mod 2^64)
//
// splitmix64_finalize(z): z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
//                         z = (z ^ (z >> 27)) * 0x94D049BB133111EB;
//                         return z ^ (z >> 31).
//
// Stream labels used by the runner (recorded in manifest.json):
//   "strategy", "latency", "agent.<index>.<kind>.<name>".
// Changing the strategy never changes a background agent's stream, so paired
// runs with the same master seed share their exogenous randomness.

#include <cstdint>
#include <string_view>

namespace exsim::research {

[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

[[nodiscard]] constexpr std::uint64_t splitmix64_finalize(std::uint64_t z) noexcept {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

[[nodiscard]] constexpr std::uint64_t derive_stream_seed(std::uint64_t master_seed, std::string_view stream_label) noexcept {
    return splitmix64_finalize((master_seed ^ fnv1a64(stream_label)) + 0x9E3779B97F4A7C15ull);
}

inline constexpr std::string_view kSeedDerivationDescription =
    "stream_seed = splitmix64_finalize((master_seed XOR fnv1a64(stream_label)) + 0x9E3779B97F4A7C15)";

}  // namespace exsim::research

#endif
