// Historical parser-bug reproducers are always replayed in the integrated
// tree. They are ordinary regression tests, including in sanitizer builds.

#include "corpus_replay.hpp"
#include "test/fuzz/targets/fix44_message.hpp"
#include "test/fuzz/targets/price_text.hpp"
#include "test/fuzz/targets/server_config.hpp"

TEST_CASE(fuzz_known_bugs, server_parse_price_overflow) {
    REQUIRE(::fuzz_regression::replay_corpus("known_bugs/price_text", &::exsim::fuzz::fuzz_price_text) > 0);
}

TEST_CASE(fuzz_known_bugs, fix44_checksum_field_without_separator) {
    REQUIRE(::fuzz_regression::replay_corpus("known_bugs/fix44_message", &::exsim::fuzz::fuzz_fix44_message) > 0);
}

TEST_CASE(fuzz_known_bugs, server_config_price_overflow_and_engine_bounds) {
    REQUIRE(::fuzz_regression::replay_corpus("known_bugs/server_config", &::exsim::fuzz::fuzz_server_config) > 0);
}
