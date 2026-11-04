// libFuzzer entry point shared by every target. The Makefile compiles it once
// per target with
//
//   -include targets/<name>.hpp -DFUZZ_TARGET_FUNCTION=exsim::fuzz::fuzz_<name>
//
// so the target body itself stays a plain header that the regression tests in
// tests/exsim/fuzz_regression can replay without libFuzzer.

#include "targets/fuzz_support.hpp"

#include <cstdio>
#include <cstdlib>

#ifndef FUZZ_TARGET_FUNCTION
#error "compile with -DFUZZ_TARGET_FUNCTION=exsim::fuzz::fuzz_<name>"
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        return FUZZ_TARGET_FUNCTION(data, size);
    } catch (const exsim::fuzz::FuzzPropertyViolation& violation) {
        std::fprintf(stderr, "==exsim-fuzz== %s:%d: %s\n", violation.source_file, violation.source_line,
                     violation.description.c_str());
        std::abort();  // libFuzzer saves the input as a crash
    }
}
