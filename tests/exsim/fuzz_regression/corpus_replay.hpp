#ifndef TESTS_EXSIM_FUZZ_REGRESSION_CORPUS_REPLAY_HPP
#define TESTS_EXSIM_FUZZ_REGRESSION_CORPUS_REPLAY_HPP

// Replays committed fuzz inputs through the same target bodies libFuzzer runs
// (fuzz/targets/*.hpp), so every seed, every corpus entry a campaign kept and
// every minimized crash input is re-checked on each `make test`, on macOS and
// Linux, without libFuzzer.

#include "../../framework/test.hpp"
#include "fuzz/targets/fuzz_support.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fuzz_regression {

using FuzzTargetFunction = int (*)(const std::uint8_t*, std::size_t);

inline std::filesystem::path repository_root() {
    return std::filesystem::path(__FILE__).parent_path() / ".." / ".." / "..";
}

// Every regular file under fuzz/corpus/<relative_directory>, sorted so the
// replay order (and any failure message) is stable.
inline std::vector<std::filesystem::path> corpus_files(const std::string& relative_directory) {
    std::vector<std::filesystem::path> files;
    const std::filesystem::path directory = repository_root() / "fuzz" / "corpus" / relative_directory;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, error)) {
        if (entry.is_regular_file()) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

inline std::vector<std::uint8_t> read_input(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// Runs every input; returns how many were replayed. A violated property or
// an exception escaping the target fails the test with the input's path.
inline std::size_t replay_corpus(const std::string& relative_directory, FuzzTargetFunction target) {
    const auto files = corpus_files(relative_directory);
    for (const auto& path : files) {
        const std::vector<std::uint8_t> input = read_input(path);
        try {
            (void)target(input.data(), input.size());
        } catch (const exsim::fuzz::FuzzPropertyViolation& violation) {
            FAIL(path.string() + ": " + violation.description + " at " + violation.source_file + ":" +
                 std::to_string(violation.source_line));
        } catch (const std::exception& error) {
            FAIL(path.string() + ": exception escaped the target: " + error.what());
        }
    }
    return files.size();
}

}  // namespace fuzz_regression

#define FUZZ_REPLAY_TEST(target_name)                                                        \
    TEST_CASE(fuzz_regression, target_name) {                                                \
        const std::size_t replayed =                                                         \
            ::fuzz_regression::replay_corpus(#target_name, &::exsim::fuzz::fuzz_##target_name); \
        REQUIRE(replayed > 0);                                                               \
    }

#endif
