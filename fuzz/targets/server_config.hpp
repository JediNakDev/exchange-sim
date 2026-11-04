#ifndef EXSIM_FUZZ_TARGETS_SERVER_CONFIG_HPP
#define EXSIM_FUZZ_TARGETS_SERVER_CONFIG_HPP

// exsim.conf parser (server::parse_config). The whole input is the file.
//
//  * Rejected: `out` is unchanged and the error is "<line>: <message>" with a
//    line number inside the file (0 = whole-file validation).
//  * Accepted: every documented validation holds (instruments valid for the
//    engine, unique symbols, unique users and non-zero unique session ids,
//    SoupBinTCP length limits, session names), the engine section is one the
//    engine accepts, and the canonical rendering of the result parses back to
//    a configuration that renders identically.

#include "fuzz_support.hpp"

#include <exsim/matching_engine.hpp>
#include <exsim/md/feed_common.hpp>
#include <exsim/server/config.hpp>
#include <limits>
#include <set>
#include <string>

namespace exsim::fuzz {

namespace server_config_fuzz {

using server::Protocol;
using server::ServerConfig;

inline std::string_view phase_name(Phase phase) {
    switch (phase) {
        case Phase::Closed: return "closed";
        case Phase::PreOpen: return "pre_open";
        case Phase::Continuous: return "continuous";
        case Phase::Halted: return "halted";
        case Phase::PreClose: return "pre_close";
    }
    return "invalid";
}

inline std::string_view stp_name(StpMode mode) {
    switch (mode) {
        case StpMode::None: return "none";
        case StpMode::CancelResting: return "cancel_resting";
        case StpMode::CancelAggressor: return "cancel_aggressor";
        case StpMode::CancelBoth: return "cancel_both";
    }
    return "invalid";
}

// Every field, in parse order, so the rendering is a complete fingerprint.
inline std::string render_config(const ServerConfig& config) {
    std::string text;
    auto line = [&](std::string_view key, std::string_view value) {
        text.append(key).append(" = ").append(value).append("\n");
    };
    auto number = [&](std::string_view key, std::uint64_t value) { line(key, std::to_string(value)); };
    auto boolean = [&](std::string_view key, bool value) { line(key, value ? "true" : "false"); };

    text += "[exchange]\n";
    line("name", config.exchange.name);
    line("bind_address", config.exchange.bind_address);
    number("ouch_port", config.exchange.ouch_port);
    number("fix_port", config.exchange.fix_port);
    number("admin_port", config.exchange.admin_port);
    number("max_connections", config.exchange.max_connections);
    number("heartbeat_interval_ms", config.exchange.heartbeat_interval_ms);
    line("soup_session", config.exchange.soup_session);

    text += "[market_data]\n";
    line("address", config.market_data.address);
    number("port", config.market_data.port);
    line("interface", config.market_data.interface_address);
    number("ttl", config.market_data.ttl);
    boolean("loopback", config.market_data.loopback);
    number("retransmit_port", config.market_data.retransmit_port);
    number("max_payload", config.market_data.max_payload);
    line("session", config.market_data.session);
    number("retransmit_buffer", config.market_data.retransmit_buffer);
    number("noii_interval_ms", config.market_data.noii_interval_ms);

    text += "[engine]\n";
    number("max_instruments", config.engine.engine.max_instruments);
    number("max_orders", config.engine.engine.max_orders);
    number("max_tokens_per_session", config.engine.max_tokens_per_session);
    number("store_messages_per_session", config.engine.store_messages_per_session);
    number("store_bytes_per_session", config.engine.store_bytes_per_session);

    text += "[journal]\n";
    line("path", config.journal_path);

    for (const InstrumentConfig& instrument : config.instruments) {
        text.append("[instrument ").append(instrument.symbol.view()).append("]\n");
        line("tick_size", server::format_price(instrument.tick_size));
        number("lot_size", instrument.lot_size);
        number("max_order_qty", instrument.max_order_qty);
        line("min_price", server::format_price(instrument.min_price));
        line("max_price", server::format_price(instrument.max_price));
        line("reference_price", server::format_price(instrument.reference_price));
        line("initial_phase", phase_name(instrument.initial_phase));
    }
    for (const server::UserConfig& user : config.users) {
        text.append("[user ").append(user.username).append("]\n");
        line("password", user.password);
        line("protocol", server::to_string(user.protocol));
        number("session_id", user.session_id);
        number("owner", user.owner);
        line("stp", stp_name(user.stp));
        boolean("cancel_on_disconnect", user.cancel_on_disconnect);
    }
    return text;
}

inline bool printable_without_space(std::string_view text) {
    for (const char character : text) {
        if (character < 0x21 || character > 0x7e) return false;
    }
    return true;
}

inline void check_accepted_config(const ServerConfig& config) {
    EXSIM_FUZZ_REQUIRE(config.exchange.soup_session.size() <= 10 &&
                           printable_without_space(config.exchange.soup_session),
                       "SoupBinTCP session is at most 10 printable characters");
    EXSIM_FUZZ_REQUIRE(config.market_data.session.size() <= 10 && printable_without_space(config.market_data.session),
                       "MoldUDP64 session is at most 10 printable characters");
    EXSIM_FUZZ_REQUIRE(!config.exchange.name.empty() && !config.market_data.address.empty(),
                       "name and address are required");
    EXSIM_FUZZ_REQUIRE(config.market_data.max_payload >= exsim::md::kMinimumMaxPayload &&
                           config.market_data.max_payload <= exsim::md::kMaximumMaxPayload,
                       "max_payload fits one UDP datagram");
    EXSIM_FUZZ_REQUIRE(!config.instruments.empty() && config.instruments.size() <= config.engine.engine.max_instruments,
                       "1..max_instruments instruments");
    {
        bool engine_accepts = true;
        try {
            (void)required_event_capacity(config.engine.engine);
        } catch (const std::exception&) {
            engine_accepts = false;
        }
        EXSIM_FUZZ_REQUIRE(engine_accepts, "an accepted [engine] section is accepted by the engine");
    }
    std::set<std::string> symbols;
    for (const InstrumentConfig& instrument : config.instruments) {
        EXSIM_FUZZ_REQUIRE(valid_instrument_config(instrument), "every accepted instrument is valid for the engine");
        EXSIM_FUZZ_REQUIRE(!instrument.symbol.view().empty() && instrument.symbol.view().size() <= 8 &&
                               printable_without_space(instrument.symbol.view()),
                           "symbols are 1-8 printable characters");
        EXSIM_FUZZ_REQUIRE(symbols.insert(std::string(instrument.symbol.view())).second, "symbols are unique");
    }
    std::set<std::string> user_names;
    std::set<SessionId> session_ids;
    for (const server::UserConfig& user : config.users) {
        EXSIM_FUZZ_REQUIRE(!user.username.empty() && printable_without_space(user.username), "user names are printable");
        EXSIM_FUZZ_REQUIRE(user_names.insert(user.username).second, "user names are unique");
        EXSIM_FUZZ_REQUIRE(user.session_id != 0 && session_ids.insert(user.session_id).second,
                           "session ids are non-zero and unique");
        if (user.protocol == Protocol::Ouch42) {
            EXSIM_FUZZ_REQUIRE(user.username.size() <= 6 && user.password.size() <= 10,
                               "SoupBinTCP username and password limits");
        }
        EXSIM_FUZZ_REQUIRE(config.find_user(user.username) == &user, "find_user finds the first and only match");
    }
}

inline void check_error_format(std::string_view error, std::string_view text) {
    std::size_t digit_count = 0;
    std::uint64_t line_number = 0;
    while (digit_count < error.size() && error[digit_count] >= '0' && error[digit_count] <= '9' && digit_count < 12) {
        line_number = line_number * 10 + static_cast<std::uint64_t>(error[digit_count] - '0');
        ++digit_count;
    }
    EXSIM_FUZZ_REQUIRE(digit_count > 0 && error.substr(digit_count, 2) == ": " && error.size() > digit_count + 2,
                       "errors are '<line>: <message>'");
    std::uint64_t line_count = 0;
    for (std::size_t position = 0; position < text.size(); ++position) {
        if (text[position] == '\n' || position + 1 == text.size()) ++line_count;
    }
    EXSIM_FUZZ_REQUIRE(line_number <= line_count, "the error line is inside the file");
}

}  // namespace server_config_fuzz

inline int fuzz_server_config(const std::uint8_t* data, std::size_t size) {
    using namespace server_config_fuzz;
    const std::string_view text = as_text(data, size);

    ServerConfig parsed{};
    parsed.journal_path = "sentinel: must survive a rejected parse";
    parsed.exchange.name = "SENTINEL";
    std::string error;
    if (!server::parse_config(text, parsed, error)) {
        EXSIM_FUZZ_REQUIRE(parsed.journal_path == "sentinel: must survive a rejected parse" &&
                               parsed.exchange.name == "SENTINEL" && parsed.instruments.empty() &&
                               parsed.users.empty(),
                           "a rejected file leaves the configuration unchanged");
        check_error_format(error, text);
        return 0;
    }
    check_accepted_config(parsed);

    const std::string canonical_text = render_config(parsed);
    ServerConfig reparsed{};
    EXSIM_FUZZ_REQUIRE(server::parse_config(canonical_text, reparsed, error), "the canonical file parses: " + error);
    EXSIM_FUZZ_REQUIRE(render_config(reparsed) == canonical_text, "the canonical file parses to the same configuration");
    return 0;
}

}  // namespace exsim::fuzz

#endif
