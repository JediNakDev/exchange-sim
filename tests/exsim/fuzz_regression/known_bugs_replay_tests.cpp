// Reproducers for open bugs in code outside the fuzzing work (see
// fuzz/targets/known_bugs.hpp and docs/FUZZING.md "Open findings").
//
// Disabled by default: the targets steer around these triggers so campaigns
// and `make test` stay green. Once the owner has fixed a bug, run
//
//   make test EXTRA_CXXFLAGS=-DEXSIM_FUZZ_ENFORCE_KNOWN_BUGS FILTER=fuzz_known_bugs
//
// (with MODE=debug for the undefined behaviour ones) and it must pass.
#ifdef EXSIM_FUZZ_ENFORCE_KNOWN_BUGS

#include "corpus_replay.hpp"
#include "fuzz/targets/fix44_message.hpp"
#include "fuzz/targets/price_text.hpp"
#include "fuzz/targets/server_config.hpp"

TEST_CASE(fuzz_known_bugs, server_parse_price_overflow) {
    REQUIRE(::fuzz_regression::replay_corpus("known_bugs/price_text", &::exsim::fuzz::fuzz_price_text) > 0);
}

TEST_CASE(fuzz_known_bugs, fix44_checksum_field_without_separator) {
    REQUIRE(::fuzz_regression::replay_corpus("known_bugs/fix44_message", &::exsim::fuzz::fuzz_fix44_message) > 0);
}

TEST_CASE(fuzz_known_bugs, server_config_price_overflow_and_engine_bounds) {
    REQUIRE(::fuzz_regression::replay_corpus("known_bugs/server_config", &::exsim::fuzz::fuzz_server_config) > 0);
}

#endif
