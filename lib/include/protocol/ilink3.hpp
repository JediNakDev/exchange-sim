#ifndef EXSIM_PROTOCOL_ILINK3_HPP
#define EXSIM_PROTOCOL_ILINK3_HPP
#include "protocol/cme_sbe.hpp"
#include "protocol/ilink3_schema.hpp"

#include <charconv>
#include <type_traits>

namespace exsim::protocol::ilink3 {
template<class T> [[nodiscard]] T get(const cme::MessageView& message, Field<T> field) noexcept {
    return cme::load_le<T>(message.body.data() + field.offset);
}
template<std::size_t N> [[nodiscard]] std::string_view get(const cme::MessageView& message, TextField<N> field) noexcept {
    const char* begin = message.body.data() + field.offset;
    std::size_t size = 0;
    while (size < N && begin[size]) ++size;
    return {begin, size};
}
template<std::size_t N> [[nodiscard]] bool valid_text(const cme::MessageView& message, TextField<N> field, bool required = true) noexcept {
    const auto text = get(message, field);
    if (required && text.empty()) return false;
    for (char ch : text) if (ch < 0x21 || ch > 0x7e) return false;
    for (std::size_t i = text.size(); i < N; ++i) if (message.body[field.offset + i]) return false;
    return true;
}
template<class Message> [[nodiscard]] bool valid(const cme::MessageView& message) noexcept {
    return message.header.schema_id == schema::id && message.header.version == schema::version &&
           message.header.template_id == Message::id && message.header.block_length >= Message::block_length &&
           message.body.size() >= message.header.block_length;
}
template<class Message> struct Root {
    std::array<char, Message::block_length> bytes = Message::defaults;
    template<class T> void set(Field<T> field, std::type_identity_t<T> value) noexcept { cme::store_le(bytes.data() + field.offset, value); }
    template<std::size_t N> bool set(TextField<N> field, std::string_view text) noexcept {
        if (text.size() > N) return false;
        std::fill_n(bytes.data() + field.offset, N, char{0});
        if (!text.empty()) std::memcpy(bytes.data() + field.offset, text.data(), text.size());
        return true;
    }
    [[nodiscard]] std::size_t encode(std::span<char> output, std::span<const char> tail = Message::empty_tail) const noexcept {
        std::array<char, 2048> body{};
        if (bytes.size() + tail.size() > body.size()) return 0;
        std::memcpy(body.data(), bytes.data(), bytes.size());
        if (!tail.empty()) std::memcpy(body.data() + bytes.size(), tail.data(), tail.size());
        return cme::encode_ilink({Message::block_length, Message::id, schema::id, schema::version},
                                std::span(body).first(bytes.size() + tail.size()), output);
    }
};

// Authentication payloads use decimal integers separated by LF, without a
// trailing LF. Fixed storage avoids allocator use while constructing them.
struct SigningText {
    std::array<char, 512> bytes{};
    std::size_t used = 0;
    bool good = true;
    void append(std::string_view value) noexcept {
        const std::size_t separator = used != 0;
        if (used + separator + value.size() > bytes.size()) { good = false; return; }
        if (separator) bytes[used++] = '\n';
        if (!value.empty()) std::memcpy(bytes.data() + used, value.data(), value.size());
        used += value.size();
    }
    void append(std::uint64_t value) noexcept {
        std::array<char, 20> digits{};
        const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
        append(std::string_view(digits.data(), static_cast<std::size_t>(result.ptr - digits.data())));
    }
    [[nodiscard]] std::string_view view() const noexcept { return {bytes.data(), used}; }
};
[[nodiscard]] bool decode_secret(std::string_view encoded, std::array<char, 32>& key) noexcept;
[[nodiscard]] std::array<char, 32> hmac_sha256(std::span<const char> key, std::string_view message) noexcept;
[[nodiscard]] bool signature_equal(std::span<const char, 32> left, std::span<const char, 32> right) noexcept;
}  // namespace exsim::protocol::ilink3
#endif
