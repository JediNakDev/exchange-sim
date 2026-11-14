#ifndef EXSIM_PROTOCOL_WIRE_HPP
#define EXSIM_PROTOCOL_WIRE_HPP
// Explicit network byte order; callers must bounds-check before using
// loads/stores.
#include <array>
#include <cstdint>
#include <engine/instrument.hpp>
#include <limits>
#include <span>
#include <string_view>
namespace exsim::protocol {
enum class Status {
    ok,
    incomplete,
    invalid_length,
    invalid_type,
    invalid_field,
    overflow,
    full,
    checksum,
    missing_field,
    duplicate_field,
    end
};
namespace wire {
template <std::size_t N>
constexpr std::array<char, N> spaces() noexcept {
    std::array<char, N> out{};
    for (char& c : out) c = ' ';
    return out;
}
template <unsigned N>
constexpr std::uint64_t load(const char* p) noexcept {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < N; ++i) v = (v << 8) | static_cast<unsigned char>(p[i]);
    return v;
}
template <unsigned N>
constexpr void store(char* p, std::uint64_t v) noexcept {
    for (unsigned i = N; i > 0; --i) {
        p[i - 1] = static_cast<char>(v & 255);
        v >>= 8;
    }
}
constexpr auto load16(const char* p) noexcept { return static_cast<std::uint16_t>(load<2>(p)); }
constexpr auto load32(const char* p) noexcept { return static_cast<std::uint32_t>(load<4>(p)); }
constexpr auto load48(const char* p) noexcept { return load<6>(p); }
constexpr auto load64(const char* p) noexcept { return load<8>(p); }
constexpr void store16(char* p, std::uint16_t v) noexcept { store<2>(p, v); }
constexpr void store32(char* p, std::uint32_t v) noexcept { store<4>(p, v); }
constexpr void store48(char* p, std::uint64_t v) noexcept { store<6>(p, v); }
constexpr void store64(char* p, std::uint64_t v) noexcept { store<8>(p, v); }
constexpr bool alpha(char* p, std::size_t n, std::string_view s) noexcept {
    if (s.size() > n) return false;
    for (std::size_t i = 0; i < n; ++i) p[i] = i < s.size() ? s[i] : ' ';
    return true;
}
constexpr std::string_view alpha(const char* p, std::size_t n) noexcept {
    while (n && p[n - 1] == ' ') --n;
    return {p, n};
}
constexpr bool numeric(char* p, std::size_t n, std::uint64_t v) noexcept {
    std::size_t digits = 1;
    for (auto t = v; t >= 10; t /= 10) ++digits;
    if (digits > n) return false;
    for (std::size_t i = 0; i < n; ++i) p[i] = ' ';
    do {
        p[--n] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    return true;
}
constexpr bool numeric(std::string_view s, std::uint64_t& v) noexcept {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    if (s.empty()) {
        v = 0;
        return true;
    }
    std::uint64_t r = 0;
    for (char c : s) {
        if (c < '0' || c > '9' || r > (UINT64_MAX - static_cast<unsigned>(c - '0')) / 10) return false;
        r = r * 10 + static_cast<unsigned>(c - '0');
    }
    v = r;
    return true;
}
}  // namespace wire
}  // namespace exsim::protocol
#endif
