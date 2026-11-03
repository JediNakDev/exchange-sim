// Seeded randomized OUCH conversation between two OuchClients and the
// gateway, checked message by message against a small reference model: an
// independent, deliberately naive price-time book plus the gateway rules
// (token reuse, gateway rejects, X/U semantics, cancel-on-disconnect) and
// SoupBinTCP recovery across random disconnects.

#include "ouch_test_harness.hpp"

#include <algorithm>
#include <map>
#include <random>

using namespace exsim;
using namespace exsim::gateway::ouch;
using namespace exsim::gateway::ouch::testing;

namespace {

constexpr Price kHundredDollars = 1'000'000;
constexpr Price kTick = 100;
constexpr Price kMinimumPrice = 100;
constexpr Price kMaximumPrice = 2'000'000;
constexpr Quantity kMaximumQuantity = 1'000'000;
constexpr std::uint32_t kMarketPrice = 0x7FFFFFFF;

struct ExpectedMessage {
    char type = 0;
    Timestamp timestamp = 0;
    std::string token;
    Quantity shares = 0;
    Price price = 0;
    char reason_or_liquidity = 0;  // J/C reason, E liquidity, S event
    std::uint64_t reference = 0;   // A/U order reference, E match number
    std::string previous_token;    // U only
};

struct ModelOrder {
    int user = 0;
    std::string stock;
    std::string token;
    std::uint64_t order_id = 0;
    char side = 'B';
    bool is_market = false;
    Price price = 0;
    Quantity leaves = 0;
    Quantity cum = 0;
    bool live = false;
};

struct ModelTokenInstructions {
    bool day = true;
    bool post_only = false;
};

class ReferenceModel {
public:
    std::vector<ExpectedMessage> expected[2];

    void login(int user, Timestamp now) {
        if (start_of_day_sent_[user]) return;
        start_of_day_sent_[user] = true;
        expected[user].push_back({'S', now, "", 0, 0, 'S', 0, ""});
    }

    void enter_order(int user, const std::string& token, char side, Quantity shares, const std::string& stock,
                     std::uint32_t price, std::uint32_t time_in_force, char display, Timestamp now) {
        if (used_tokens_[user].count(token) != 0) return;  // duplicate: silently ignored
        used_tokens_[user][token] = {};
        if (stock != "AAPL" && stock != "MSFT") return reject(user, token, 'S', now);
        const bool supported_time_in_force = time_in_force == 0 || time_in_force == 99998 || time_in_force == 99999;
        if (!supported_time_in_force || (display != 'Y' && display != 'P')) return reject(user, token, 'O', now);
        const bool day = time_in_force != 0;
        const bool post_only = display == 'P';
        used_tokens_[user][token] = {day, post_only};
        const bool is_market = price == kMarketPrice;
        const char engine_side = side == 'B' ? 'B' : 'S';
        if (post_only && (is_market || !day)) return reject(user, token, 'O', now);
        if (!valid_quantity(shares)) return reject(user, token, 'Z', now);
        if (!is_market && !valid_price(static_cast<Price>(price))) return reject(user, token, 'X', now);
        if (post_only && would_cross(stock, engine_side, static_cast<Price>(price))) return reject(user, token, 'O', now);

        ModelOrder order;
        order.user = user;
        order.stock = stock;
        order.token = token;
        order.order_id = ++last_order_id_;
        order.side = engine_side;
        order.is_market = is_market;
        order.price = static_cast<Price>(price);
        order.leaves = shares;
        expected[user].push_back({'A', now, token, shares, static_cast<Price>(price), 0, order.order_id, ""});
        orders_.push_back(order);
        const std::size_t index = orders_.size() - 1;
        match(index, now);
        if (orders_[index].leaves > 0) {
            if (is_market || !day) {
                expected[user].push_back({'C', now, token, orders_[index].leaves, 0, 'I', 0, ""});
            } else {
                orders_[index].live = true;
            }
        }
    }

    void replace_order(int user, const std::string& existing, const std::string& replacement, Quantity shares,
                       std::uint32_t price, std::uint32_t time_in_force, char display, Timestamp now) {
        if (used_tokens_[user].count(replacement) != 0) return;
        used_tokens_[user][replacement] = {};
        const auto existing_instructions = used_tokens_[user].find(existing);
        if (existing_instructions == used_tokens_[user].end()) return;
        const ModelTokenInstructions instructions = existing_instructions->second;
        const bool supported_time_in_force = time_in_force == 0 || time_in_force == 99998 || time_in_force == 99999;
        if (!supported_time_in_force || (display != 'Y' && display != 'P') || (time_in_force != 0) != instructions.day ||
            (display == 'P') != instructions.post_only) {
            return reject(user, replacement, 'O', now);
        }
        used_tokens_[user][replacement] = instructions;
        ModelOrder* order = find_live(user, existing);
        if (order == nullptr) return;  // engine UnknownOrder: OUCH sends nothing
        if (!valid_quantity(shares)) return reject(user, replacement, 'Z', now);
        if (!valid_price(static_cast<Price>(price))) return reject(user, replacement, 'X', now);
        if (instructions.post_only && would_cross(order->stock, order->side, static_cast<Price>(price))) {
            return reject(user, replacement, 'O', now);
        }
        if (shares <= order->cum) {
            expected[user].push_back({'C', now, existing, order->leaves, 0, 'U', 0, ""});
            order->live = false;
            return;
        }
        order->order_id = ++last_order_id_;
        order->token = replacement;
        order->price = static_cast<Price>(price);
        order->leaves = shares - order->cum;
        order->live = false;  // out of the book while it aggresses
        expected[user].push_back({'U', now, replacement, order->leaves, order->price, 0, order->order_id, existing});
        const std::size_t index = static_cast<std::size_t>(order - orders_.data());
        match(index, now);
        if (orders_[index].leaves > 0) orders_[index].live = true;
    }

    void cancel_order(int user, const std::string& token, Quantity shares, Timestamp now) {
        if (used_tokens_[user].count(token) == 0) return;
        ModelOrder* order = find_live(user, token);
        if (order == nullptr) return;
        Quantity target = 0;
        if (shares != 0) target = shares > order->cum ? shares - order->cum : 0;
        if (shares != 0 && target >= order->leaves) return;  // no-op
        expected[user].push_back({'C', now, token, order->leaves - target, 0, 'U', 0, ""});
        order->leaves = target;
        if (target == 0) order->live = false;
    }

    void modify_order(int user, const std::string& token, Timestamp now) { reject(user, token, 'O', now); }

    void disconnect(int user, bool cancel_on_disconnect, Timestamp now) {
        if (!cancel_on_disconnect) return;
        std::vector<ModelOrder*> live_orders;
        for (ModelOrder& order : orders_) {
            if (order.live && order.user == user) live_orders.push_back(&order);
        }
        std::sort(live_orders.begin(), live_orders.end(),
                  [](const ModelOrder* a, const ModelOrder* b) { return a->order_id < b->order_id; });
        for (ModelOrder* order : live_orders) {
            expected[user].push_back({'C', now, order->token, order->leaves, 0, 'U', 0, ""});
            order->live = false;
        }
    }

    [[nodiscard]] std::vector<const ModelOrder*> live_orders() const {
        std::vector<const ModelOrder*> result;
        for (const ModelOrder& order : orders_) {
            if (order.live) result.push_back(&order);
        }
        return result;
    }

    [[nodiscard]] const std::map<std::string, ModelTokenInstructions>& used_tokens(int user) const {
        return used_tokens_[user];
    }

private:
    static bool valid_quantity(Quantity shares) { return shares > 0 && shares <= kMaximumQuantity; }
    static bool valid_price(Price price) {
        return price >= kMinimumPrice && price <= kMaximumPrice && price % kTick == 0;
    }

    void reject(int user, const std::string& token, char reason, Timestamp now) {
        expected[user].push_back({'J', now, token, 0, 0, reason, 0, ""});
    }

    ModelOrder* find_live(int user, const std::string& token) {
        for (ModelOrder& order : orders_) {
            if (order.live && order.user == user && order.token == token) return &order;
        }
        return nullptr;
    }

    // Best resting order on the side opposite to `side`: best price, then lowest order id.
    ModelOrder* best_opposite(const std::string& stock, char side) {
        ModelOrder* best = nullptr;
        for (ModelOrder& order : orders_) {
            if (!order.live || order.stock != stock || order.side == side) continue;
            if (best == nullptr) {
                best = &order;
                continue;
            }
            const bool better_price = side == 'B' ? order.price < best->price : order.price > best->price;
            if (better_price || (order.price == best->price && order.order_id < best->order_id)) best = &order;
        }
        return best;
    }

    bool would_cross(const std::string& stock, char side, Price price) {
        const ModelOrder* best = best_opposite(stock, side);
        if (best == nullptr) return false;
        return side == 'B' ? price >= best->price : price <= best->price;
    }

    void match(std::size_t aggressor_index, Timestamp now) {
        while (orders_[aggressor_index].leaves > 0) {
            ModelOrder& aggressor = orders_[aggressor_index];
            ModelOrder* resting = best_opposite(aggressor.stock, aggressor.side);
            if (resting == nullptr) break;
            const bool crosses = aggressor.is_market ||
                                 (aggressor.side == 'B' ? aggressor.price >= resting->price
                                                        : aggressor.price <= resting->price);
            if (!crosses) break;
            const Quantity quantity = std::min(aggressor.leaves, resting->leaves);
            const std::uint64_t match_number = ++last_match_number_;
            aggressor.leaves -= quantity;
            aggressor.cum += quantity;
            resting->leaves -= quantity;
            resting->cum += quantity;
            const ModelOrder& buy = aggressor.side == 'B' ? aggressor : *resting;
            const ModelOrder& sell = aggressor.side == 'B' ? *resting : aggressor;
            expected[buy.user].push_back(
                {'E', now, buy.token, quantity, resting->price, aggressor.side == 'B' ? 'R' : 'A', match_number, ""});
            expected[sell.user].push_back(
                {'E', now, sell.token, quantity, resting->price, aggressor.side == 'S' ? 'R' : 'A', match_number, ""});
            if (resting->leaves == 0) resting->live = false;
        }
    }

    std::map<std::string, ModelTokenInstructions> used_tokens_[2];
    std::vector<ModelOrder> orders_;
    std::uint64_t last_order_id_ = 0;
    std::uint64_t last_match_number_ = 0;
    bool start_of_day_sent_[2] = {false, false};
};

void compare(const ReceivedMessage& actual, const ExpectedMessage& expected, std::size_t index) {
    CHECK_EQ(actual.type, expected.type);
    if (actual.type != expected.type) return;
    CHECK_EQ(actual.sequence, index + 1);
    switch (actual.type) {
        case 'S':
            CHECK_EQ(actual.system_event.timestamp, expected.timestamp);
            CHECK_EQ(actual.system_event.event, expected.reason_or_liquidity);
            break;
        case 'A':
            CHECK_EQ(actual.accepted.timestamp, expected.timestamp);
            CHECK_EQ(token_text(actual.accepted.token), expected.token);
            CHECK_EQ(actual.accepted.shares, expected.shares);
            CHECK_EQ(actual.accepted.price, expected.price);
            CHECK_EQ(actual.accepted.order_reference, expected.reference);
            break;
        case 'U':
            CHECK_EQ(actual.replaced.timestamp, expected.timestamp);
            CHECK_EQ(token_text(actual.replaced.replacement_token), expected.token);
            CHECK_EQ(token_text(actual.replaced.previous_token), expected.previous_token);
            CHECK_EQ(actual.replaced.shares, expected.shares);
            CHECK_EQ(actual.replaced.price, expected.price);
            CHECK_EQ(actual.replaced.order_reference, expected.reference);
            break;
        case 'C':
            CHECK_EQ(actual.canceled.timestamp, expected.timestamp);
            CHECK_EQ(token_text(actual.canceled.token), expected.token);
            CHECK_EQ(actual.canceled.shares, expected.shares);
            CHECK_EQ(actual.canceled.reason, expected.reason_or_liquidity);
            break;
        case 'E':
            CHECK_EQ(actual.executed.timestamp, expected.timestamp);
            CHECK_EQ(token_text(actual.executed.token), expected.token);
            CHECK_EQ(actual.executed.shares, expected.shares);
            CHECK_EQ(actual.executed.price, expected.price);
            CHECK_EQ(actual.executed.liquidity, expected.reason_or_liquidity);
            CHECK_EQ(actual.executed.match_number, expected.reference);
            break;
        case 'J':
            CHECK_EQ(actual.rejected.timestamp, expected.timestamp);
            CHECK_EQ(token_text(actual.rejected.token), expected.token);
            CHECK_EQ(actual.rejected.reason, expected.reason_or_liquidity);
            break;
        default: FAIL(std::string("unexpected message type ") + actual.type);
    }
}

struct SimulatedUser {
    std::string username;
    std::string password;
    SessionId session;
    bool cancel_on_disconnect;
    RecordingHandler handler;
    std::unique_ptr<ClientLink<RecordingHandler>> link;
    bool connected = false;
    int next_token_number = 0;
    std::vector<std::string> tokens_sent;
};

void run_randomized_conversation(std::uint64_t seed, int steps) {
    TestExchange exchange;
    ReferenceModel model;
    std::mt19937_64 random(seed);
    auto chance = [&](int percent) { return static_cast<int>(random() % 100) < percent; };
    auto pick = [&](std::size_t count) { return static_cast<std::size_t>(random() % count); };

    SimulatedUser users[2] = {{"alice", "alicepw", 1, true, {}, nullptr, false, 0, {}},
                              {"bob", "bobpw", 2, false, {}, nullptr, false, 0, {}}};
    for (SimulatedUser& user : users) user.link = std::make_unique<ClientLink<RecordingHandler>>(exchange, user.handler);

    auto connect = [&](int user_index) {
        SimulatedUser& user = users[user_index];
        REQUIRE(user.link->login(user.username, user.password, user.link->client().next_expected_sequence()));
        user.connected = true;
        model.login(user_index, exchange.now());
    };
    auto pump_all = [&] {
        for (SimulatedUser& user : users) {
            if (user.connected) user.link->pump();
        }
    };
    auto random_price = [&]() -> std::uint32_t {
        if (chance(2)) return kMarketPrice;
        if (chance(2)) return kHundredDollars + 50;     // off tick
        if (chance(1)) return kMaximumPrice + kTick;   // out of range
        return static_cast<std::uint32_t>(kHundredDollars + (static_cast<Price>(pick(11)) - 5) * kTick);
    };
    auto random_quantity = [&]() -> Quantity { return chance(3) ? 0 : static_cast<Quantity>(1 + pick(300)); };
    auto known_or_unknown_token = [&](SimulatedUser& user) -> std::string {
        if (user.tokens_sent.empty() || chance(5)) return "UNKNOWN" + std::to_string(pick(5));
        return user.tokens_sent[pick(user.tokens_sent.size())];
    };
    auto fresh_or_reused_token = [&](SimulatedUser& user, int reuse_percent) -> std::string {
        if (!user.tokens_sent.empty() && chance(reuse_percent)) return user.tokens_sent[pick(user.tokens_sent.size())];
        std::string token = user.username.substr(0, 1) + std::to_string(++user.next_token_number);
        user.tokens_sent.push_back(token);
        return token;
    };

    int disconnect_count = 0;
    connect(0);
    connect(1);
    for (int step = 0; step < steps; ++step) {
        exchange.advance(1 + pick(1000));
        const Timestamp now = exchange.now();
        const int user_index = static_cast<int>(pick(2));
        SimulatedUser& user = users[user_index];
        if (!user.connected) {
            if (chance(30)) connect(user_index);
            pump_all();
            continue;
        }
        auto& client = user.link->client();
        const int action = static_cast<int>(pick(100));
        if (action < 45) {
            const std::string token = fresh_or_reused_token(user, 8);
            const char side = "BSTE"[pick(4)];
            const std::string stock = chance(4) ? "ZZZZ" : (chance(70) ? "AAPL" : "MSFT");
            const std::uint32_t price = random_price();
            const Quantity shares = random_quantity();
            const std::uint32_t time_in_force =
                chance(2) ? 30 : (chance(20) ? 0 : (chance(15) ? 99998u : 99999u));
            const char display = chance(2) ? 'N' : (chance(10) ? 'P' : 'Y');
            REQUIRE(client.enter_order(enter_order(token, side, shares, stock, static_cast<Price>(price), time_in_force,
                                                   display),
                                       now));
            model.enter_order(user_index, token, side, shares, stock, price, time_in_force, display, now);
        } else if (action < 65) {
            const std::string token = known_or_unknown_token(user);
            const Quantity shares = chance(40) ? 0 : static_cast<Quantity>(pick(400));
            REQUIRE(client.cancel_order(cancel_order(token, shares), now));
            model.cancel_order(user_index, token, shares, now);
        } else if (action < 83) {
            const std::string existing = known_or_unknown_token(user);
            const std::string replacement = fresh_or_reused_token(user, 8);
            const Quantity shares = random_quantity() + (chance(50) ? 0 : static_cast<Quantity>(pick(100)));
            std::uint32_t price = random_price();
            if (price == kMarketPrice) price = kHundredDollars;
            const std::uint32_t time_in_force = chance(5) ? 0 : (chance(30) ? 99998u : 99999u);
            const char display = chance(8) ? 'P' : 'Y';
            REQUIRE(client.replace_order(
                replace_order(existing, replacement, shares, static_cast<Price>(price), time_in_force, display), now));
            model.replace_order(user_index, existing, replacement, shares, price, time_in_force, display, now);
        } else if (action < 86) {
            const std::string token = known_or_unknown_token(user);
            ouch42::ModifyOrder modify;
            modify.token = token14(token);
            modify.side = 'S';
            modify.shares = 1;
            REQUIRE(client.modify_order(modify, now));
            model.modify_order(user_index, token, now);
        } else if (action < 89) {
            // Malformed: unknown OUCH message type. Ignored by the gateway.
            const std::string frame = Golden().byte('Q').u64(step).soup('U');
            char* destination = client.transport().prepare(frame.size());
            REQUIRE(destination != nullptr);
            std::memcpy(destination, frame.data(), frame.size());
            client.transport().commit(frame.size());
        } else if (action < 93) {
            REQUIRE(client.send_heartbeat(now));
        } else {
            user.link->drop_transport();
            user.connected = false;
            ++disconnect_count;
            model.disconnect(user_index, user.cancel_on_disconnect, now);
        }
        pump_all();
    }

    exchange.advance(1);
    for (int user_index = 0; user_index < 2; ++user_index) {
        if (!users[user_index].connected) connect(user_index);
    }
    pump_all();

    for (int user_index = 0; user_index < 2; ++user_index) {
        const SimulatedUser& user = users[user_index];
        const std::vector<ExpectedMessage>& expected = model.expected[user_index];
        CHECK(user.handler.sequence_mismatches.empty());
        CHECK_EQ(user.handler.unknown_messages, 0);
        REQUIRE_EQ(user.handler.messages.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            compare(user.handler.messages[index], expected[index], index);
        }
        CHECK_EQ(exchange.user_state(user.session).store().last_seq(), expected.size());
    }

    // The conversation really covers the interesting paths.
    std::map<std::string, int> coverage;
    for (const std::vector<ExpectedMessage>& messages : model.expected) {
        for (const ExpectedMessage& message : messages) {
            ++coverage[std::string(1, message.type)];
            if (message.type == 'J' || message.type == 'C' || message.type == 'E') {
                ++coverage[std::string(1, message.type) + message.reason_or_liquidity];
            }
        }
    }
    for (const char* required : {"A", "E", "U", "C", "J", "ER", "EA", "CU", "CI", "JS", "JO", "JX", "JZ"}) {
        CHECK_GT(coverage[required], 0);
    }
    CHECK_GT(coverage["E"], 100);
    CHECK_GT(disconnect_count, 10);

    // The engine's live orders are exactly the model's.
    const std::vector<const ModelOrder*> live = model.live_orders();
    CHECK_EQ(exchange.engine().live_order_count(), live.size());
    for (const ModelOrder* order : live) {
        const SessionId session = users[order->user].session;
        const std::optional<OrderInfo> engine_order = exchange.find_live_order(session, padded_token(order->token));
        REQUIRE(engine_order.has_value());
        CHECK_EQ(engine_order->order_id, order->order_id);
        CHECK_EQ(engine_order->leaves_qty, order->leaves);
        CHECK_EQ(engine_order->cum_qty, order->cum);
        CHECK_EQ(engine_order->price, order->price);
    }
}

}  // namespace

TEST_CASE(gateway_ouch_randomized, conversation_matches_reference_model_seed_1) { run_randomized_conversation(1, 4000); }
TEST_CASE(gateway_ouch_randomized, conversation_matches_reference_model_seed_2) { run_randomized_conversation(2, 4000); }
TEST_CASE(gateway_ouch_randomized, conversation_matches_reference_model_seed_3) {
    run_randomized_conversation(20261103, 4000);
}
