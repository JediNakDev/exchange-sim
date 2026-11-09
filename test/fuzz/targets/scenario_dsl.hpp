#ifndef EXSIM_FUZZ_TARGETS_SCENARIO_DSL_HPP
#define EXSIM_FUZZ_TARGETS_SCENARIO_DSL_HPP

// Scenario DSL (sim::run_scenario) driving the production MatchingEngine.
// The whole input is a .scn file. Every malformed line, failed assertion or
// engine rejection must surface as an exception derived from std::exception
// (with the line number), never as a crash or undefined behaviour; and the
// run is deterministic, so a second run gives the same outcome.
//
// Design limit, not a bug: `config max_orders=` and `max_instruments=` size
// the engine's preallocated storage, and the DSL is test code with no upper
// bound, so the harness skips files asking for more than kMaximumOrders or
// kMaximumInstruments (it reads them with the same std::stoull the DSL uses).

#include "fuzz_support.hpp"

#include <matching_engine.hpp>
#include <sim/scenario.hpp>
#include <sstream>
#include <string>

namespace exsim::fuzz {

namespace scenario_dsl_fuzz {

inline constexpr std::uint64_t kMaximumOrders = 1u << 14;
inline constexpr std::uint64_t kMaximumInstruments = 64;

// True when some `config` line asks for an engine larger than the bounds.
inline bool requests_oversized_engine(std::string_view file_text) {
    std::istringstream file{std::string(file_text)};
    std::string text;
    while (std::getline(file, text)) {
        text = text.substr(0, text.find('#'));
        std::istringstream line(text);
        std::string operation;
        if (!(line >> operation) || operation != "config") continue;
        std::string word;
        while (line >> word) {
            const std::size_t equals = word.find('=');
            if (equals == std::string::npos) continue;
            const std::string key = word.substr(0, equals);
            const std::uint64_t bound = key == "max_orders" ? kMaximumOrders
                                        : key == "max_instruments" ? kMaximumInstruments
                                                                   : 0;
            if (bound == 0) continue;
            try {
                if (std::stoull(word.substr(equals + 1)) > bound) return true;
            } catch (const std::exception&) {
                // The DSL itself rejects this value.
            }
        }
    }
    return false;
}

inline std::string run_once(std::string_view file_text) {
    std::istringstream input{std::string(file_text)};
    try {
        sim::run_scenario<MatchingEngine>(input);
    } catch (const std::exception& error) {
        const std::string message = error.what();
        EXSIM_FUZZ_REQUIRE(message.rfind("line ", 0) == 0, "scenario errors name their line: " + message);
        return message;
    }
    return "accepted";
}

}  // namespace scenario_dsl_fuzz

inline int fuzz_scenario_dsl(const std::uint8_t* data, std::size_t size) {
    using namespace scenario_dsl_fuzz;
    const std::string_view file_text = as_text(data, size);
    if (requests_oversized_engine(file_text)) return 0;
    const std::string first_outcome = run_once(file_text);
    EXSIM_FUZZ_REQUIRE(run_once(file_text) == first_outcome, "scenario runs are deterministic");
    return 0;
}

}  // namespace exsim::fuzz

#endif
