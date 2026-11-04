#ifndef EXSIM_FUZZ_TARGETS_FUZZ_SUPPORT_HPP
#define EXSIM_FUZZ_TARGETS_FUZZ_SUPPORT_HPP

// Shared pieces of every fuzz target body.
//
// A target is `int fuzz_<name>(const std::uint8_t* data, std::size_t size)`.
// The same function runs under libFuzzer (fuzz/libfuzzer_entry.cpp) and inside
// the normal test runner (tests/exsim/fuzz_regression), so a target never
// calls abort() itself. A violated property throws FuzzPropertyViolation,
// which deliberately does not derive from std::exception: targets that catch
// std::exception from a throwing parser can never swallow it by accident.
// Memory errors and undefined behaviour are reported by the sanitizers.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace exsim::fuzz {

struct FuzzPropertyViolation {
    const char* source_file;
    int source_line;
    std::string description;
};

[[noreturn]] inline void report_property_violation(const char* source_file, int source_line,
                                                   std::string description) {
    throw FuzzPropertyViolation{source_file, source_line, std::move(description)};
}

#define EXSIM_FUZZ_REQUIRE(condition, description)                                              \
    do {                                                                                         \
        if (!(condition)) {                                                                      \
            ::exsim::fuzz::report_property_violation(__FILE__, __LINE__,                         \
                                                     std::string("property failed: " #condition  \
                                                                 " (") + (description) + ")");   \
        }                                                                                        \
    } while (0)

inline std::span<const char> as_chars(const std::uint8_t* data, std::size_t size) noexcept {
    return {reinterpret_cast<const char*>(data), size};
}

inline std::string_view as_text(const std::uint8_t* data, std::size_t size) noexcept {
    return {reinterpret_cast<const char*>(data), size};
}

inline bool same_bytes(std::span<const char> left, std::span<const char> right) noexcept {
    return left.size() == right.size() &&
           (left.empty() || std::memcmp(left.data(), right.data(), left.size()) == 0);
}

// Reads structured values out of the fuzz input. Past the end every read
// returns zero bytes, so a target never has to special-case short inputs.
class FuzzByteReader {
   public:
    FuzzByteReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] std::size_t remaining_bytes() const noexcept { return size_ - offset_; }
    [[nodiscard]] bool exhausted() const noexcept { return offset_ == size_; }

    std::uint8_t take_byte() noexcept { return offset_ < size_ ? data_[offset_++] : 0; }

    template <typename Unsigned>
    Unsigned take_unsigned() noexcept {
        Unsigned value = 0;
        for (std::size_t byte_index = 0; byte_index < sizeof(Unsigned); ++byte_index) {
            value = static_cast<Unsigned>((static_cast<std::uint64_t>(value) << 8) | take_byte());
        }
        return value;
    }

    std::uint16_t take_u16() noexcept { return take_unsigned<std::uint16_t>(); }
    std::uint32_t take_u32() noexcept { return take_unsigned<std::uint32_t>(); }
    std::uint64_t take_u64() noexcept { return take_unsigned<std::uint64_t>(); }
    bool take_bool() noexcept { return (take_byte() & 1) != 0; }

    // Uniform-ish value in [0, bound) for bound > 0.
    std::size_t take_index(std::size_t bound) noexcept { return bound == 0 ? 0 : take_u32() % bound; }

    // Up to `maximum_length` bytes; the length itself is read from the input.
    std::string_view take_text(std::size_t maximum_length) noexcept {
        const std::size_t wanted = take_index(maximum_length + 1);
        const std::size_t length = wanted < remaining_bytes() ? wanted : remaining_bytes();
        const std::string_view text(reinterpret_cast<const char*>(data_ + offset_), length);
        offset_ += length;
        return text;
    }

    std::string_view take_rest() noexcept {
        const std::string_view text(reinterpret_cast<const char*>(data_ + offset_), size_ - offset_);
        offset_ = size_;
        return text;
    }

    // Mostly boundary values: fuzzers rarely guess 2^32 or INT64_MIN from raw bytes.
    std::int64_t take_interesting_int64() noexcept {
        const std::uint8_t selector = take_byte();
        const auto small_delta = static_cast<std::int64_t>(static_cast<std::int8_t>(take_byte()) % 4);
        switch (selector % 6) {
            case 0: return static_cast<std::int64_t>(take_u64());
            case 1: return static_cast<std::int64_t>(0xffffffffLL) + small_delta;
            case 2: return static_cast<std::int64_t>(0xffffffffffffLL) + small_delta;
            case 3: return small_delta;
            case 4: return small_delta >= 0 ? INT64_MAX - small_delta : INT64_MIN - small_delta;
            default: return static_cast<std::int64_t>(take_u32());
        }
    }

   private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
};

}  // namespace exsim::fuzz

#endif
