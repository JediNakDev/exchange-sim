#ifndef EXSIM_API_SHA256_HPP
#define EXSIM_API_SHA256_HPP

// SHA-256 (FIPS 180-4) for configuration digests. The contract names sha256
// for configuration and input data digests and fnv1a_64 for run digests.

#include <string>
#include <string_view>

namespace exsim::api {

// Lower-case hex SHA-256 of `bytes` (64 characters).
[[nodiscard]] std::string sha256_hex(std::string_view bytes);

}  // namespace exsim::api

#endif
