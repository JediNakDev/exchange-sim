// exsim_client: a small OUCH 4.2 order entry client for a running exsim
// server. Logs in, sends orders given as arguments or read from a script,
// prints every acknowledgement, execution and reject, and logs out.
//
//   exsim_client --user NAME --password PASSWORD [--server 127.0.0.1:31000]
//                [--sequence N] [--reply-timeout-ms N] [--linger-ms N]
//                [--script PATH] [COMMAND ...]
//
// A command is one argument (quote it) or one script line ('#' starts a comment):
//
//   buy TOKEN SYMBOL SHARES PRICE [ioc]      enter a limit order (price 150.25)
//   sell TOKEN SYMBOL SHARES PRICE [ioc]
//   replace TOKEN NEW_TOKEN SHARES PRICE     replace an order
//   cancel TOKEN [REMAINING_SHARES]          cancel (default: all of it)
//   wait MILLISECONDS                        keep printing messages for a while
//
// After each order command the client waits (up to --reply-timeout-ms) for
// the exchange's answer about that token before sending the next one; after
// the last command it keeps listening for --linger-ms, then logs out.
// Exit status 0 when every command was answered, 1 on bad arguments or a
// failed login, 2 when a command went unanswered or the connection dropped.

#include "exsim/client/exchange_client_sessions.hpp"
#include "exsim/gateway/ouch/ouch_client.hpp"
#include "exsim/net/socket.hpp"
#include "exsim/server/config.hpp"

#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace exsim;
namespace ouch42 = exsim::protocol::ouch42;

struct ClientOptions {
    net::Ipv4Endpoint server = net::loopback_endpoint(31000);
    std::string username;
    std::string password;
    std::uint64_t requested_sequence = 0;
    int reply_timeout_ms = 2000;
    int linger_ms = 200;
    std::string script_path;
    std::vector<std::string> commands;
};

void print_usage() {
    std::fprintf(stderr,
                 "usage: exsim_client --user NAME --password PASSWORD [--server HOST:PORT] [--sequence N]\n"
                 "                    [--reply-timeout-ms N] [--linger-ms N] [--script PATH] [COMMAND ...]\n"
                 "commands: buy|sell TOKEN SYMBOL SHARES PRICE [ioc]; replace TOKEN NEW_TOKEN SHARES PRICE;\n"
                 "          cancel TOKEN [REMAINING_SHARES]; wait MILLISECONDS\n");
}

bool parse_unsigned(std::string_view text, std::uint64_t maximum, std::uint64_t& value_out) {
    if (text.empty() || text.size() > 19) return false;
    std::uint64_t value = 0;
    for (char digit : text) {
        if (digit < '0' || digit > '9') return false;
        value = value * 10 + std::uint64_t(digit - '0');
    }
    if (value > maximum) return false;
    value_out = value;
    return true;
}

bool parse_endpoint(std::string_view text, net::Ipv4Endpoint& endpoint_out) {
    const std::size_t colon = text.rfind(':');
    std::uint64_t port = 0;
    if (colon == std::string_view::npos || !parse_unsigned(text.substr(colon + 1), 65535, port) || port == 0) {
        return false;
    }
    if (!net::parse_ipv4_address(text.substr(0, colon), endpoint_out.address)) return false;
    endpoint_out.port = static_cast<std::uint16_t>(port);
    return true;
}

bool parse_arguments(int argc, char** argv, ClientOptions& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument.rfind("--", 0) != 0) {
            options.commands.emplace_back(argument);
            continue;
        }
        if (index + 1 >= argc) return false;
        const std::string_view value = argv[++index];
        std::uint64_t number = 0;
        if (argument == "--server") {
            if (!parse_endpoint(value, options.server)) return false;
        } else if (argument == "--user") {
            options.username = value;
        } else if (argument == "--password") {
            options.password = value;
        } else if (argument == "--sequence") {
            if (!parse_unsigned(value, UINT64_MAX, options.requested_sequence)) return false;
        } else if (argument == "--reply-timeout-ms") {
            if (!parse_unsigned(value, 600'000, number)) return false;
            options.reply_timeout_ms = static_cast<int>(number);
        } else if (argument == "--linger-ms") {
            if (!parse_unsigned(value, 600'000, number)) return false;
            options.linger_ms = static_cast<int>(number);
        } else if (argument == "--script") {
            options.script_path = value;
        } else {
            return false;
        }
    }
    return !options.username.empty() && !options.password.empty();
}

std::vector<std::string_view> split_words(std::string_view line) {
    std::vector<std::string_view> words;
    std::size_t position = 0;
    while (position < line.size()) {
        while (position < line.size() && (line[position] == ' ' || line[position] == '\t')) ++position;
        const std::size_t start = position;
        while (position < line.size() && line[position] != ' ' && line[position] != '\t') ++position;
        if (position > start) words.push_back(line.substr(start, position - start));
    }
    return words;
}

std::string token_text(const std::array<char, 14>& token) {
    std::string text(token.data(), token.size());
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

std::array<char, 14> make_token(std::string_view text) {
    std::array<char, 14> token;
    token.fill(' ');
    for (std::size_t index = 0; index < text.size() && index < token.size(); ++index) token[index] = text[index];
    return token;
}

std::string symbol_text(const Symbol& symbol) {
    std::string text(symbol.view());
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

// Prints everything the exchange sends and remembers which tokens it has
// answered about (the token the client is currently waiting for).
struct PrintingHandler {
    std::string awaited_token;
    bool awaited_token_answered = false;
    bool login_accepted = false;
    bool login_rejected = false;
    bool end_of_session = false;

    void answered(const std::string& token) {
        if (token == awaited_token) awaited_token_answered = true;
    }

    void on_login_accepted(const protocol::soupbintcp::LoginAccepted& accepted) {
        login_accepted = true;
        std::printf("login accepted session=%.*s next_sequence=%" PRIu64 "\n", int(accepted.session.size()),
                    accepted.session.data(), accepted.sequence);
    }
    void on_login_rejected(char reason) {
        login_rejected = true;
        std::printf("login rejected reason=%c\n", reason);
    }
    void on_end_of_session() {
        end_of_session = true;
        std::printf("end of session\n");
    }
    void on_server_timeout() { std::printf("server timed out\n"); }
    void on_system_event(const ouch42::SystemEvent& message, std::uint64_t sequence) {
        std::printf("[%" PRIu64 "] system event %c\n", sequence, message.event);
    }
    void on_order_accepted(const ouch42::OrderAccepted& message, std::uint64_t sequence) {
        const std::string token = token_text(message.token);
        std::printf("[%" PRIu64 "] accepted %s %c %s %u @ %s ref=%" PRIu64 "\n", sequence, token.c_str(),
                    message.side, symbol_text(message.stock).c_str(), unsigned(message.shares),
                    server::format_price(message.price).c_str(), message.order_reference);
        answered(token);
    }
    void on_order_replaced(const ouch42::OrderReplaced& message, std::uint64_t sequence) {
        const std::string token = token_text(message.replacement_token);
        std::printf("[%" PRIu64 "] replaced %s -> %s %u @ %s\n", sequence, token_text(message.previous_token).c_str(),
                    token.c_str(), unsigned(message.shares), server::format_price(message.price).c_str());
        answered(token);
    }
    void on_order_canceled(const ouch42::OrderCanceled& message, std::uint64_t sequence) {
        const std::string token = token_text(message.token);
        std::printf("[%" PRIu64 "] canceled %s %u shares reason=%c\n", sequence, token.c_str(),
                    unsigned(message.shares), message.reason);
        answered(token);
    }
    void on_order_executed(const ouch42::OrderExecuted& message, std::uint64_t sequence) {
        const std::string token = token_text(message.token);
        std::printf("[%" PRIu64 "] executed %s %u @ %s liquidity=%c match=%" PRIu64 "\n", sequence, token.c_str(),
                    unsigned(message.shares), server::format_price(message.price).c_str(), message.liquidity,
                    message.match_number);
    }
    void on_order_rejected(const ouch42::OrderRejected& message, std::uint64_t sequence) {
        const std::string token = token_text(message.token);
        std::printf("[%" PRIu64 "] rejected %s reason=%c\n", sequence, token.c_str(), message.reason);
        answered(token);
    }
    void on_cancel_reject(const ouch42::CancelReject& message, std::uint64_t sequence) {
        const std::string token = token_text(message.token);
        std::printf("[%" PRIu64 "] cancel rejected %s\n", sequence, token.c_str());
        answered(token);
    }
    void on_cancel_pending(const ouch42::CancelPending& message, std::uint64_t sequence) {
        std::printf("[%" PRIu64 "] cancel pending %s\n", sequence, token_text(message.token).c_str());
    }
};

using Session = client::OuchTcpSession<PrintingHandler>;

// Pumps the session for up to timeout_ms or until `done` holds.
template <class Done>
bool pump_until(Session& session, int timeout_ms, Done&& done) {
    const std::uint64_t deadline = net::monotonic_ns() + std::uint64_t(timeout_ms) * 1'000'000;
    for (;;) {
        if (!session.poll()) return done();
        if (done()) return true;
        if (net::monotonic_ns() >= deadline) return false;
        const int descriptor = session.fd();
        client::wait_for_input(std::span<const int>(&descriptor, 1), 5);
    }
}

enum class CommandOutcome : std::uint8_t { Answered, NotAnswered, BadCommand, ConnectionLost };

CommandOutcome run_command(std::string_view line, Session& session, PrintingHandler& handler,
                           const ClientOptions& options) {
    const std::vector<std::string_view> words = split_words(line);
    if (words.empty()) return CommandOutcome::Answered;
    const std::string_view verb = words[0];
    std::uint64_t number = 0;
    bool sent = false;
    if ((verb == "buy" || verb == "sell") && (words.size() == 5 || (words.size() == 6 && words[5] == "ioc"))) {
        ouch42::EnterOrder order;
        Price price = 0;
        if (words[1].size() > 14 || words[2].size() > 8 || !parse_unsigned(words[3], 1'000'000'000, number) ||
            !server::parse_price(words[4], price)) {
            return CommandOutcome::BadCommand;
        }
        order.token = make_token(words[1]);
        order.side = verb == "buy" ? 'B' : 'S';
        order.shares = static_cast<Quantity>(number);
        order.stock = Symbol(words[2]);
        order.price = price;
        order.time_in_force = words.size() == 6 ? 0 : 99999;  // 0 = immediate or cancel, 99999 = day
        order.display = 'Y';
        order.capacity = 'A';
        order.ise = 'N';
        order.minimum_quantity = 0;
        order.cross_type = 'N';
        order.customer_type = 'R';
        handler.awaited_token = std::string(words[1]);
        sent = session.client().enter_order(order, net::monotonic_ns());
    } else if (verb == "replace" && words.size() == 5) {
        ouch42::ReplaceOrder replace;
        Price price = 0;
        if (words[1].size() > 14 || words[2].size() > 14 || !parse_unsigned(words[3], 1'000'000'000, number) ||
            !server::parse_price(words[4], price)) {
            return CommandOutcome::BadCommand;
        }
        replace.existing_token = make_token(words[1]);
        replace.replacement_token = make_token(words[2]);
        replace.shares = static_cast<Quantity>(number);
        replace.price = price;
        replace.time_in_force = 99999;
        replace.display = 'Y';
        replace.ise = 'N';
        replace.minimum_quantity = 0;
        handler.awaited_token = std::string(words[2]);
        sent = session.client().replace_order(replace, net::monotonic_ns());
    } else if (verb == "cancel" && (words.size() == 2 || words.size() == 3)) {
        ouch42::CancelOrder cancel;
        if (words[1].size() > 14 || (words.size() == 3 && !parse_unsigned(words[2], 1'000'000'000, number))) {
            return CommandOutcome::BadCommand;
        }
        cancel.token = make_token(words[1]);
        cancel.shares = static_cast<Quantity>(number);
        handler.awaited_token = std::string(words[1]);
        sent = session.client().cancel_order(cancel, net::monotonic_ns());
    } else if (verb == "wait" && words.size() == 2 && parse_unsigned(words[1], 3'600'000, number)) {
        pump_until(session, static_cast<int>(number), [] { return false; });
        return session.connected() ? CommandOutcome::Answered : CommandOutcome::ConnectionLost;
    } else {
        return CommandOutcome::BadCommand;
    }
    if (!sent || !session.flush()) return CommandOutcome::ConnectionLost;
    handler.awaited_token_answered = false;
    const bool answered =
        pump_until(session, options.reply_timeout_ms, [&] { return handler.awaited_token_answered; });
    if (answered) return CommandOutcome::Answered;
    return session.connected() ? CommandOutcome::NotAnswered : CommandOutcome::ConnectionLost;
}

}  // namespace

int main(int argc, char** argv) {
    ClientOptions options;
    if (!parse_arguments(argc, argv, options)) {
        print_usage();
        return 1;
    }
    if (!options.script_path.empty()) {
        std::ifstream script(options.script_path);
        if (!script) {
            std::fprintf(stderr, "exsim_client: cannot read %s\n", options.script_path.c_str());
            return 1;
        }
        for (std::string line; std::getline(script, line);) {
            const std::size_t comment = line.find('#');
            if (comment != std::string::npos) line.resize(comment);
            if (!split_words(line).empty()) options.commands.push_back(line);
        }
    }

    PrintingHandler handler;
    Session session(handler);
    if (!session.connect_and_login(options.server, {options.username, options.password, ""},
                                   options.requested_sequence)) {
        std::fprintf(stderr, "exsim_client: cannot connect to the server\n");
        return 1;
    }
    pump_until(session, options.reply_timeout_ms, [&] { return handler.login_accepted || handler.login_rejected; });
    if (!handler.login_accepted) {
        if (!handler.login_rejected) std::fprintf(stderr, "exsim_client: no login response\n");
        return 1;
    }

    int exit_status = 0;
    for (const std::string& command : options.commands) {
        const CommandOutcome outcome = run_command(command, session, handler, options);
        if (outcome == CommandOutcome::BadCommand) {
            std::fprintf(stderr, "exsim_client: bad command: %s\n", command.c_str());
            print_usage();
            exit_status = 1;
            break;
        }
        if (outcome == CommandOutcome::NotAnswered) {
            std::printf("no answer for: %s\n", command.c_str());
            exit_status = 2;
        }
        if (outcome == CommandOutcome::ConnectionLost) {
            std::printf("connection lost\n");
            return 2;
        }
    }
    pump_until(session, options.linger_ms, [] { return false; });
    if (session.connected() && session.client().logout(net::monotonic_ns())) {
        session.flush();
        // The server closes the connection after a Logout Request.
        pump_until(session, options.reply_timeout_ms, [&] { return !session.connected(); });
        std::printf("logged out\n");
    }
    session.disconnect();
    return exit_status;
}
