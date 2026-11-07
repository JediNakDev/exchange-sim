#include "exsim/api/sha256.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace exsim::api {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotate_right(std::uint32_t value, int bits) noexcept { return (value >> bits) | (value << (32 - bits)); }

void compress_block(std::array<std::uint32_t, 8>& state, const unsigned char* block) noexcept {
    std::array<std::uint32_t, 64> schedule{};
    for (int index = 0; index < 16; ++index)
        schedule[index] = (std::uint32_t{block[4 * index]} << 24) | (std::uint32_t{block[4 * index + 1]} << 16) |
                          (std::uint32_t{block[4 * index + 2]} << 8) | std::uint32_t{block[4 * index + 3]};
    for (int index = 16; index < 64; ++index) {
        const std::uint32_t sigma0 = rotate_right(schedule[index - 15], 7) ^ rotate_right(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3);
        const std::uint32_t sigma1 = rotate_right(schedule[index - 2], 17) ^ rotate_right(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10);
        schedule[index] = schedule[index - 16] + sigma0 + schedule[index - 7] + sigma1;
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
    for (int index = 0; index < 64; ++index) {
        const std::uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temporary1 = h + sum1 + choose + kRoundConstants[index] + schedule[index];
        const std::uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}  // namespace

std::string sha256_hex(std::string_view bytes) {
    std::array<std::uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::size_t offset = 0;
    while (bytes.size() - offset >= 64) {
        compress_block(state, reinterpret_cast<const unsigned char*>(bytes.data() + offset));
        offset += 64;
    }
    unsigned char tail[128] = {};
    const std::size_t remaining = bytes.size() - offset;
    for (std::size_t index = 0; index < remaining; ++index) tail[index] = static_cast<unsigned char>(bytes[offset + index]);
    tail[remaining] = 0x80;
    const std::size_t tail_length = remaining + 1 + 8 <= 64 ? 64 : 128;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(bytes.size()) * 8;
    for (int index = 0; index < 8; ++index) tail[tail_length - 1 - index] = static_cast<unsigned char>(bit_length >> (8 * index));
    compress_block(state, tail);
    if (tail_length == 128) compress_block(state, tail + 64);
    std::string hex;
    hex.reserve(64);
    char buffer[9];
    for (const std::uint32_t word : state) {
        std::snprintf(buffer, sizeof(buffer), "%08x", word);
        hex += buffer;
    }
    return hex;
}

std::string sha256_file_hex(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open artifact for SHA256");
    std::array<std::uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char block[128]{};
    std::uint64_t total = 0;
    std::size_t remaining = 0;
    for (;;) {
        input.read(reinterpret_cast<char*>(block), 64);
        remaining = static_cast<std::size_t>(input.gcount());
        total += remaining;
        if (remaining < 64) break;
        compress_block(state, block);
    }
    if (input.bad()) throw std::runtime_error("cannot read artifact for SHA256");
    block[remaining] = 0x80;
    const std::size_t length = remaining + 9 <= 64 ? 64 : 128;
    for (std::size_t i = remaining + 1; i < length; ++i) block[i] = 0;
    for (int i = 0; i < 8; ++i) block[length - 1 - i] = static_cast<unsigned char>((total * 8) >> (8 * i));
    compress_block(state, block);
    if (length == 128) compress_block(state, block + 64);
    std::string hex;
    char text[9];
    for (const auto word : state) { std::snprintf(text, sizeof(text), "%08x", word); hex += text; }
    return hex;
}

}  // namespace exsim::api
