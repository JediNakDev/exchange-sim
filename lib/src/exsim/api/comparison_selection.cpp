#include "api/comparison_selection.hpp"

#include <stdexcept>

namespace exsim::api {
namespace {
constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
}

std::string encode_run_selection(const std::vector<std::string>& ids) {
    if (ids.empty()) return "none";
    if (ids.size() > 512) throw std::invalid_argument("too many selected runs");
    std::string bytes;
    for (const auto& id : ids) {
        if (id.size() != 20 || !id.starts_with("run_")) throw std::invalid_argument("invalid selected run identity");
        for (std::size_t i = 4; i < 20; i += 2) {
            const auto hex = [](char c) -> unsigned {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                throw std::invalid_argument("invalid run hex");
            };
            bytes += static_cast<char>((hex(id[i]) << 4) | hex(id[i + 1]));
        }
    }
    std::string token = "r1_";
    unsigned accumulator = 0, bits = 0;
    for (const unsigned char byte : bytes) {
        accumulator = (accumulator << 8) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            token += kAlphabet[(accumulator >> bits) & 63];
        }
    }
    if (bits) token += kAlphabet[(accumulator << (6 - bits)) & 63];
    return token;
}

std::optional<std::vector<std::string>> decode_run_selection(std::string_view token) {
    if (!token.starts_with("r1_") || token.size() > 5500) return std::nullopt;
    std::string bytes;
    unsigned accumulator = 0, bits = 0;
    for (const char c : token.substr(3)) {
        const auto value = kAlphabet.find(c);
        if (value == std::string_view::npos) return std::nullopt;
        accumulator = (accumulator << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes += static_cast<char>((accumulator >> bits) & 255);
        }
    }
    if (bytes.empty() || bytes.size() % 8 || bytes.size() > 4096 || (bits && (accumulator & ((1u << bits) - 1)))) return std::nullopt;
    std::vector<std::string> ids;
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < bytes.size(); i += 8) {
        std::string id = "run_";
        for (std::size_t j = 0; j < 8; ++j) {
            const auto byte = static_cast<unsigned char>(bytes[i + j]);
            id += hex[byte >> 4];
            id += hex[byte & 15];
        }
        ids.push_back(std::move(id));
    }
    if (encode_run_selection(ids) != token) return std::nullopt;
    return ids;
}
}  // namespace exsim::api
