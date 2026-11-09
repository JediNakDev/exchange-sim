#ifndef EXSIM_FUZZ_TARGETS_ADMIN_COMMAND_HPP
#define EXSIM_FUZZ_TARGETS_ADMIN_COMMAND_HPP

// Admin console command parser (server::parse_admin, server contract section 4).
//
// 1. The whole input as one line. Contract: on failure `out` is unchanged and
//    `error` is set; on success `out` is replaced and `error` cleared. An
//    unknown verb is always rejected as an unknown command. An accepted
//    command satisfies the documented argument ranges, and its canonical
//    rendering parses back to the identical command.
// 2. Structured direction: a command built from the input bytes, rendered with
//    fuzz-chosen keyword case, whitespace and rate key order, must parse back
//    to exactly that command.

#include "fuzz_support.hpp"

#include <exsim/server/admin.hpp>
#include <array>
#include <string>
#include <vector>

namespace exsim::fuzz {

namespace admin_command_fuzz {

using server::AdminCommand;
using server::AdminOp;

inline constexpr std::array<std::string_view, 5> kPhaseNames = {"closed", "pre_open", "continuous", "halted",
                                                                 "pre_close"};

inline bool same_command(const AdminCommand& left, const AdminCommand& right) {
    return left.op == right.op && left.symbol == right.symbol && left.user == right.user &&
           left.phase == right.phase && left.count == right.count && left.delay == right.delay &&
           left.rate.drop_ppm == right.rate.drop_ppm && left.rate.duplicate_ppm == right.rate.duplicate_ppm &&
           left.rate.reorder_ppm == right.rate.reorder_ppm && left.rate.reorder_delay == right.rate.reorder_delay &&
           left.rate.seed == right.rate.seed && left.on == right.on;
}

inline bool is_admin_whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

inline std::string lower_ascii(std::string_view text) {
    std::string lowered(text);
    for (char& character : lowered) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return lowered;
}

inline std::string first_token(std::string_view line) {
    std::size_t start = 0;
    while (start < line.size() && is_admin_whitespace(line[start])) ++start;
    std::size_t end = start;
    while (end < line.size() && !is_admin_whitespace(line[end])) ++end;
    return std::string(line.substr(start, end - start));
}

// Renders keywords with a case pattern and separators chosen by the reader
// (nullptr: lower case, single spaces).
class AdminLineRenderer {
   public:
    explicit AdminLineRenderer(FuzzByteReader* reader) : reader_(reader) {}

    void keyword(std::string_view word) {
        separator();
        for (const char character : word) {
            const bool upper = reader_ != nullptr && reader_->take_bool();
            line_ += upper && character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A')
                                                                   : character;
        }
    }
    void token(std::string_view text) {
        separator();
        line_ += text;
    }
    void number(std::uint64_t value) { token(std::to_string(value)); }
    void key_value(std::string_view key, std::uint64_t value) {
        keyword(key);
        line_ += '=';
        line_ += std::to_string(value);
    }
    std::string finish() {
        if (reader_ != nullptr && reader_->take_bool()) line_ += "\r\n";
        return std::move(line_);
    }

   private:
    void separator() {
        if (reader_ == nullptr) {
            if (!line_.empty()) line_ += ' ';
            return;
        }
        static constexpr std::array<std::string_view, 4> kSeparators = {" ", "\t", "  ", " \t "};
        const std::string_view chosen = kSeparators[reader_->take_index(kSeparators.size())];
        if (!line_.empty() || reader_->take_bool()) line_ += chosen;
    }

    FuzzByteReader* reader_;
    std::string line_;
};

inline std::string render_admin_line(const AdminCommand& command, FuzzByteReader* reader) {
    AdminLineRenderer renderer(reader);
    auto fault_user = [&](std::string_view what) {
        renderer.keyword("fault");
        renderer.keyword("user");
        renderer.token(command.user);
        renderer.keyword(what);
    };
    switch (command.op) {
        case AdminOp::Help: renderer.keyword("help"); break;
        case AdminOp::Status: renderer.keyword("status"); break;
        case AdminOp::Shutdown: renderer.keyword("shutdown"); break;
        case AdminOp::Phase:
            renderer.keyword("phase");
            renderer.token(command.symbol);
            renderer.keyword(kPhaseNames[static_cast<std::size_t>(command.phase)]);
            break;
        case AdminOp::Book:
            renderer.keyword("book");
            renderer.token(command.symbol);
            if (command.count != 5 || (reader != nullptr && reader->take_bool())) renderer.number(command.count);
            break;
        case AdminOp::FaultMdDrop:
        case AdminOp::FaultMdDup:
            renderer.keyword("fault");
            renderer.keyword("md");
            renderer.keyword(command.op == AdminOp::FaultMdDrop ? "drop" : "dup");
            renderer.number(command.count);
            break;
        case AdminOp::FaultMdReorder:
            renderer.keyword("fault");
            renderer.keyword("md");
            renderer.keyword("reorder");
            renderer.number(command.count);
            if (command.delay != 1 || (reader != nullptr && reader->take_bool())) renderer.number(command.delay);
            break;
        case AdminOp::FaultMdRate: {
            renderer.keyword("fault");
            renderer.keyword("md");
            renderer.keyword("rate");
            const std::array<std::pair<std::string_view, std::uint64_t>, 5> keys = {{
                {"drop", command.rate.drop_ppm},
                {"dup", command.rate.duplicate_ppm},
                {"reorder", command.rate.reorder_ppm},
                {"delay", command.rate.reorder_delay},
                {"seed", command.rate.seed},
            }};
            const sim::PacketFaultConfig defaults{};
            const std::array<std::uint64_t, 5> default_values = {defaults.drop_ppm, defaults.duplicate_ppm,
                                                                 defaults.reorder_ppm, defaults.reorder_delay,
                                                                 defaults.seed};
            const std::size_t first_key = reader != nullptr ? reader->take_index(keys.size()) : 0;
            for (std::size_t offset = 0; offset < keys.size(); ++offset) {
                const std::size_t key_index = (first_key + offset) % keys.size();
                const bool skip_default = reader == nullptr || reader->take_bool();
                if (keys[key_index].second == default_values[key_index] && skip_default) continue;
                renderer.key_value(keys[key_index].first, keys[key_index].second);
            }
            break;
        }
        case AdminOp::FaultMdOff:
            renderer.keyword("fault");
            renderer.keyword("md");
            renderer.keyword("off");
            break;
        case AdminOp::FaultUserReject:
            fault_user("reject");
            renderer.number(command.count);
            break;
        case AdminOp::FaultUserDropOut:
            fault_user("drop_out");
            renderer.number(command.count);
            break;
        case AdminOp::FaultUserDelay:
            fault_user("delay");
            renderer.number(command.delay);
            break;
        case AdminOp::FaultUserDisconnect: fault_user("disconnect"); break;
        case AdminOp::FaultUserOff: fault_user("off"); break;
        case AdminOp::Bots:
            renderer.keyword("bots");
            renderer.keyword(command.on ? "on" : "off");
            break;
    }
    return renderer.finish();
}

inline bool is_name_token(std::string_view text) {
    if (text.empty()) return false;
    for (const char character : text) {
        if (is_admin_whitespace(character)) return false;
    }
    return true;
}

inline void check_accepted_command_ranges(const AdminCommand& command) {
    const AdminCommand defaults{};
    const bool uses_symbol = command.op == AdminOp::Phase || command.op == AdminOp::Book;
    const bool uses_user = command.op >= AdminOp::FaultUserReject && command.op <= AdminOp::FaultUserOff;
    EXSIM_FUZZ_REQUIRE(uses_symbol ? is_name_token(command.symbol) : command.symbol.empty(), "symbol is one token");
    EXSIM_FUZZ_REQUIRE(uses_user ? is_name_token(command.user) : command.user.empty(), "user is one token");
    switch (command.op) {
        case AdminOp::Book:
        case AdminOp::FaultMdDrop:
        case AdminOp::FaultMdDup:
        case AdminOp::FaultUserReject:
        case AdminOp::FaultUserDropOut:
            EXSIM_FUZZ_REQUIRE(command.count > 0, "counts are positive");
            break;
        case AdminOp::FaultMdReorder:
            EXSIM_FUZZ_REQUIRE(command.count > 0 && command.delay > 0, "reorder count and delay are positive");
            break;
        case AdminOp::FaultMdRate:
            EXSIM_FUZZ_REQUIRE(command.rate.drop_ppm <= 1'000'000 && command.rate.duplicate_ppm <= 1'000'000 &&
                                   command.rate.reorder_ppm <= 1'000'000 && command.rate.reorder_delay > 0,
                               "rates are ppm and the reorder delay is positive");
            break;
        default: break;
    }
    if (command.op != AdminOp::FaultMdRate) {
        EXSIM_FUZZ_REQUIRE(command.rate.drop_ppm == defaults.rate.drop_ppm &&
                               command.rate.duplicate_ppm == defaults.rate.duplicate_ppm &&
                               command.rate.reorder_ppm == defaults.rate.reorder_ppm &&
                               command.rate.reorder_delay == defaults.rate.reorder_delay &&
                               command.rate.seed == defaults.rate.seed,
                           "only rate commands carry rates");
    }
}

inline void check_parse_of_line(std::string_view line) {
    AdminCommand sentinel{};
    sentinel.op = AdminOp::Shutdown;
    sentinel.symbol = "SENTINEL-SYMBOL";
    sentinel.user = "SENTINEL-USER";
    sentinel.count = 777;
    sentinel.rate.seed = 99;
    AdminCommand parsed = sentinel;
    std::string error = "previous error";
    const bool accepted = server::parse_admin(line, parsed, error);

    static constexpr std::array<std::string_view, 7> kVerbs = {"help", "status", "shutdown", "phase",
                                                               "book",  "bots",   "fault"};
    const std::string verb = lower_ascii(first_token(line));
    bool known_verb = false;
    for (const std::string_view each_verb : kVerbs) known_verb = known_verb || verb == each_verb;

    if (!accepted) {
        EXSIM_FUZZ_REQUIRE(same_command(parsed, sentinel), "a rejected line leaves the command unchanged");
        EXSIM_FUZZ_REQUIRE(!error.empty() && error != "previous error", "a rejected line sets a new error");
        if (!verb.empty() && !known_verb) {
            EXSIM_FUZZ_REQUIRE(error.rfind("unknown command '", 0) == 0, "an unknown verb is reported as such");
        }
        return;
    }
    EXSIM_FUZZ_REQUIRE(error.empty(), "an accepted line clears the error");
    EXSIM_FUZZ_REQUIRE(known_verb, "only known verbs are accepted");
    check_accepted_command_ranges(parsed);

    const std::string canonical_line = render_admin_line(parsed, nullptr);
    AdminCommand reparsed{};
    EXSIM_FUZZ_REQUIRE(server::parse_admin(canonical_line, reparsed, error), "the canonical line parses");
    EXSIM_FUZZ_REQUIRE(same_command(parsed, reparsed), "the canonical line parses to the same command");
}

inline std::string take_name_token(FuzzByteReader& reader) {
    std::string name;
    for (const char character : reader.take_text(12)) {
        if (!is_admin_whitespace(character)) name += character;
    }
    return name.empty() ? std::string("X") : name;
}

inline std::uint32_t take_positive(FuzzByteReader& reader) {
    const std::uint32_t value = reader.take_u32();
    return value == 0 ? 1 : value;
}

inline AdminCommand take_structured_command(FuzzByteReader& reader) {
    AdminCommand command{};
    command.op = static_cast<AdminOp>(reader.take_index(static_cast<std::size_t>(AdminOp::Shutdown) + 1));
    switch (command.op) {
        case AdminOp::Phase:
            command.symbol = take_name_token(reader);
            command.phase = static_cast<Phase>(reader.take_index(kPhaseNames.size()));
            break;
        case AdminOp::Book:
            command.symbol = take_name_token(reader);
            command.count = take_positive(reader);
            break;
        case AdminOp::FaultMdDrop:
        case AdminOp::FaultMdDup: command.count = take_positive(reader); break;
        case AdminOp::FaultMdReorder:
            command.count = take_positive(reader);
            command.delay = take_positive(reader);
            break;
        case AdminOp::FaultMdRate:
            command.rate.drop_ppm = reader.take_u32() % 1'000'001;
            command.rate.duplicate_ppm = reader.take_u32() % 1'000'001;
            command.rate.reorder_ppm = reader.take_u32() % 1'000'001;
            command.rate.reorder_delay = take_positive(reader);
            command.rate.seed = reader.take_u64();
            break;
        case AdminOp::FaultUserReject:
        case AdminOp::FaultUserDropOut:
            command.user = take_name_token(reader);
            command.count = take_positive(reader);
            break;
        case AdminOp::FaultUserDelay:
            command.user = take_name_token(reader);
            command.delay = reader.take_u32();
            break;
        case AdminOp::FaultUserDisconnect:
        case AdminOp::FaultUserOff: command.user = take_name_token(reader); break;
        case AdminOp::Bots: command.on = reader.take_bool(); break;
        default: break;
    }
    return command;
}

}  // namespace admin_command_fuzz

inline int fuzz_admin_command(const std::uint8_t* data, std::size_t size) {
    using namespace admin_command_fuzz;
    check_parse_of_line(as_text(data, size));

    FuzzByteReader reader(data, size);
    const AdminCommand expected = take_structured_command(reader);
    const std::string rendered = render_admin_line(expected, &reader);
    AdminCommand parsed{};
    std::string error;
    EXSIM_FUZZ_REQUIRE(server::parse_admin(rendered, parsed, error), "a rendered command parses: " + error);
    EXSIM_FUZZ_REQUIRE(same_command(parsed, expected), "a rendered command parses to itself");
    return 0;
}

}  // namespace exsim::fuzz

#endif
