#ifndef EXSIM_FUZZ_TARGETS_WIRE_TEXT_FIELDS_HPP
#define EXSIM_FUZZ_TARGETS_WIRE_TEXT_FIELDS_HPP

// lib/include/protocol/wire.hpp helpers: space-padded numeric and alpha
// fields (SoupBinTCP login sequence numbers, OUCH tokens, ITCH symbols) and
// big-endian loads/stores.

#include "soupbintcp.hpp"

#include <array>
#include <protocol/wire.hpp>

namespace exsim::fuzz {

inline int fuzz_wire_text_fields(const std::uint8_t* data, std::size_t size) {
    namespace wire = protocol::wire;
    const std::string_view input = as_text(data, size);

    std::uint64_t parsed = 0;
    const auto expected = soupbintcp_fuzz::model_space_padded_number(input);
    const bool accepted = wire::numeric(input, parsed);
    EXSIM_FUZZ_REQUIRE(accepted == expected.has_value(), "numeric parse accepts exactly space-padded digits");
    if (accepted) EXSIM_FUZZ_REQUIRE(parsed == *expected, "numeric parse value");

    FuzzByteReader reader(data, size);
    const std::uint64_t value = reader.take_bool() ? reader.take_u64() : reader.take_u32() % 100000;
    const std::size_t field_width = 1 + reader.take_index(20);
    std::array<char, 20> field{};
    std::size_t digits = 1;
    for (std::uint64_t remaining = value; remaining >= 10; remaining /= 10) ++digits;
    const bool written = wire::numeric(field.data(), field_width, value);
    EXSIM_FUZZ_REQUIRE(written == (digits <= field_width), "numeric format fits exactly when the digits fit");
    if (written) {
        const std::string_view text(field.data(), field_width);
        EXSIM_FUZZ_REQUIRE(text.find_first_not_of(' ') == field_width - digits, "numbers are right aligned");
        std::uint64_t round_trip = 0;
        EXSIM_FUZZ_REQUIRE(wire::numeric(text, round_trip) && round_trip == value, "numeric field round trip");
    }

    const std::size_t alpha_width = 1 + reader.take_index(16);
    const std::string_view alpha_text = reader.take_text(20);
    std::array<char, 16> alpha_field{};
    const bool alpha_written = wire::alpha(alpha_field.data(), alpha_width, alpha_text);
    EXSIM_FUZZ_REQUIRE(alpha_written == (alpha_text.size() <= alpha_width), "alpha fits exactly when short enough");
    if (alpha_written) {
        std::string_view trimmed = alpha_text;
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.remove_suffix(1);
        EXSIM_FUZZ_REQUIRE(wire::alpha(alpha_field.data(), alpha_width) == trimmed,
                           "alpha round trip drops only trailing padding");
    }

    const std::uint64_t raw = reader.take_u64();
    char bytes[8];
    wire::store16(bytes, static_cast<std::uint16_t>(raw));
    EXSIM_FUZZ_REQUIRE(wire::load16(bytes) == static_cast<std::uint16_t>(raw), "16-bit round trip");
    wire::store32(bytes, static_cast<std::uint32_t>(raw));
    EXSIM_FUZZ_REQUIRE(wire::load32(bytes) == static_cast<std::uint32_t>(raw), "32-bit round trip");
    wire::store48(bytes, raw);
    EXSIM_FUZZ_REQUIRE(wire::load48(bytes) == (raw & 0xffffffffffffULL), "48-bit round trip keeps the low bits");
    wire::store64(bytes, raw);
    EXSIM_FUZZ_REQUIRE(wire::load64(bytes) == raw && static_cast<unsigned char>(bytes[0]) == (raw >> 56),
                       "64-bit round trip is big endian");
    return 0;
}

}  // namespace exsim::fuzz

#endif
