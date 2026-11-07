#ifndef EXSIM_API_COMPARISON_SELECTION_HPP
#define EXSIM_API_COMPARISON_SELECTION_HPP
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace exsim::api {
// Compact exact run identities so 512 selected runs fit the 8 KiB request line.
std::string encode_run_selection(const std::vector<std::string>& ids);
std::optional<std::vector<std::string>> decode_run_selection(std::string_view token);
}
#endif
