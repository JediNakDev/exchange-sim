#include "ilink_test_support.hpp"
#include "test/framework/alloc_counter.hpp"

using namespace ilink_test;

TEST_CASE(ilink_crypto, rfc4231_and_base64url_validation) {
    const std::array<char, 20> key = [] { std::array<char, 20> result{}; result.fill(0x0b); return result; }();
    const auto signature = wire::hmac_sha256(key, "Hi There");
    constexpr unsigned char expected[] = {0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
                                          0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7};
    for (std::size_t i = 0; i < 32; ++i) CHECK_EQ(static_cast<unsigned char>(signature[i]), expected[i]);
    CHECK(wire::signature_equal(signature, signature));
    auto different = signature; different[0] ^= 1; CHECK(!wire::signature_equal(signature, different));
    std::array<char, 32> decoded{};
    REQUIRE(wire::decode_secret(std::string(43, 'A') + "=", decoded));
    CHECK(std::all_of(decoded.begin(), decoded.end(), [](char value) { return value == 0; }));
    CHECK(!wire::decode_secret(std::string(42, 'A') + "B", decoded));  // noncanonical unused bits
    CHECK(!wire::decode_secret(std::string(42, 'A') + "/", decoded));
    CHECK(!wire::decode_secret("invalid", decoded));
}

TEST_CASE(ilink_crypto, negotiation_signature_matches_independent_reference) {
    const auto configuration = ilink_test::config();
    const auto message = negotiation(configuration.users[0], 1700000000000001ull, 1700036000000000001ull);
    // Independently computed HMAC-SHA256 with a 32-byte zero key and the
    // exact CME payload "1700036000000000001\\n1700000000000001\\nAAA\\nEXSIM".
    constexpr unsigned char expected[] = {0x20,0x83,0xd6,0xd2,0xda,0x7e,0x62,0xf4,0x68,0xb4,0x9c,0xe5,0x33,0x2e,0xa0,0x1f,
                                          0xc7,0x81,0x84,0x82,0x4b,0xf3,0xbe,0x90,0xd4,0xa7,0x5c,0x61,0x0c,0xe1,0x4c,0x43};
    for (std::size_t i = 0; i < 32; ++i)
        CHECK_EQ(static_cast<unsigned char>(message.bytes[schema::Negotiate500::HMACSignature.offset + i]), expected[i]);
}

TEST_CASE(ilink_session, signed_login_and_order_reply_with_current_schema) {
    Exchange exchange; Client alice(exchange, 1); alice.login();
    CHECK_EQ(alice.connection.bound_session(), 1);
    alice.send(order(alice.user_config(), 1, 0, "ORDER-12345678901234"));
    const auto responses = frames(alice.memory); REQUIRE_EQ(responses.size(), 1u);
    REQUIRE_EQ(responses[0].id(), 522);
    const auto view = responses[0].view();
    CHECK_EQ(view.header.version, 9); CHECK_EQ(view.header.block_length, 226);
    CHECK_EQ(wire::get(view, schema::ExecutionReportNew522::ClOrdID), "ORDER-12345678901234");
    CHECK_EQ(wire::get(view, schema::ExecutionReportNew522::OrderRequestID), 0u);
    CHECK_EQ(wire::get(view, schema::ExecutionReportNew522::SecurityID), 800001);
    CHECK_EQ(exchange.core.engine().live_order_count(), 1u);
}

TEST_CASE(ilink_session, invalid_hmac_uuid_and_second_connection_rejected) {
    Exchange exchange;
    {
        Client bad(exchange, 1, 100);
        auto request = negotiation(bad.user_config(), bad.uuid, exchange.auth_time());
        request.bytes[0] ^= 1; bad.send(request);
        REQUIRE_EQ(frames(bad.memory)[0].id(), 502);
        CHECK(bad.connection.wants_close()); bad.connection.on_closed(exchange.core);
    }
    Client alice(exchange, 1); alice.login();
    Client duplicate(exchange, 1, 101);
    duplicate.uuid = alice.uuid + 1;
    duplicate.send(negotiation(duplicate.user_config(), duplicate.uuid, exchange.auth_time()));
    REQUIRE_EQ(frames(duplicate.memory)[0].id(), 502);
    CHECK_EQ(exchange.core.bound_connection_count(), 1u);
    duplicate.connection.on_closed(exchange.core);
    CHECK_EQ(alice.connection.bound_session(), 1);
}

TEST_CASE(ilink_session, switching_credentials_cannot_leak_a_session_reservation) {
    Exchange exchange; Client alice(exchange, 1);
    alice.send(negotiation(alice.user_config(), alice.uuid, exchange.auth_time()));
    alice.memory.clear();
    const auto& bob = exchange.configuration.users[1];
    alice.send(negotiation(bob, alice.uuid + 1, exchange.auth_time()));
    REQUIRE_EQ(frames(alice.memory)[0].id(), 502);
    alice.connection.on_closed(exchange.core);
    CHECK_EQ(exchange.user(1).reserved_by, 0u);
    CHECK_EQ(exchange.user(2).reserved_by, 0u);
    Client replacement(exchange, 1, 100); replacement.login(false);
}

TEST_CASE(ilink_session, tcp_fragmentation_coalescing_and_wrong_schema) {
    Exchange exchange; Client alice(exchange, 1);
    const auto request = encode(negotiation(alice.user_config(), alice.uuid, exchange.auth_time()));
    for (std::size_t length = 0; length < request.size(); ++length)
        CHECK_EQ(alice.connection.on_data(std::span(request).first(length), exchange.core), 0u);
    alice.feed(request); REQUIRE_EQ(frames(alice.memory)[0].id(), 501); alice.memory.clear();
    alice.login(false);
    auto first = encode(order(alice.user_config(), 1, 1, "ONE"));
    const auto second = encode(order(alice.user_config(), 2, 2, "TWO"));
    first.insert(first.end(), second.begin(), second.end()); alice.feed(first);
    CHECK_EQ(frames(alice.memory).size(), 2u); CHECK_EQ(exchange.core.engine().live_order_count(), 2u);
    alice.memory.clear(); auto invalid = second;
    invalid[10] = 8; alice.feed(invalid);
    CHECK_EQ(frames(alice.memory)[0].id(), 507);
}

TEST_CASE(ilink_orders, matching_partial_fill_and_mdp_trade_identifier) {
    Exchange exchange; Client alice(exchange, 1), bob(exchange, 2); alice.login(); bob.login();
    alice.send(order(alice.user_config(), 1, 1, "BUY", 1, 10)); alice.memory.clear();
    bob.send(order(bob.user_config(), 1, 1, "SELL", 2, 4));
    auto maker = frames(alice.memory), taker = frames(bob.memory);
    REQUIRE_EQ(maker.size(), 1u); REQUIRE_EQ(taker.size(), 2u);
    REQUIRE_EQ(maker[0].id(), 525); REQUIRE_EQ(taker[1].id(), 525);
    const auto trade = maker[0].view(), contra = taker[1].view();
    CHECK_EQ(wire::get(trade, schema::ExecutionReportTradeOutright525::LeavesQty), 6u);
    CHECK_EQ(wire::get(trade, schema::ExecutionReportTradeOutright525::CumQty), 4u);
    CHECK_EQ(wire::get(trade, schema::ExecutionReportTradeOutright525::MDTradeEntryID),
             wire::get(contra, schema::ExecutionReportTradeOutright525::MDTradeEntryID));
    protocol::cme::GroupView fills;
    REQUIRE_EQ(protocol::cme::decode_group(trade.trailing(), 15, fills), protocol::Status::ok);
    CHECK_EQ(fills.count, 1); CHECK_EQ(protocol::cme::load_le<std::uint32_t>(fills.entry(0).data() + 8), 4u);
    CHECK_EQ(wire::get(trade, schema::ExecutionReportTradeOutright525::AggressorIndicator), 0);
    CHECK_EQ(wire::get(contra, schema::ExecutionReportTradeOutright525::AggressorIndicator), 1);
}

TEST_CASE(ilink_orders, replace_cancel_mass_cancel_and_ownership) {
    Exchange exchange; Client alice(exchange, 1), bob(exchange, 2); alice.login(); bob.login();
    alice.send(order(alice.user_config(), 1, 1, "BUY"));
    // Keep the owned response while reading fields borrowed by MessageView.
    const auto accepted = frames(alice.memory);
    const auto id = wire::get(accepted[0].view(), schema::ExecutionReportNew522::OrderID);
    alice.memory.clear();
    using R = schema::OrderCancelReplaceRequest515;
    wire::Root<R> replace; request_fields(replace, alice.user_config(), 2, 2);
    replace.set(R::OrderID, id); replace.set(R::ClOrdID, "REPLACED"); replace.set(R::SecurityID, 800001); replace.set(R::Side, 1);
    replace.set(R::Price, 149000000000ll); replace.set(R::OrderQty, 12); replace.set(R::OrdType, '2');
    replace.set(R::TimeInForce, 0); replace.set(R::OFMOverride, 0); alice.send(replace);
    const auto changed = frames(alice.memory); REQUIRE_EQ(changed[0].id(), 531);
    const auto new_id = wire::get(changed[0].view(), schema::ExecutionReportModify531::OrderID);
    using C = schema::OrderCancelRequest516;
    wire::Root<C> cancel; request_fields(cancel, bob.user_config(), 1, 1);
    cancel.set(C::OrderID, new_id); cancel.set(C::ClOrdID, "REPLACED"); cancel.set(C::SecurityID, 800001); cancel.set(C::Side, 1);
    bob.send(cancel); REQUIRE_EQ(frames(bob.memory)[0].id(), 535); CHECK_EQ(exchange.core.engine().live_order_count(), 1u);
    alice.memory.clear(); request_fields(cancel, alice.user_config(), 3, 3); alice.send(cancel);
    REQUIRE_EQ(frames(alice.memory)[0].id(), 534); CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
    alice.memory.clear(); alice.send(order(alice.user_config(), 4, 4, "A")); alice.send(order(alice.user_config(), 5, 5, "B"));
    alice.memory.clear(); using M = schema::OrderMassActionRequest529;
    wire::Root<M> mass; request_fields(mass, alice.user_config(), 6, 6); mass.set(M::MassActionScope, 7); alice.send(mass);
    const auto canceled = frames(alice.memory); REQUIRE_EQ(canceled.size(), 3u); REQUIRE_EQ(canceled.back().id(), 562);
    CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
    CHECK_EQ(wire::get(canceled.back().view(), schema::OrderMassActionReport562::TotalAffectedOrders), 2u);
    protocol::cme::GroupView affected;
    REQUIRE_EQ(protocol::cme::decode_group(canceled.back().view().trailing(), 32, affected), protocol::Status::ok);
    CHECK_EQ(affected.count, 2);
}

TEST_CASE(ilink_orders, invalid_fields_party_precision_and_duplicate_requests) {
    Exchange exchange; Client alice(exchange, 1); alice.login();
    for (std::uint32_t seq = 1; seq <= 6; ++seq) {
        using M = schema::NewOrderSingle514;
        auto request = order(alice.user_config(), seq, seq, "BAD" + std::to_string(seq));
        if (seq == 1) request.set(M::PartyDetailsListReqID, 999);
        if (seq == 2) request.set(M::Price, 150000000001ll);
        if (seq == 3) request.set(M::TimeInForce, 1);
        if (seq == 4) request.set(M::DisplayQty, 2);
        if (seq == 5) request.set(M::OrderQty, 100000);
        if (seq == 6) request.set(M::ManualOrderIndicator, 2);
        alice.memory.clear(); alice.send(request);
        REQUIRE_EQ(frames(alice.memory)[0].id(), 521);
        CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
    }
    alice.memory.clear(); alice.send(order(alice.user_config(), 7, 7, "LIVE"));
    REQUIRE_EQ(frames(alice.memory)[0].id(), 522);
    alice.memory.clear(); alice.send(order(alice.user_config(), 8, 7, "DUPLICATE"));
    REQUIRE_EQ(frames(alice.memory)[0].id(), 521);
    CHECK_EQ(exchange.core.engine().live_order_count(), 1u);
    alice.memory.clear(); alice.send(order(alice.user_config(), 9, 9, "LIVE"));
    REQUIRE_EQ(frames(alice.memory)[0].id(), 521);
}

TEST_CASE(ilink_orders, engine_rejection_and_fak_elimination) {
    Exchange exchange; Client alice(exchange, 1); alice.login();
    alice.send(order(alice.user_config(), 1, 1, "OFF-TICK", 1, 10, 150005000000ll));
    REQUIRE_EQ(frames(alice.memory)[0].id(), 523); CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
    alice.memory.clear(); auto request = order(alice.user_config(), 2, 2, "FAK");
    request.set(schema::NewOrderSingle514::TimeInForce, 3); alice.send(request);
    const auto output = frames(alice.memory); REQUIRE_EQ(output.size(), 2u);
    CHECK_EQ(output[0].id(), 522); CHECK_EQ(output[1].id(), 524);
    CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
}

TEST_CASE(ilink_session, gaps_not_applied_sequence_reset_and_duplicate_prevention) {
    Exchange exchange; Client alice(exchange, 1); alice.login();
    alice.send(order(alice.user_config(), 3, 1, "GAP"));
    const auto output = frames(alice.memory); REQUIRE_EQ(output[0].id(), 513);
    CHECK_EQ(wire::get(output[0].view(), schema::NotApplied513::FromSeqNo), 1u);
    CHECK_EQ(wire::get(output[0].view(), schema::NotApplied513::MsgCount), 2u);
    CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
    wire::Root<schema::Sequence506> sequence;
    sequence.set(schema::Sequence506::UUID, alice.uuid); sequence.set(schema::Sequence506::NextSeqNo, 3);
    // CME does not require clients to send FaultToleranceIndicator.
    sequence.set(schema::Sequence506::KeepAliveIntervalLapsed, 0);
    alice.memory.clear(); alice.send(sequence); alice.send(order(alice.user_config(), 3, 1, "GAP"));
    REQUIRE_EQ(frames(alice.memory)[0].id(), 522); CHECK_EQ(exchange.core.engine().live_order_count(), 1u);
    alice.memory.clear(); alice.send(order(alice.user_config(), 3, 1, "GAP"));
    CHECK(alice.connection.wants_close()); CHECK_EQ(frames(alice.memory)[0].id(), 507);
    CHECK_EQ(exchange.core.engine().live_order_count(), 1u);
}

TEST_CASE(ilink_session, reconnect_replays_passive_fills_and_previous_uuid) {
    Exchange exchange; Client alice(exchange, 1), bob(exchange, 2); alice.login(); bob.login();
    alice.send(order(alice.user_config(), 1, 1, "BUY"));
    alice.connection.on_closed(exchange.core);
    bob.send(order(bob.user_config(), 1, 1, "SELL", 2, 4));
    CHECK_EQ(exchange.user(1).store.last_seq(), 2u);
    Client resumed(exchange, 1, 101); resumed.login(false, 2);
    using R = schema::RetransmitRequest508;
    wire::Root<R> request; request.set(R::UUID, resumed.uuid); request.set(R::FromSeqNo, 2); request.set(R::MsgCount, 1);
    request.set(R::RequestTimestamp, exchange.clock.realtime_ns()); resumed.send(request);
    const auto replayed = frames(resumed.memory); REQUIRE_EQ(replayed.size(), 2u);
    CHECK_EQ(replayed[0].id(), 509); REQUIRE_EQ(replayed[1].id(), 525);
    CHECK_EQ(wire::get(replayed[1].view(), schema::ExecutionReportTradeOutright525::PossRetransFlag), 1);
    CHECK_EQ(wire::get(replayed[1].view(), schema::ExecutionReportTradeOutright525::SeqNum), 2u);
    protocol::cme::MessageView original;
    REQUIRE_EQ(protocol::cme::decode_ilink(exchange.user(1).store.get(2).bytes, original), protocol::Status::ok);
    CHECK_EQ(wire::get(original, schema::ExecutionReportTradeOutright525::PossRetransFlag), 0);
    resumed.connection.on_closed(exchange.core);
    Client next(exchange, 1, 102); next.uuid += 100; next.login();
    request.set(R::UUID, next.uuid); request.set(R::LastUUID, alice.uuid); next.send(request);
    const auto previous = frames(next.memory); REQUIRE_EQ(previous.size(), 2u); REQUIRE_EQ(previous[1].id(), 525);
    CHECK_EQ(wire::get(previous[1].view(), schema::ExecutionReportTradeOutright525::UUID), alice.uuid);
    CHECK_EQ(exchange.user(1).store.last_seq(), 0u);
    next.memory.clear(); request.set(R::FromSeqNo, 99); next.send(request);
    CHECK_EQ(frames(next.memory)[0].id(), 510);
}

TEST_CASE(ilink_session, cancel_on_disconnect_store_overflow_and_keepalive) {
    auto config = ilink_test::config(); config.users[0].cancel_on_disconnect = true;
    config.engine.store_messages_per_session = 2;
    Exchange exchange(config); Client alice(exchange, 1); alice.login();
    alice.send(order(alice.user_config(), 1, 1, "ONE"));
    alice.send(order(alice.user_config(), 2, 2, "TWO"));
    alice.send(order(alice.user_config(), 3, 3, "THREE"));
    CHECK(alice.connection.wants_close()); CHECK(!alice.connection.flush_before_close());
    alice.connection.on_closed(exchange.core); CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
    CHECK_EQ(exchange.core.bound_connection_count(), 0u);
    Client bob(exchange, 2); bob.login();
    exchange.clock.day += 5000000000; bob.connection.on_timer(exchange.core);
    const auto warning = frames(bob.memory); REQUIRE_EQ(warning[0].id(), 506);
    CHECK_EQ(wire::get(warning[0].view(), schema::Sequence506::KeepAliveIntervalLapsed), 1);
    bob.memory.clear(); exchange.clock.day += 5000000000; bob.connection.on_timer(exchange.core);
    CHECK_EQ(frames(bob.memory)[0].id(), 507); CHECK(bob.connection.wants_close());
}

TEST_CASE(ilink_orders, mass_cancel_fragments_list_every_affected_order) {
    Exchange exchange; Client alice(exchange, 1); alice.login();
    for (std::uint32_t i = 1; i <= 40; ++i) alice.send(order(alice.user_config(), i, i, "ORDER" + std::to_string(i)));
    alice.memory.clear(); using M = schema::OrderMassActionRequest529;
    wire::Root<M> request; request_fields(request, alice.user_config(), 41, 41); request.set(M::MassActionScope, 7); alice.send(request);
    const auto output = frames(alice.memory);
    std::size_t count = 0, fragments = 0;
    std::uint64_t report_id = 0;
    for (const auto& frame : output) if (frame.id() == 562) {
        const auto view = frame.view();
        if (!report_id) report_id = wire::get(view, schema::OrderMassActionReport562::MassActionReportID);
        CHECK_EQ(wire::get(view, schema::OrderMassActionReport562::MassActionReportID), report_id);
        protocol::cme::GroupView group;
        REQUIRE_EQ(protocol::cme::decode_group(view.trailing(), 32, group), protocol::Status::ok);
        count += group.count; ++fragments;
    }
    CHECK_EQ(count, 40u); CHECK_EQ(fragments, 2u);
    CHECK_EQ(wire::get(output.back().view(), schema::OrderMassActionReport562::LastFragment), 1);
    CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
}

TEST_CASE(ilink_orders, steady_state_order_path_does_not_allocate) {
    if (!testfw::allocation_counting_available()) return;
    Exchange exchange; Client alice(exchange, 1); alice.login();
    const auto bytes = encode(order(alice.user_config(), 1, 1, "NO-ALLOCATION"));
    std::size_t allocations = 0, consumed = 0;
    { testfw::AllocationScope scope;
      consumed = alice.connection.on_data(bytes, exchange.core); allocations = scope.count(); }
    CHECK_EQ(consumed, bytes.size()); CHECK_EQ(allocations, 0u);
}
