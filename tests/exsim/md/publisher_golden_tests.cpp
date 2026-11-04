// Golden ITCH 5.0 / MoldUDP64 encodings for every event mapping of
// docs/GATEWAYS.md section 4, driven through a real exsim::MatchingEngine.
// Expected bytes are literal hex, independent of the codec encoders.

#include <exsim/matching_engine.hpp>
#include <exsim/md/publisher.hpp>
#include <tests/exsim/md/md_test_support.hpp>
#include <tests/framework/test.hpp>

#include <cstdio>
#include <initializer_list>

using namespace exsim;
using namespace exsim::mdtest;

namespace {

constexpr std::string_view kSession = "45 58 53 49 4D 20 20 20 20 20";  // "EXSIM     "
constexpr std::string_view kAAA = "41 41 41 20 20 20 20 20";
constexpr std::string_view kBBB = "42 42 42 20 20 20 20 20";

std::string concat(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (auto part : parts) {
        out += part;
        out += ' ';
    }
    return out;
}

struct GoldenFeed {
    EngineConfig engine_config{4, 1024};
    MatchingEngine engine{engine_config};
    EventBuffer events{required_event_capacity(engine_config)};
    std::deque<Bytes> packets;
    md::MarketDataPublisher<PacketQueueSink> publisher;

    static md::PublisherConfig default_config() {
        md::PublisherConfig config;
        config.max_instruments = 4;
        config.retransmit_buffer_messages = 1024;
        config.noii_interval_ns = 1000;
        return config;
    }

    explicit GoldenFeed(std::initializer_list<InstrumentConfig> instruments,
                        md::PublisherConfig config = default_config(), Timestamp start = 0x10)
        : publisher(config, PacketQueueSink{&packets}) {
        for (const auto& instrument : instruments) REQUIRE(engine.add_instrument(instrument) != 0);
        REQUIRE(publisher.add_instruments_from(engine) == md::PublishStatus::Ok);
        REQUIRE(publisher.start_session(start) == md::PublishStatus::Ok);
    }

    // Applies one command and returns the ITCH messages it produced.
    std::vector<SequencedMessage> submit(const Command& command) {
        events.clear();
        REQUIRE(engine.handle(command, events) == HandleResult::Applied);
        const std::size_t first_packet = packets.size();
        REQUIRE(publisher.on_events(events) == md::PublishStatus::Ok);
        std::deque<Bytes> produced(packets.begin() + static_cast<std::ptrdiff_t>(first_packet), packets.end());
        return messages_of(produced);
    }

    std::vector<SequencedMessage> poll(Timestamp now) {
        const std::size_t first_packet = packets.size();
        REQUIRE(publisher.poll(now, engine) == md::PublishStatus::Ok);
        std::deque<Bytes> produced(packets.begin() + static_cast<std::ptrdiff_t>(first_packet), packets.end());
        return messages_of(produced);
    }
};

void check_message(const SequencedMessage& message, std::string_view golden_hex) {
    CHECK_EQ(hex_of(message.bytes), hex_of(hex_bytes(golden_hex)));
}

}  // namespace

TEST_CASE(md_golden, startup_sequence_packet) {
    GoldenFeed feed({test_instrument("AAA"), test_instrument("BBB", Phase::PreOpen)});
    REQUIRE_EQ(feed.packets.size(), 1u);
    const Bytes expected = hex_bytes(concat({
        kSession, "0000000000000001", "0006",
        "000C", "53 0000 0000 000000000010 4F",
        "0027", "52 0001 0000 000000000010", kAAA, "51 4E 00000001 4E 43 5A20 50 4E 4E 20 4E 00000000 4E",
        "0019", "48 0001 0000 000000000010", kAAA, "54 20 20202020",
        "0027", "52 0002 0000 000000000010", kBBB, "51 4E 00000001 4E 43 5A20 50 4E 4E 20 4E 00000000 4E",
        "0019", "48 0002 0000 000000000010", kBBB, "51 20 20202020",
        "000C", "53 0000 0000 000000000010 53",
    }));
    CHECK_EQ(hex_of(feed.packets[0]), hex_of(expected));
    CHECK_EQ(feed.publisher.next_sequence(), 7u);
}

TEST_CASE(md_golden, rested_is_add_order) {
    GoldenFeed feed({test_instrument("AAA")});
    feed.packets.clear();
    const auto messages = feed.submit(limit_order(0x20, 1, 1, 1, Side::BUY, 1'000'000, 100));
    REQUIRE_EQ(feed.packets.size(), 1u);
    CHECK_EQ(hex_of(feed.packets[0]).substr(0, 40), hex_of(hex_bytes(concat({kSession, "0000000000000005", "0001"}))));
    REQUIRE_EQ(messages.size(), 1u);
    CHECK_EQ(messages[0].sequence, 5u);
    check_message(messages[0], concat({"41 0001 0000 000000000020 0000000000000001 42 00000064", kAAA, "000F4240"}));
}

TEST_CASE(md_golden, continuous_fill_executes_resting_leg_only) {
    GoldenFeed feed({test_instrument("AAA")});
    (void)feed.submit(limit_order(0x20, 1, 1, 1, Side::BUY, 1'000'000, 100));
    // Sell aggressor: E on the resting buy (order 1), match 1.
    auto messages = feed.submit(limit_order(0x30, 1, 2, 1, Side::SELL, 1'000'000, 30));
    REQUIRE_EQ(messages.size(), 1u);
    check_message(messages[0], "45 0001 0000 000000000030 0000000000000001 0000001E 0000000000000001");
    // Buy aggressor IOC larger than the resting sell: E on the sell, nothing for the IOC remainder.
    (void)feed.submit(limit_order(0x40, 1, 2, 2, Side::SELL, 1'000'100, 50));
    messages = feed.submit(limit_order(0x50, 1, 3, 1, Side::BUY, 1'000'100, 80, TimeInForce::IOC));
    REQUIRE_EQ(messages.size(), 1u);
    check_message(messages[0], "45 0001 0000 000000000050 0000000000000003 00000032 0000000000000002");
}

TEST_CASE(md_golden, partial_reduction_is_cancel_and_full_removal_is_delete) {
    GoldenFeed feed({test_instrument("AAA")});
    (void)feed.submit(limit_order(0x20, 1, 1, 1, Side::BUY, 1'000'000, 100));
    (void)feed.submit(limit_order(0x30, 1, 2, 1, Side::SELL, 1'000'000, 30));  // leaves 70, cum 30
    // reduce_to_total_qty 80 -> leaves 50: X for 20 shares.
    auto messages = feed.submit(CancelOrder{0x40, 1, 1, 9, 80});
    REQUIRE_EQ(messages.size(), 1u);
    check_message(messages[0], "58 0001 0000 000000000040 0000000000000001 00000014");
    // reduce_to_total_qty 20 <= cum: leaves 0, the order is removed: D.
    messages = feed.submit(CancelOrder{0x50, 1, 1, 10, 20});
    REQUIRE_EQ(messages.size(), 1u);
    check_message(messages[0], "44 0001 0000 000000000050 0000000000000001");
    // Full user cancel of another order: D.
    (void)feed.submit(limit_order(0x60, 1, 1, 2, Side::SELL, 1'000'100, 10));
    messages = feed.submit(CancelOrder{0x70, 1, 2, 11, 0});
    REQUIRE_EQ(messages.size(), 1u);
    check_message(messages[0], "44 0001 0000 000000000070 0000000000000003");
}

TEST_CASE(md_golden, replace_keeping_priority_is_cancel_or_nothing) {
    GoldenFeed feed({test_instrument("AAA")});
    (void)feed.submit(limit_order(0x20, 1, 1, 1, Side::BUY, 1'000'000, 100));
    auto messages = feed.submit(ReplaceOrder{0x30, 1, 1, 2, 1'000'000, 60, false});
    REQUIRE_EQ(messages.size(), 1u);
    check_message(messages[0], "58 0001 0000 000000000030 0000000000000001 00000028");
    // Token change only: priority kept, leaves unchanged: no ITCH output.
    messages = feed.submit(ReplaceOrder{0x40, 1, 2, 3, 1'000'000, 60, false});
    CHECK_EQ(messages.size(), 0u);
}

TEST_CASE(md_golden, replace_losing_priority_then_resting_is_one_order_replace) {
    GoldenFeed feed({test_instrument("AAA")});
    (void)feed.submit(limit_order(0x20, 1, 1, 1, Side::BUY, 1'000'000, 100));
    auto messages = feed.submit(ReplaceOrder{0x30, 1, 1, 2, 1'000'100, 100, false});
    REQUIRE_EQ(messages.size(), 1u);  // U only, the Rested is consumed (no A)
    check_message(messages[0], "55 0001 0000 000000000030 0000000000000001 0000000000000002 00000064 000F42A4");
    // force_priority_loss (OUCH U) at the same price also produces U.
    messages = feed.submit(ReplaceOrder{0x40, 1, 2, 3, 1'000'100, 100, true});
    REQUIRE_EQ(messages.size(), 1u);
    check_message(messages[0], "55 0001 0000 000000000040 0000000000000002 0000000000000003 00000064 000F42A4");
}

TEST_CASE(md_golden, replace_losing_priority_then_executing_is_delete_execute_add) {
    GoldenFeed feed({test_instrument("AAA")});
    (void)feed.submit(limit_order(0x20, 1, 1, 1, Side::BUY, 999'900, 100));
    (void)feed.submit(limit_order(0x30, 1, 2, 1, Side::SELL, 1'000'100, 40));
    const auto messages = feed.submit(ReplaceOrder{0x40, 1, 1, 2, 1'000'100, 100, false});
    REQUIRE_EQ(message_types(messages), std::string("DEA"));
    check_message(messages[0], "44 0001 0000 000000000040 0000000000000001");
    check_message(messages[1], "45 0001 0000 000000000040 0000000000000002 00000028 0000000000000001");
    check_message(messages[2], concat({"41 0001 0000 000000000040 0000000000000003 42 0000003C", kAAA, "000F42A4"}));
}

TEST_CASE(md_golden, replace_to_quantity_at_or_below_executed_is_delete) {
    GoldenFeed feed({test_instrument("AAA")});
    (void)feed.submit(limit_order(0x20, 1, 1, 1, Side::BUY, 1'000'000, 100));
    (void)feed.submit(limit_order(0x30, 1, 2, 1, Side::SELL, 1'000'000, 30));
    const auto messages = feed.submit(ReplaceOrder{0x40, 1, 1, 2, 1'000'000, 20, false});
    REQUIRE_EQ(messages.size(), 1u);
    check_message(messages[0], "44 0001 0000 000000000040 0000000000000001");
}

TEST_CASE(md_golden, opening_auction_is_two_leg_executions_and_cross_with_fresh_match) {
    GoldenFeed feed({test_instrument("AAA", Phase::PreOpen)});
    (void)feed.submit(limit_order(0x20, 1, 1, 1, Side::BUY, 1'000'000, 100));
    auto messages = feed.submit(limit_order(0x30, 1, 2, 1, Side::SELL, 999'900, 60));
    REQUIRE_EQ(messages.size(), 1u);  // call phase: the crossing sell rests
    check_message(messages[0], concat({"41 0001 0000 000000000030 0000000000000002 53 0000003C", kAAA, "000F41DC"}));
    messages = feed.submit(SetPhase{0x40, 1, Phase::Continuous});
    REQUIRE_EQ(message_types(messages), std::string("CCQH"));
    // Buy leg first, both at the uncross price, non-printable.
    check_message(messages[0], "43 0001 0000 000000000040 0000000000000001 0000003C 0000000000000001 4E 000F4240");
    check_message(messages[1], "43 0001 0000 000000000040 0000000000000002 0000003C 0000000000000001 4E 000F4240");
    // Q carries AuctionSummary.match_id (2), not the constituent fill's (1); cross type O.
    check_message(messages[2], concat({"51 0001 0000 000000000040 000000000000003C", kAAA, "000F4240 0000000000000002 4F"}));
    check_message(messages[3], concat({"48 0001 0000 000000000040", kAAA, "54 20 20202020"}));
}

TEST_CASE(md_golden, closing_auction_cross_type_and_end_of_day_expiry) {
    GoldenFeed feed({test_instrument("AAA")});
    auto messages = feed.submit(SetPhase{0x20, 1, Phase::PreClose});
    REQUIRE_EQ(messages.size(), 1u);
    check_message(messages[0], concat({"48 0001 0000 000000000020", kAAA, "51 20 20202020"}));
    (void)feed.submit(limit_order(0x30, 1, 1, 1, Side::BUY, 1'000'000, 100));
    (void)feed.submit(limit_order(0x40, 1, 2, 1, Side::SELL, 999'900, 60));
    messages = feed.submit(SetPhase{0x50, 1, Phase::Closed});
    // Uncross, then the DAY remainder (40 shares of order 1) expires: D, then H.
    REQUIRE_EQ(message_types(messages), std::string("CCQDH"));
    check_message(messages[2], concat({"51 0001 0000 000000000050 000000000000003C", kAAA, "000F4240 0000000000000002 43"}));
    check_message(messages[3], "44 0001 0000 000000000050 0000000000000001");
    check_message(messages[4], concat({"48 0001 0000 000000000050", kAAA, "48 20 20202020"}));
}

TEST_CASE(md_golden, halted_crossed_book_uncrosses_with_cross_type_h) {
    GoldenFeed feed({test_instrument("AAA", Phase::PreOpen)});
    (void)feed.submit(limit_order(0x20, 1, 1, 1, Side::BUY, 1'000'000, 100));
    (void)feed.submit(limit_order(0x30, 1, 2, 1, Side::SELL, 999'900, 60));
    auto messages = feed.submit(SetPhase{0x40, 1, Phase::Halted});
    REQUIRE_EQ(messages.size(), 1u);  // crossed book is inherited, no uncross
    check_message(messages[0], concat({"48 0001 0000 000000000040", kAAA, "48 20 20202020"}));
    messages = feed.submit(SetPhase{0x50, 1, Phase::Continuous});
    REQUIRE_EQ(message_types(messages), std::string("CCQH"));
    check_message(messages[2], concat({"51 0001 0000 000000000050 000000000000003C", kAAA, "000F4240 0000000000000002 48"}));
    check_message(messages[3], concat({"48 0001 0000 000000000050", kAAA, "54 20 20202020"}));
}

TEST_CASE(md_golden, trading_action_state_per_phase) {
    GoldenFeed feed({test_instrument("AAA")});
    const std::pair<Phase, std::string_view> transitions[] = {
        {Phase::Halted, "48"}, {Phase::PreOpen, "51"}, {Phase::Continuous, "54"},
        {Phase::PreClose, "51"}, {Phase::Closed, "48"},
    };
    Timestamp ts = 0x20;
    for (const auto& [phase, state] : transitions) {
        const auto messages = feed.submit(SetPhase{ts, 1, phase});
        REQUIRE_EQ(messages.size(), 1u);
        char time_hex[13];
        std::snprintf(time_hex, sizeof time_hex, "%012llx", static_cast<unsigned long long>(ts));
        check_message(messages[0], concat({"48 0001 0000", time_hex, kAAA, state, "20 20202020"}));
        ts += 0x10;
    }
}

TEST_CASE(md_golden, mass_cancel_deletes_each_order_and_stp_cancel_resting_deletes) {
    GoldenFeed feed({test_instrument("AAA")});
    (void)feed.submit(limit_order(0x20, 1, 1, 1, Side::BUY, 999'900, 10));
    (void)feed.submit(limit_order(0x30, 1, 1, 2, Side::BUY, 999'800, 10));
    (void)feed.submit(limit_order(0x40, 1, 2, 1, Side::SELL, 1'000'100, 10));
    auto messages = feed.submit(MassCancel{0x50, 1, 0, 77, false});
    REQUIRE_EQ(message_types(messages), std::string("DD"));
    check_message(messages[0], "44 0001 0000 000000000050 0000000000000001");
    check_message(messages[1], "44 0001 0000 000000000050 0000000000000002");
    // STP cancel-resting: the resting sell (order 3) is deleted, the aggressor rests: D then A.
    NewOrder aggressor = limit_order(0x60, 1, 3, 1, Side::BUY, 1'000'100, 5);
    aggressor.owner = 7;
    aggressor.stp = StpMode::CancelResting;
    NewOrder resting_owner = limit_order(0x58, 1, 4, 1, Side::SELL, 1'000'200, 5);
    resting_owner.owner = 7;
    (void)feed.submit(resting_owner);  // order 4
    // Make order 3's owner match: re-enter it with owner 7.
    (void)feed.submit(CancelOrder{0x59, 2, 1, 5, 0});
    NewOrder resting_same_owner = limit_order(0x5A, 1, 2, 2, Side::SELL, 1'000'100, 5);
    resting_same_owner.owner = 7;
    (void)feed.submit(resting_same_owner);  // order 5
    messages = feed.submit(aggressor);      // order 6
    REQUIRE_EQ(message_types(messages), std::string("DA"));
    check_message(messages[0], "44 0001 0000 000000000060 0000000000000005");
    check_message(messages[1], concat({"41 0001 0000 000000000060 0000000000000006 42 00000005", kAAA, "000F42A4"}));
    // FOK that cannot fill: no ITCH output at all.
    messages = feed.submit(limit_order(0x70, 1, 3, 2, Side::SELL, 999'000, 500, TimeInForce::FOK));
    CHECK_EQ(messages.size(), 0u);
}

TEST_CASE(md_golden, one_sided_noii_and_crossed_noii) {
    GoldenFeed feed({test_instrument("AAA", Phase::PreOpen)}, GoldenFeed::default_config(), 0);
    (void)feed.submit(limit_order(0x20, 1, 1, 1, Side::BUY, 1'000'000, 100));
    CHECK_EQ(feed.poll(999).size(), 0u);  // interval (1000 ns) not yet elapsed
    auto messages = feed.poll(1000);
    REQUIRE_EQ(messages.size(), 1u);
    // One-sided: paired 0, imbalance 100 buy, near = far = 0, reference = configured reference, cross O.
    check_message(messages[0], concat({"49 0001 0000 0000000003E8 0000000000000000 0000000000000064 42", kAAA,
                                       "00000000 00000000 000F4240 4F 20"}));
    (void)feed.submit(limit_order(1100, 1, 2, 1, Side::SELL, 999'900, 60));
    CHECK_EQ(feed.poll(1999).size(), 0u);
    messages = feed.poll(2000);
    REQUIRE_EQ(messages.size(), 1u);
    // Crossed: paired 60, imbalance 40 buy, near = far = indicative price 100.00.
    check_message(messages[0], concat({"49 0001 0000 0000000007D0 000000000000003C 0000000000000028 42", kAAA,
                                       "000F4240 000F4240 000F4240 4F 20"}));
}

TEST_CASE(md_golden, closing_noii_uses_last_trade_as_reference) {
    GoldenFeed feed({test_instrument("AAA"), test_instrument("BBB")}, GoldenFeed::default_config(), 0);
    (void)feed.submit(limit_order(0x20, 1, 1, 1, Side::BUY, 999'900, 10));
    (void)feed.submit(limit_order(0x30, 1, 2, 1, Side::SELL, 999'900, 10));  // last trade 99.99
    (void)feed.submit(SetPhase{0x40, 1, Phase::PreClose});
    (void)feed.submit(limit_order(0x50, 1, 2, 2, Side::SELL, 1'000'000, 50));
    const auto messages = feed.poll(1000);
    REQUIRE_EQ(messages.size(), 1u);  // BBB is continuous: no NOII for it
    check_message(messages[0], concat({"49 0001 0000 0000000003E8 0000000000000000 0000000000000032 53", kAAA,
                                       "00000000 00000000 000F41DC 43 20"}));
}

TEST_CASE(md_golden, heartbeat_end_of_day_and_repeated_end_of_session) {
    GoldenFeed feed({test_instrument("AAA")}, GoldenFeed::default_config(), 0);
    feed.packets.clear();
    REQUIRE(feed.publisher.poll_heartbeat(999'999'999) == md::PublishStatus::Ok);
    CHECK_EQ(feed.packets.size(), 0u);
    REQUIRE(feed.publisher.poll_heartbeat(1'000'000'000) == md::PublishStatus::Ok);
    REQUIRE_EQ(feed.packets.size(), 1u);
    CHECK_EQ(hex_of(feed.packets[0]), hex_of(hex_bytes(concat({kSession, "0000000000000005", "0000"}))));
    REQUIRE(feed.publisher.poll_heartbeat(1'500'000'000) == md::PublishStatus::Ok);
    CHECK_EQ(feed.packets.size(), 1u);
    (void)feed.submit(limit_order(1'800'000'000, 1, 1, 1, Side::BUY, 1'000'000, 100));
    REQUIRE(feed.publisher.poll_heartbeat(2'500'000'000) == md::PublishStatus::Ok);
    CHECK_EQ(feed.packets.size(), 2u);  // silence counts from the data packet at 1.8 s
    REQUIRE(feed.publisher.poll_heartbeat(2'800'000'000) == md::PublishStatus::Ok);
    REQUIRE_EQ(feed.packets.size(), 3u);
    CHECK_EQ(hex_of(feed.packets[2]), hex_of(hex_bytes(concat({kSession, "0000000000000006", "0000"}))));
    CHECK_EQ(feed.publisher.stats().heartbeats_sent, 2u);

    feed.packets.clear();
    REQUIRE(feed.publisher.end_of_day(0x1000000000) == md::PublishStatus::Ok);
    REQUIRE_EQ(feed.packets.size(), 2u);
    CHECK_EQ(hex_of(feed.packets[0]),
             hex_of(hex_bytes(concat({kSession, "0000000000000006", "0002", "000C", "53 0000 0000 001000000000 45",
                                      "000C", "53 0000 0000 001000000000 43"}))));
    CHECK_EQ(hex_of(feed.packets[1]), hex_of(hex_bytes(concat({kSession, "0000000000000008", "FFFF"}))));
    CHECK(feed.publisher.state() == md::PublisherState::Ended);
    EventBuffer empty(1);
    CHECK(feed.publisher.on_events(empty) == md::PublishStatus::NotAllowedInState);
    // After the end, the End of Session packet repeats every heartbeat interval.
    REQUIRE(feed.publisher.poll_heartbeat(0x1000000000 + 999'999'999) == md::PublishStatus::Ok);
    CHECK_EQ(feed.packets.size(), 2u);
    REQUIRE(feed.publisher.poll_heartbeat(0x1000000000 + 1'000'000'000) == md::PublishStatus::Ok);
    REQUIRE_EQ(feed.packets.size(), 3u);
    CHECK_EQ(hex_of(feed.packets[2]), hex_of(hex_bytes(concat({kSession, "0000000000000008", "FFFF"}))));
}

TEST_CASE(md_golden, timestamp_is_relative_to_day_origin) {
    md::PublisherConfig config = GoldenFeed::default_config();
    config.day_origin = 5'000'000'000;
    GoldenFeed feed({test_instrument("AAA")}, config, 5'000'000'000 + 0x10);
    const auto startup = messages_of(feed.packets);
    REQUIRE_EQ(startup.size(), 4u);
    check_message(startup[0], "53 0000 0000 000000000010 4F");
    const auto messages = feed.submit(limit_order(5'000'000'000 + 0x20, 1, 1, 1, Side::BUY, 1'000'000, 100));
    REQUIRE_EQ(messages.size(), 1u);
    check_message(messages[0], concat({"41 0001 0000 000000000020 0000000000000001 42 00000064", kAAA, "000F4240"}));
}
