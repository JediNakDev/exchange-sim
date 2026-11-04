// Seeded randomized conversation: a FixClient talks to a FixConnection over a
// faulty client-to-exchange wire that drops frames, repeats earlier frames as
// PossDup resends, delivers raw duplicates (which force a too-low Logout and a
// reconnect) and reconnects at random. A second, reliable session provides
// liquidity so fills happen. After the wire quiesces, the outcome is compared
// with a simple model:
//   1. the engine received exactly the client's accepted sends, once each and
//      in send order (cancel-on-disconnect mass cancels excluded);
//   2. the client's handler saw every stored exchange application message
//      exactly once, at its MsgSeqNum, and nothing else;
//   3. ExecIDs are unique for the day.

#include "tests/exsim/gateway/fix/fix_test_support.hpp"

#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace fix_test;

namespace {

constexpr exsim::Timestamp kSecond = fix::kNanosecondsPerSecond;

struct ModelHandler {
    std::map<std::uint64_t, std::string> received_by_sequence;
    std::uint64_t duplicate_deliveries = 0;
    std::uint64_t session_rejects = 0;

    void record(std::uint64_t sequence, std::string key) {
        if (!received_by_sequence.emplace(sequence, std::move(key)).second) {
            ++duplicate_deliveries;
        }
    }
    void on_logon(const fix::FixInboundMessageInfo&) {}
    void on_execution_report(const fix44::ExecutionReport& report, const fix::FixInboundMessageInfo& info) {
        record(info.sequence, "8:" + std::string(report.execution_id.view()));
    }
    void on_order_cancel_reject(const fix44::OrderCancelReject& reject, const fix::FixInboundMessageInfo& info) {
        record(info.sequence, "9:" + std::string(reject.client_order_id.view()));
    }
    void on_order_mass_cancel_report(const fix44::OrderMassCancelReport& report,
                                     const fix::FixInboundMessageInfo& info) {
        record(info.sequence, "r:" + std::string(report.order_id.view()));
    }
    void on_session_reject(const fix44::Reject&, const fix::FixInboundMessageInfo&) { ++session_rejects; }
    void on_logout(std::string_view) {}
};

struct FaultStatistics {
    std::uint64_t dropped = 0;
    std::uint64_t possdup_repeats = 0;
    std::uint64_t raw_duplicates = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t fills = 0;
    std::uint64_t engine_commands = 0;
    std::uint64_t application_messages = 0;
};

class RandomizedConversation {
public:
    explicit RandomizedConversation(std::uint64_t seed)
        : random_(seed), wire_(exchange_, handler_, client_settings("CLIENT1", "secret1", 5)) {
        exchange_.record_history = true;
        liquidity_.logon(0, false, {});
        wire_.connect(false);
        deliver(false);
    }

    void run(int steps, FaultStatistics& statistics) {
        statistics_ = &statistics;
        for (int step = 0; step < steps; ++step) {
            perform_random_action();
            deliver(true);
            liquidity_.sink().clear();
        }
        quiesce();
        verify();
    }

private:
    std::uint64_t roll(std::uint64_t bound) { return random_() % bound; }

    exsim::Price random_price() { return (14'900 + static_cast<exsim::Price>(roll(201))) * (kDollar / 100); }

    void perform_random_action() {
        auto& client = wire_.client();
        const auto now = exchange_.fix_time();
        const std::uint64_t action = roll(100);
        if (action < 35) {
            const std::string token = "N" + std::to_string(++token_counter_);
            const exsim::Side side = roll(2) == 0 ? exsim::Side::BUY : exsim::Side::SELL;
            const std::string_view symbol = roll(4) == 0 ? "MSFT" : "AAPL";
            if (client.send_new_order_single(make_new_order_single(token, side, static_cast<exsim::Quantity>(1 + roll(200)),
                                                                   random_price(), symbol),
                                             now)) {
                intended_.push_back("D:" + token);
                order_tokens_.push_back({token, side, std::string(symbol)});
            }
        } else if (action < 47 && !order_tokens_.empty()) {
            const OrderToken& target = order_tokens_[roll(order_tokens_.size())];
            const std::string token = "C" + std::to_string(++token_counter_);
            if (client.send_order_cancel_request(make_cancel_request(target.token, token, target.side, target.symbol),
                                                 now)) {
                intended_.push_back("F:" + token);
            }
        } else if (action < 59 && !order_tokens_.empty()) {
            const OrderToken target = order_tokens_[roll(order_tokens_.size())];
            const std::string token = "R" + std::to_string(++token_counter_);
            if (client.send_order_cancel_replace_request(
                    make_replace_request(target.token, token, target.side,
                                         static_cast<exsim::Quantity>(1 + roll(200)), random_price(), target.symbol),
                    now)) {
                intended_.push_back("G:" + token);
                order_tokens_.push_back({token, target.side, target.symbol});
            }
        } else if (action < 62) {
            const std::string token = "M" + std::to_string(++token_counter_);
            if (client.send_order_mass_cancel_request(
                    make_mass_cancel_request(token, roll(2) == 0 ? std::string_view{} : std::string_view("AAPL")),
                    now)) {
                intended_.push_back("q:" + token);
            }
        } else if (action < 72) {
            const std::string token = "L" + std::to_string(++token_counter_);
            const bool buy = roll(2) == 0;
            liquidity_.send("D", new_order_fields(token, buy ? "1" : "2", 1 + roll(300),
                                                  std::to_string(149 + roll(3)), roll(4) == 0 ? "MSFT" : "AAPL"));
        } else if (action < 90) {
            exchange_.advance(roll(3 * kSecond));
            wire_.connection().on_timer(exchange_);
            wire_.client().on_timer(exchange_.fix_time());
        } else if (action < 92) {
            reconnect();
        }
    }

    void reconnect() {
        wire_.disconnect();
        ++statistics_->reconnects;
        wire_.connect(false);
    }

    void feed_exchange(std::string_view frame) {
        wire_.connection().on_data({frame.data(), frame.size()}, exchange_);
    }

    // Delivers queued bytes both ways until quiet (bounded), injecting faults
    // on the client-to-exchange direction when asked.
    void deliver(bool inject_faults) {
        for (int round = 0; round < 64; ++round) {
            bool delivered = false;
            const std::string queued(wire_.client_to_exchange().bytes());
            wire_.client_to_exchange().clear();
            std::size_t position = 0;
            while (position < queued.size() && !wire_.connection().wants_disconnect()) {
                std::size_t frame_length = 0;
                REQUIRE(fix44::frame_size({queued.data() + position, queued.size() - position}, frame_length) ==
                        exsim::protocol::Status::ok);
                const std::string_view frame(queued.data() + position, frame_length);
                position += frame_length;
                delivered = true;
                const std::uint64_t fault = inject_faults ? roll(100) : 100;
                if (fault < 5) {
                    ++statistics_->dropped;
                    continue;
                }
                feed_exchange(frame);
                if (fault < 11) {
                    const std::uint64_t last_sent = wire_.client().session_day_state().next_outbound_seq() - 1;
                    const std::uint64_t repeated_sequence = 1 + roll(last_sent);
                    wire_.client().session_day_state().resend(repeated_sequence, repeated_sequence,
                                                              exchange_.fix_time());
                    ++statistics_->possdup_repeats;
                } else if (fault < 12) {
                    feed_exchange(frame);
                    ++statistics_->raw_duplicates;
                }
            }
            const std::string_view to_client = wire_.exchange_to_client().bytes();
            if (!to_client.empty()) {
                const std::size_t consumed =
                    wire_.client().on_data({to_client.data(), to_client.size()}, exchange_.fix_time());
                wire_.exchange_to_client().consume(consumed);
                delivered = delivered || consumed != 0;
            }
            if (wire_.wants_disconnect()) {
                reconnect();
                delivered = true;
            }
            if (!delivered) {
                return;
            }
        }
    }

    bool settled() {
        auto& client_day = wire_.client().session_day_state();
        auto& exchange_day = exchange_.user(1).session_day_state();
        return wire_.client().active() && wire_.connection().phase() == fix::FixConnectionPhase::Active &&
               client_day.expected_inbound_seq() == exchange_day.next_outbound_seq() &&
               exchange_day.expected_inbound_seq() == client_day.next_outbound_seq() &&
               !wire_.client().resend_request_outstanding() && !wire_.connection().resend_request_outstanding() &&
               wire_.client().held_message_count() == 0 && wire_.connection().held_message_count() == 0;
    }

    void quiesce() {
        for (int second = 0; second < 600 && !settled(); ++second) {
            exchange_.advance(kSecond);
            wire_.connection().on_timer(exchange_);
            wire_.client().on_timer(exchange_.fix_time());
            deliver(false);
        }
        REQUIRE(settled());
    }

    void verify() {
        CHECK(engine_command_tokens(exchange_, 1) == intended_);
        statistics_->engine_commands += intended_.size();
        CHECK_EQ(handler_.duplicate_deliveries, 0u);

        const auto& store = exchange_.user(1).session_day_state().store();
        std::map<std::uint64_t, std::string> stored_application_messages;
        std::set<std::string> execution_ids;
        for (std::uint64_t sequence = 1; sequence <= store.last_seq(); ++sequence) {
            const auto stored = store.get(sequence);
            if (stored.bytes.empty()) {
                continue;
            }
            const auto parsed = split_frames({stored.bytes.data(), stored.bytes.size()});
            REQUIRE_EQ(parsed.size(), std::size_t{1});
            const ParsedFixMessage& message = parsed[0];
            CHECK_EQ(message.sequence(), sequence);
            std::string key;
            if (message.type() == "8") {
                key = "8:" + message.get(17);
                CHECK(execution_ids.insert(message.get(17)).second);
                if (message.get(150) == "F") ++statistics_->fills;
            } else if (message.type() == "9") {
                key = "9:" + message.get(11);
            } else {
                key = "r:" + message.get(37);
                CHECK(execution_ids.insert(message.get(37)).second);
            }
            stored_application_messages.emplace(sequence, key);
        }
        CHECK_EQ(exchange_.user(1).session_day_state().messages_not_stored(), 0u);
        CHECK(handler_.received_by_sequence == stored_application_messages);
        statistics_->application_messages += stored_application_messages.size();
        CHECK_EQ(handler_.received_by_sequence.size(), stored_application_messages.size());
    }

    struct OrderToken {
        std::string token;
        exsim::Side side;
        std::string symbol;
    };

    std::mt19937_64 random_;
    FixTestExchange exchange_;
    ModelHandler handler_;
    FixClientWire<ModelHandler> wire_;
    FixTestConnection liquidity_{exchange_, "CLIENT2"};
    std::vector<std::string> intended_;
    std::vector<OrderToken> order_tokens_;
    std::uint64_t token_counter_ = 0;
    FaultStatistics construction_statistics_;
    FaultStatistics* statistics_ = &construction_statistics_;
};

}  // namespace

TEST_CASE(gateway_fix_randomized, faulty_wire_conversation_matches_model) {
    FaultStatistics statistics;
    for (std::uint64_t seed = 1; seed <= 24; ++seed) {
        RandomizedConversation conversation(seed * 0x9E3779B97F4A7C15ull);
        conversation.run(400, statistics);
    }
    // The faults were exercised.
    CHECK_GT(statistics.dropped, 100u);
    CHECK_GT(statistics.possdup_repeats, 100u);
    CHECK_GT(statistics.raw_duplicates, 10u);
    CHECK_GT(statistics.reconnects, 50u);
    CHECK_GT(statistics.fills, 100u);
    CHECK_GT(statistics.engine_commands, 3000u);
    CHECK_GT(statistics.application_messages, 5000u);
}
