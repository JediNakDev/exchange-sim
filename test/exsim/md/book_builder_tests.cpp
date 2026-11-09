// ItchBookBuilder: level ordering, FIFO under every ITCH order message,
// crossed books, rejections that leave the book unchanged, capacity limits
// and instrument metadata.

#include <exsim/md/book_builder.hpp>
#include <test/exsim/md/md_test_support.hpp>
#include <test/framework/test.hpp>

#include <stdexcept>
#include <vector>

using namespace exsim;
using namespace exsim::mdtest;
namespace itch = exsim::protocol::itch50;

namespace {

template <class Message>
Bytes encoded(const Message& message) {
    char bytes[64];
    const std::size_t length = itch::encode(message, bytes, sizeof bytes);
    REQUIRE(length != 0);
    return Bytes(bytes, bytes + length);
}

Bytes add_order(InstrumentId locate, OrderId reference, Side side, Quantity shares, Price price) {
    itch::AddOrder message;
    message.stock_locate = locate;
    message.order_reference = reference;
    message.side = side;
    message.shares = shares;
    message.stock = Symbol("AAA");
    message.price = price;
    return encoded(message);
}

Bytes executed(InstrumentId locate, OrderId reference, Quantity shares) {
    itch::OrderExecuted message;
    message.stock_locate = locate;
    message.order_reference = reference;
    message.shares = shares;
    message.match_number = 1;
    return encoded(message);
}

Bytes executed_with_price(InstrumentId locate, OrderId reference, Quantity shares) {
    itch::OrderExecutedWithPrice message;
    message.stock_locate = locate;
    message.order_reference = reference;
    message.shares = shares;
    message.match_number = 1;
    message.printable = 'N';
    message.price = 1;
    return encoded(message);
}

Bytes cancel(InstrumentId locate, OrderId reference, Quantity shares) {
    itch::OrderCancel message;
    message.stock_locate = locate;
    message.order_reference = reference;
    message.shares = shares;
    return encoded(message);
}

Bytes delete_order(InstrumentId locate, OrderId reference) {
    itch::OrderDelete message;
    message.stock_locate = locate;
    message.order_reference = reference;
    return encoded(message);
}

Bytes replace(InstrumentId locate, OrderId original, OrderId replacement, Quantity shares, Price price) {
    itch::OrderReplace message;
    message.stock_locate = locate;
    message.original_reference = original;
    message.new_reference = replacement;
    message.shares = shares;
    message.price = price;
    return encoded(message);
}

std::vector<OrderId> fifo(const md::ItchBookBuilder& builder, InstrumentId instrument, Side side, Price price) {
    std::vector<OrderId> ids(64);
    ids.resize(builder.level_orders(instrument, side, price, ids.data(), ids.size()));
    return ids;
}

std::vector<LevelInfo> levels(const md::ItchBookBuilder& builder, InstrumentId instrument, Side side) {
    std::vector<LevelInfo> out(64);
    out.resize(builder.depth(instrument, side, out.data(), out.size()));
    return out;
}

bool same_level(const LevelInfo& level, Price price, Quantity qty, std::uint32_t count) {
    return level.price == price && level.qty == qty && level.order_count == count;
}

std::uint64_t digest(const md::ItchBookBuilder& builder) {
    std::vector<LevelInfo> level_buffer(64);
    std::vector<OrderId> id_buffer(64);
    return book_digest(builder, builder.max_instruments(), level_buffer, id_buffer,
                       [&](InstrumentId id) { return builder.trading_state(id); });
}

md::BookUpdateStatus apply_message(md::ItchBookBuilder& builder, const Bytes& message) { return builder.apply(message); }

}  // namespace

TEST_CASE(md_book_builder, config_rules) {
    auto construct_throws = [](const md::BookBuilderConfig& config) {
        try {
            md::ItchBookBuilder builder(config);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    md::BookBuilderConfig config{4, 16, 8};
    CHECK(!construct_throws(config));
    config.max_instruments = 0;
    CHECK(construct_throws(config));
    config = {4, 0, 8};
    CHECK(construct_throws(config));
    config = {4, 16, 0};
    CHECK(construct_throws(config));
}

TEST_CASE(md_book_builder, levels_in_priority_order_and_crossed_book) {
    md::ItchBookBuilder builder({4, 64, 16});
    CHECK(apply_message(builder, add_order(1, 1, Side::BUY, 10, 1'000)) == md::BookUpdateStatus::Applied);
    CHECK(apply_message(builder, add_order(1, 2, Side::BUY, 20, 1'200)) == md::BookUpdateStatus::Applied);
    CHECK(apply_message(builder, add_order(1, 3, Side::BUY, 5, 1'100)) == md::BookUpdateStatus::Applied);
    CHECK(apply_message(builder, add_order(1, 4, Side::BUY, 7, 1'200)) == md::BookUpdateStatus::Applied);
    CHECK(apply_message(builder, add_order(1, 5, Side::SELL, 3, 1'150)) == md::BookUpdateStatus::Applied);  // crosses
    CHECK(apply_message(builder, add_order(1, 6, Side::SELL, 4, 1'300)) == md::BookUpdateStatus::Applied);
    CHECK(apply_message(builder, add_order(2, 7, Side::SELL, 9, 1'000)) == md::BookUpdateStatus::Applied);

    const auto bids = levels(builder, 1, Side::BUY);
    REQUIRE_EQ(bids.size(), 3u);
    CHECK(same_level(bids[0], 1'200, 27, 2));
    CHECK(same_level(bids[1], 1'100, 5, 1));
    CHECK(same_level(bids[2], 1'000, 10, 1));
    const auto asks = levels(builder, 1, Side::SELL);
    REQUIRE_EQ(asks.size(), 2u);
    CHECK(same_level(asks[0], 1'150, 3, 1));
    CHECK(same_level(asks[1], 1'300, 4, 1));
    CHECK(*builder.best_bid(1) > *builder.best_ask(1));  // crossed is represented as is
    CHECK(fifo(builder, 1, Side::BUY, 1'200) == std::vector<OrderId>({2, 4}));
    CHECK_EQ(levels(builder, 2, Side::SELL).size(), 1u);
    CHECK(!builder.best_bid(2).has_value());
    CHECK_EQ(builder.order_count(), 7u);
    // depth honours the caller's limit.
    LevelInfo top[1];
    CHECK_EQ(builder.depth(1, Side::BUY, top, 1), 1u);
    CHECK_EQ(top[0].price, 1'200);
    const auto order = builder.find_order(3);
    REQUIRE(order.has_value());
    CHECK_EQ(order->instrument, 1u);
    CHECK(order->side == Side::BUY);
    CHECK_EQ(order->shares, 5u);
    CHECK(!builder.find_order(99).has_value());
}

TEST_CASE(md_book_builder, reductions_keep_position_and_removal_empties_levels) {
    md::ItchBookBuilder builder({4, 64, 16});
    for (OrderId reference = 1; reference <= 4; ++reference)
        REQUIRE(apply_message(builder, add_order(1, reference, Side::SELL, 10, 2'000)) == md::BookUpdateStatus::Applied);
    CHECK(apply_message(builder, cancel(1, 1, 4)) == md::BookUpdateStatus::Applied);
    CHECK(apply_message(builder, executed(1, 2, 3)) == md::BookUpdateStatus::Applied);
    CHECK(apply_message(builder, executed_with_price(1, 3, 1)) == md::BookUpdateStatus::Applied);
    CHECK(fifo(builder, 1, Side::SELL, 2'000) == std::vector<OrderId>({1, 2, 3, 4}));
    CHECK(same_level(levels(builder, 1, Side::SELL)[0], 2'000, 32, 4));
    CHECK(apply_message(builder, executed(1, 2, 7)) == md::BookUpdateStatus::Applied);  // fully executed
    CHECK(apply_message(builder, cancel(1, 3, 9)) == md::BookUpdateStatus::Applied);    // fully canceled
    CHECK(fifo(builder, 1, Side::SELL, 2'000) == std::vector<OrderId>({1, 4}));
    CHECK(apply_message(builder, delete_order(1, 1)) == md::BookUpdateStatus::Applied);
    CHECK(apply_message(builder, executed_with_price(1, 4, 10)) == md::BookUpdateStatus::Applied);
    CHECK_EQ(builder.level_count(1, Side::SELL), 0u);
    CHECK_EQ(builder.order_count(), 0u);
    // Freed slots and references are reusable.
    CHECK(apply_message(builder, add_order(1, 1, Side::BUY, 5, 1'000)) == md::BookUpdateStatus::Applied);
    CHECK_EQ(builder.order_count(), 1u);
}

TEST_CASE(md_book_builder, replace_moves_new_reference_to_the_tail) {
    md::ItchBookBuilder builder({4, 64, 16});
    REQUIRE(apply_message(builder, add_order(1, 1, Side::BUY, 10, 1'000)) == md::BookUpdateStatus::Applied);
    REQUIRE(apply_message(builder, add_order(1, 2, Side::BUY, 10, 1'000)) == md::BookUpdateStatus::Applied);
    REQUIRE(apply_message(builder, add_order(1, 3, Side::BUY, 10, 900)) == md::BookUpdateStatus::Applied);
    CHECK(apply_message(builder, replace(1, 1, 11, 15, 1'000)) == md::BookUpdateStatus::Applied);
    CHECK(fifo(builder, 1, Side::BUY, 1'000) == std::vector<OrderId>({2, 11}));
    CHECK(same_level(levels(builder, 1, Side::BUY)[0], 1'000, 25, 2));
    CHECK(!builder.find_order(1).has_value());
    // To another price: the old level disappears if it empties.
    CHECK(apply_message(builder, replace(1, 3, 13, 4, 1'100)) == md::BookUpdateStatus::Applied);
    const auto bids = levels(builder, 1, Side::BUY);
    REQUIRE_EQ(bids.size(), 2u);
    CHECK(same_level(bids[0], 1'100, 4, 1));
    CHECK(same_level(bids[1], 1'000, 25, 2));
    // The replacement keeps the original side.
    CHECK(builder.find_order(13)->side == Side::BUY);
}

TEST_CASE(md_book_builder, inconsistent_messages_are_rejected_without_change) {
    md::ItchBookBuilder builder({4, 64, 16});
    REQUIRE(apply_message(builder, add_order(1, 1, Side::BUY, 10, 1'000)) == md::BookUpdateStatus::Applied);
    REQUIRE(apply_message(builder, add_order(2, 2, Side::SELL, 10, 1'100)) == md::BookUpdateStatus::Applied);
    const std::uint64_t before = digest(builder);

    CHECK(apply_message(builder, add_order(1, 1, Side::SELL, 5, 1'200)) == md::BookUpdateStatus::DuplicateOrder);
    CHECK(apply_message(builder, add_order(0, 9, Side::BUY, 5, 1'000)) == md::BookUpdateStatus::UnknownInstrument);
    CHECK(apply_message(builder, add_order(5, 9, Side::BUY, 5, 1'000)) == md::BookUpdateStatus::UnknownInstrument);
    CHECK(apply_message(builder, add_order(1, 0, Side::BUY, 5, 1'000)) == md::BookUpdateStatus::Malformed);
    CHECK(apply_message(builder, add_order(1, 9, Side::BUY, 0, 1'000)) == md::BookUpdateStatus::Malformed);
    CHECK(apply_message(builder, executed(1, 7, 1)) == md::BookUpdateStatus::UnknownOrder);
    CHECK(apply_message(builder, executed(1, 1, 11)) == md::BookUpdateStatus::SharesExceedOrder);
    CHECK(apply_message(builder, executed(1, 1, 0)) == md::BookUpdateStatus::Malformed);
    CHECK(apply_message(builder, cancel(2, 1, 1)) == md::BookUpdateStatus::InstrumentMismatch);
    CHECK(apply_message(builder, delete_order(1, 2)) == md::BookUpdateStatus::InstrumentMismatch);
    CHECK(apply_message(builder, delete_order(1, 7)) == md::BookUpdateStatus::UnknownOrder);
    CHECK(apply_message(builder, replace(1, 7, 8, 5, 1'000)) == md::BookUpdateStatus::UnknownOrder);
    CHECK(apply_message(builder, replace(1, 1, 2, 5, 1'000)) == md::BookUpdateStatus::DuplicateOrder);
    CHECK(apply_message(builder, replace(2, 1, 8, 5, 1'000)) == md::BookUpdateStatus::InstrumentMismatch);
    CHECK(apply_message(builder, replace(1, 1, 0, 5, 1'000)) == md::BookUpdateStatus::Malformed);
    CHECK(apply_message(builder, replace(1, 1, 8, 0, 1'000)) == md::BookUpdateStatus::Malformed);
    Bytes truncated = add_order(1, 9, Side::BUY, 5, 1'000);
    truncated.pop_back();
    CHECK(apply_message(builder, truncated) == md::BookUpdateStatus::Malformed);
    CHECK(apply_message(builder, Bytes{}) == md::BookUpdateStatus::Malformed);
    CHECK(apply_message(builder, Bytes{'Z', 0, 1}) == md::BookUpdateStatus::UnsupportedType);

    CHECK_EQ(digest(builder), before);
    CHECK_EQ(builder.order_count(), 2u);
    CHECK_EQ(builder.rejected_message_count(), 19u);
    CHECK_EQ(builder.applied_message_count(), 2u);
    CHECK(builder.last_rejection() == md::BookUpdateStatus::UnsupportedType);
}

TEST_CASE(md_book_builder, capacity_limits_are_checked_before_changes) {
    md::ItchBookBuilder orders_limited({1, 2, 16});
    REQUIRE(apply_message(orders_limited, add_order(1, 1, Side::BUY, 1, 1'000)) == md::BookUpdateStatus::Applied);
    REQUIRE(apply_message(orders_limited, add_order(1, 2, Side::BUY, 1, 1'000)) == md::BookUpdateStatus::Applied);
    CHECK(apply_message(orders_limited, add_order(1, 3, Side::BUY, 1, 1'000)) == md::BookUpdateStatus::CapacityExceeded);
    // A replace frees its original slot first, so it fits.
    CHECK(apply_message(orders_limited, replace(1, 1, 3, 1, 1'000)) == md::BookUpdateStatus::Applied);

    md::ItchBookBuilder levels_limited({1, 16, 2});
    REQUIRE(apply_message(levels_limited, add_order(1, 1, Side::BUY, 1, 1'000)) == md::BookUpdateStatus::Applied);
    REQUIRE(apply_message(levels_limited, add_order(1, 2, Side::BUY, 1, 1'000)) == md::BookUpdateStatus::Applied);
    REQUIRE(apply_message(levels_limited, add_order(1, 3, Side::BUY, 1, 900)) == md::BookUpdateStatus::Applied);
    CHECK(apply_message(levels_limited, add_order(1, 4, Side::BUY, 1, 800)) == md::BookUpdateStatus::CapacityExceeded);
    CHECK(apply_message(levels_limited, add_order(1, 4, Side::BUY, 1, 900)) == md::BookUpdateStatus::Applied);  // existing level
    CHECK(apply_message(levels_limited, add_order(1, 5, Side::SELL, 1, 800)) == md::BookUpdateStatus::Applied);  // other side
    const std::uint64_t before = digest(levels_limited);
    // Order 1 shares its level: moving it to a third price needs a new level.
    CHECK(apply_message(levels_limited, replace(1, 1, 6, 1, 700)) == md::BookUpdateStatus::CapacityExceeded);
    CHECK_EQ(digest(levels_limited), before);
    CHECK(levels_limited.find_order(1).has_value());
    // Order 2 left alone at 1000 after this: its level vanishes, so the move fits.
    REQUIRE(apply_message(levels_limited, delete_order(1, 1)) == md::BookUpdateStatus::Applied);
    CHECK(apply_message(levels_limited, replace(1, 2, 6, 1, 700)) == md::BookUpdateStatus::Applied);
    const auto bids = levels(levels_limited, 1, Side::BUY);
    REQUIRE_EQ(bids.size(), 2u);
    CHECK_EQ(bids[0].price, 900);
    CHECK_EQ(bids[1].price, 700);
}

TEST_CASE(md_book_builder, metadata_messages) {
    md::ItchBookBuilder builder({4, 16, 8});
    CHECK_EQ(builder.last_system_event(), ' ');
    CHECK_EQ(builder.trading_state(1), ' ');
    CHECK(builder.symbol(1) == nullptr);
    CHECK_EQ(builder.find_instrument(Symbol("AAA")), 0u);

    itch::SystemEvent system;
    system.event = 'S';
    CHECK(apply_message(builder, encoded(system)) == md::BookUpdateStatus::Applied);
    CHECK_EQ(builder.last_system_event(), 'S');

    itch::StockDirectory directory;
    directory.stock_locate = 3;
    directory.stock = Symbol("CCC");
    CHECK(apply_message(builder, encoded(directory)) == md::BookUpdateStatus::Applied);
    CHECK_EQ(builder.find_instrument(Symbol("CCC")), 3u);
    REQUIRE(builder.symbol(3) != nullptr);
    CHECK(*builder.symbol(3) == Symbol("CCC"));
    directory.stock_locate = 9;
    CHECK(apply_message(builder, encoded(directory)) == md::BookUpdateStatus::UnknownInstrument);

    itch::StockTradingAction action;
    action.stock_locate = 3;
    action.stock = Symbol("CCC");
    action.trading_state = 'Q';
    action.reserved = ' ';
    CHECK(apply_message(builder, encoded(action)) == md::BookUpdateStatus::Applied);
    CHECK_EQ(builder.trading_state(3), 'Q');

    itch::NetOrderImbalance imbalance;
    imbalance.stock_locate = 3;
    imbalance.paired_shares = 40;
    imbalance.imbalance_direction = 'B';
    imbalance.cross_type = 'O';
    imbalance.price_variation = ' ';
    CHECK(apply_message(builder, encoded(imbalance)) == md::BookUpdateStatus::Applied);
    REQUIRE(builder.last_imbalance(3).has_value());
    CHECK_EQ(builder.last_imbalance(3)->paired_shares, 40u);
    CHECK(!builder.last_imbalance(2).has_value());

    itch::CrossTrade cross;
    cross.stock_locate = 3;
    cross.shares = 40;
    cross.price = 1'000;
    cross.match_number = 9;
    cross.cross_type = 'O';
    CHECK(apply_message(builder, encoded(cross)) == md::BookUpdateStatus::Applied);
    REQUIRE(builder.last_cross(3).has_value());
    CHECK_EQ(builder.last_cross(3)->match_number, 9u);

    itch::Trade trade;
    trade.stock_locate = 3;
    trade.order_reference = 0;
    trade.shares = 5;
    CHECK(apply_message(builder, encoded(trade)) == md::BookUpdateStatus::Applied);
    CHECK_EQ(builder.order_count(), 0u);

    itch::AddOrderMPID attributed;
    attributed.stock_locate = 3;
    attributed.order_reference = 77;
    attributed.side = Side::SELL;
    attributed.shares = 12;
    attributed.price = 1'500;
    CHECK(apply_message(builder, encoded(attributed)) == md::BookUpdateStatus::Applied);
    REQUIRE(builder.best_ask(3).has_value());
    CHECK_EQ(*builder.best_ask(3), 1'500);

    // on_message is the receiver interface over apply().
    builder.on_message(1, delete_order(3, 77));
    CHECK_EQ(builder.order_count(), 0u);
}
