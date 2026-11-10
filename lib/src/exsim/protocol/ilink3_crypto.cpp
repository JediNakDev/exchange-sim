#include "protocol/ilink3.hpp"
#include <exception>
#ifdef __APPLE__
#include <CommonCrypto/CommonHMAC.h>
#include <string.h>
#else
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#endif

namespace exsim::protocol::ilink3 {
bool decode_secret(std::string_view encoded, std::array<char, 32>& key) noexcept {
    if (encoded.size() == 44 && encoded.back() == '=') encoded.remove_suffix(1);
    if (encoded.size() != 43) return false;
    std::array<char, 32> result{};
    std::uint32_t bits = 0;
    unsigned count = 0;
    std::size_t size = 0;
    for (const char ch : encoded) {
        const int value = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 :
                          ch >= '0' && ch <= '9' ? ch - '0' + 52 : ch == '-' ? 62 : ch == '_' ? 63 : -1;
        if (value < 0) return false;
        bits = (bits << 6) | static_cast<unsigned>(value); count += 6;
        if (count >= 8) { count -= 8; result[size++] = static_cast<char>((bits >> count) & 255); }
    }
    if (size != 32 || (bits & ((1u << count) - 1)) != 0) return false;
    key = result;
    return true;
}
std::array<char, 32> hmac_sha256(std::span<const char> key, std::string_view message) noexcept {
    std::array<char, 32> result{};
#ifdef __APPLE__
    CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), message.data(), message.size(), result.data());
#else
    unsigned size = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(message.data()), message.size(),
              reinterpret_cast<unsigned char*>(result.data()), &size) || size != result.size()) std::terminate();
#endif
    return result;
}
bool signature_equal(std::span<const char, 32> left, std::span<const char, 32> right) noexcept {
#ifdef __APPLE__
    return timingsafe_bcmp(left.data(), right.data(), left.size()) == 0;
#else
    return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
#endif
}
}  // namespace exsim::protocol::ilink3
