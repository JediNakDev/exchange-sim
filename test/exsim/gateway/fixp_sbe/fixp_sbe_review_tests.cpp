// Independent QA review of the FIXP/SBE order entry gateway (merge 340a949).
// Reproducers for findings first (they fail on the reviewed code), then
// regression checks for behaviour the review verified as correct. Findings and
// their IDs are listed in the review report (REVIEW-SBE-*).

#include "fixp_sbe_test_support.hpp"
#include <test/framework/alloc_counter.hpp>

using namespace fixp_sbe_test;

namespace {

constexpr std::int64_t kHundredDollars = 1'000'000;  // 100.0000 with 4 implied decimals

fixp::FixpSbeUserStateSettings store_settings(std::size_t store_messages, std::size_t store_bytes,
                                              std::size_t store_reserve_messages) {
    fixp::FixpSbeUserStateSettings settings;
    settings.max_tokens = 4096;
    settings.store_messages = store_messages;
    settings.store_bytes = store_bytes;
    settings.store_reserve_messages = store_reserve_messages;
    return settings;
}

sbe::Establish establish_with_credentials(const EngineTestContext& context, std::string_view credentials) {
    sbe::Establish establish;
    establish.session_id = kFirstSessionUuid;
    establish.timestamp_ns = context.realtime_ns();
    establish.keepalive_interval_ms = 1000;
    establish.credentials = credentials;
    return establish;
}

sbe::RetransmitRequest retransmit_request(const EngineTestContext& context, std::uint64_t from_seq_no,
                                          std::uint32_t count) {
    sbe::RetransmitRequest request;
    request.session_id = kFirstSessionUuid;
    request.timestamp_ns = context.realtime_ns();
    request.from_seq_no = from_seq_no;
    request.count = count;
    return request;
}

}  // namespace

// ============================================================================
// Findings (expected to FAIL on the reviewed code)
// ============================================================================

// REVIEW-SBE-01. Passive fills cannot be refused by gateway admission.
TEST_CASE(gateway_fixp_sbe_review, establish_is_refused_after_reports_were_lost_to_store_overflow) {
    EngineTestContext context;
    const auto instrument = context.add_instrument("AAPL");
    UserState& alice = context.add_user("alice", "secret", 1, 0, exsim::StpMode::None, true,
                                       store_settings(8, 8 * 128, 1));
    {
        ServerConnectionHarness server(context);
        server.log_in();
        server.send(limit_order(1, sbe::SideCode::Buy, kHundredDollars, 100));
        REQUIRE_EQ(alice.live_order_count(), 1u);
        for (std::uint32_t id = 1; id <= 8; ++id) {
            context.submit_resting_order(instrument, 2, id, exsim::Side::SELL, kHundredDollars, 1);
        }
        REQUIRE_EQ(alice.statistics().messages_lost_to_store_overflow, 1u);
        CHECK(server.connection().close_reason() == fixp::FixpSbeCloseReason::StoreFull);
    }
    REQUIRE(alice.store_overflowed());
    REQUIRE_EQ(alice.last_server_sequence_number(), 8u);
    ServerConnectionHarness reconnect(context, 2);
    reconnect.send(establish_with_credentials(context, "alice:secret"));
    auto frames = reconnect.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    REQUIRE(frames[0].is(sbe::TemplateId::EstablishmentReject));
    CHECK(frames[0].as<sbe::EstablishmentReject>().reason == fixp::kStoreOverflowedRefusal);
    ServerConnectionHarness renegotiate(context, 3);
    renegotiate.negotiate(kSecondSessionUuid);
    frames = renegotiate.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    REQUIRE(frames[0].is(sbe::TemplateId::NegotiationReject));
    CHECK(frames[0].as<sbe::NegotiationReject>().reason == fixp::kStoreOverflowedRefusal);
}

// A large passive Trade fails while the smaller Cancel would still fit.
// Assert the retained prefix itself, even when recovery is refused.
TEST_CASE(gateway_fixp_sbe_review, report_lost_to_byte_store_overflow_is_not_hidden_behind_contiguous_sequence) {
    EngineTestContext context;
    const auto instrument = context.add_instrument("AAPL");
    constexpr auto new_bytes = sbe::kFramePrefixSize + sbe::ExecutionReportNew::kBlockLength;
    constexpr auto trade_bytes = sbe::kFramePrefixSize + sbe::ExecutionReportTrade::kBlockLength;
    constexpr auto cancel_bytes = sbe::kFramePrefixSize + sbe::ExecutionReportCancel::kBlockLength;
    static_assert(cancel_bytes < trade_bytes);
    // Enough for admission, two fills, and a cancel, but not a third fill.
    UserState& alice = context.add_user("alice", "secret", 1, 0, exsim::StpMode::None, true,
                                       store_settings(100, new_bytes + 3 * trade_bytes - 4, 0));
    {
        ServerConnectionHarness server(context);
        server.log_in();
        server.send(limit_order(1, sbe::SideCode::Buy, kHundredDollars, 100));
        for (std::uint32_t id = 1; id <= 3; ++id) {
            context.submit_resting_order(instrument, 2, id, exsim::Side::SELL, kHundredDollars, 10);
        }
        REQUIRE_EQ(alice.statistics().messages_lost_to_store_overflow, 1u);
    }
    CHECK_EQ(alice.last_server_sequence_number(), 3u);
    CHECK_EQ(alice.store().bytes_used(), new_bytes + 2 * trade_bytes);
    CHECK_EQ(alice.live_order_count(), 0u);
    CHECK_EQ(alice.statistics().messages_lost_to_store_overflow, 3u); // fill, cancel, mass cancel outcome
}

// REVIEW-SBE-02. The store reserve only gates new orders and replaces by a
// fixed message count; it does not grow with the user's live orders. Every
// live order needs one more report at cancel on disconnect, mass cancel or
// end of day, so a user with more live orders than the reserve overflows the
// store on a plain disconnect. With FixpSbeUserStateSettings::from the reserve
// is at most 1024 while a user may hold engine.max_orders live orders.
TEST_CASE(gateway_fixp_sbe_review, store_reserve_covers_cancel_reports_of_live_orders) {
    EngineTestContext context;
    context.add_instrument("AAPL");
    UserState& alice = context.add_user("alice", "secret", 1, 0, exsim::StpMode::None, /*cancel_on_disconnect=*/true,
                                        store_settings(40, 40 * 128, 4));
    {
        ServerConnectionHarness server(context);
        server.log_in();
        std::uint64_t cl_ord_id = 1;
        // Place orders until the gateway refuses them for store reserve.
        for (; cl_ord_id <= 40; ++cl_ord_id) {
            server.send(limit_order(cl_ord_id, sbe::SideCode::Buy, kHundredDollars, 10));
            const std::vector<ParsedFrame> frames = server.take_output();
            REQUIRE_EQ(frames.size(), 1u);
            if (frames[0].is(sbe::TemplateId::ExecutionReportReject)) {
                CHECK(frames[0].as<sbe::ExecutionReportReject>().exsim_reject_reason ==
                      sbe::ExsimRejectReasonCode::OutboundStoreFull);
                break;
            }
        }
        REQUIRE(context.engine().live_order_count() > 4u);
    }  // cancel on disconnect for every live order
    CHECK_EQ(alice.statistics().messages_lost_to_store_overflow, 0u);
    CHECK_EQ(context.engine().live_order_count(), 0u);
}

// REVIEW-SBE-03. FIXP 1.0 "Terminate Response": upon receiving a Terminate
// message, the receiver must respond with a Terminate message. The server
// answers only in the Established state; a Terminate received after a
// successful Negotiate (a FIXP session exists) closes the transport silently.
TEST_CASE(gateway_fixp_sbe_review, terminate_received_while_negotiated_is_answered_with_terminate) {
    EngineTestContext context;
    context.add_user("alice", "secret", 1);
    ServerConnectionHarness server(context);
    server.negotiate(kFirstSessionUuid);
    std::vector<ParsedFrame> frames = server.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    REQUIRE(frames[0].is(sbe::TemplateId::NegotiationResponse));
    REQUIRE(server.connection().state() == fixp::FixpSbeConnectionState::Negotiated);

    sbe::Terminate terminate;
    terminate.session_id = kFirstSessionUuid;
    terminate.code = sbe::TerminationCode::Finished;
    server.send(terminate);
    CHECK(server.connection().close_reason() == fixp::FixpSbeCloseReason::PeerTerminated);
    frames = server.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0].is(sbe::TemplateId::Terminate));
}

TEST_CASE(gateway_fixp_sbe_review, shutdown_while_negotiated_sends_terminate) {
    EngineTestContext context;
    context.add_user("alice", "secret", 1);
    ServerConnectionHarness server(context);
    server.negotiate(kFirstSessionUuid);
    (void)server.take_output();
    server.connection().end_session(context);
    auto frames = server.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    REQUIRE(frames[0].is(sbe::TemplateId::Terminate));
    CHECK(frames[0].as<sbe::Terminate>().code == sbe::TerminationCode::Finished);
    CHECK(server.connection().close_requested());
    CHECK(server.connection().flush_before_close());
}

// REVIEW-SBE-05: a failed response must leave the earlier flow recoverable.
TEST_CASE(gateway_fixp_sbe_review, failed_negotiation_response_does_not_burn_uuid_or_supersede_flow) {
    EngineTestContext context;
    context.add_instrument("AAPL");
    auto& alice = context.add_user("alice", "secret", 1);
    {
        ServerConnectionHarness server(context);
        server.log_in();
        server.send(limit_order(1, sbe::SideCode::Buy, kHundredDollars, 10));
    }
    const auto last = alice.last_server_sequence_number();
    {
        ServerConnectionHarness failed(context, 2);
        failed.output().refuse_writes = true;
        failed.negotiate(kSecondSessionUuid);
        CHECK(failed.connection().close_reason() == fixp::FixpSbeCloseReason::SendBufferFull);
    }
    CHECK(alice.logical_session_uuid() == kFirstSessionUuid);
    CHECK_EQ(alice.last_server_sequence_number(), last);
    ServerConnectionHarness retry(context, 3);
    retry.negotiate(kSecondSessionUuid);
    const auto frames = retry.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0].is(sbe::TemplateId::NegotiationResponse));
}

// REVIEW-SBE-06: reject a redundant Establish while continuing the same flow.
TEST_CASE(gateway_fixp_sbe_review, redundant_establish_keeps_the_established_flow) {
    EngineTestContext context;
    context.add_instrument("AAPL");
    auto& alice = context.add_user("alice", "secret", 1);
    ServerConnectionHarness server(context);
    server.log_in();
    server.establish(kFirstSessionUuid);
    auto frames = server.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    REQUIRE(frames[0].is(sbe::TemplateId::EstablishmentReject));
    CHECK(frames[0].as<sbe::EstablishmentReject>().code == sbe::EstablishmentRejectCode::AlreadyEstablished);
    REQUIRE(server.connection().state() == fixp::FixpSbeConnectionState::Established);
    server.send(limit_order(1, sbe::SideCode::Buy, kHundredDollars, 10));
    CHECK_EQ(alice.last_server_sequence_number(), 1u);
    CHECK_EQ(alice.live_order_count(), 1u);
}

// Repeated rejects and forward gaps must not consume the terminal reserve.
TEST_CASE(gateway_fixp_sbe_review, repeated_requests_preserve_terminal_store_reserve) {
    for (const bool gap : {false, true}) {
        EngineTestContext context;
        context.add_instrument("AAPL");
        auto& alice = context.add_user("alice", "secret", 1, 0, exsim::StpMode::None, true,
                                      store_settings(12, 12 * 128, 1));
        {
            ServerConnectionHarness server(context);
            server.log_in();
            server.send(limit_order(1, sbe::SideCode::Buy, kHundredDollars, 10));
            for (std::uint32_t id = 2; id < 30 && !server.connection().close_requested(); ++id) {
                const auto expected = alice.next_expected_client_sequence_number();
                const auto commands = context.submitted_commands;
                if (gap) server.sequence(2 * id);
                else server.send(limit_order(id, sbe::SideCode::Buy, kHundredDollars, 10));
                if (server.connection().close_requested()) {
                    CHECK_EQ(alice.next_expected_client_sequence_number(), expected);
                    // Closing the transport submits only cancel-on-disconnect.
                    CHECK_EQ(context.submitted_commands, commands + 1);
                    CHECK_EQ(alice.live_order_count(), 0u);
                }
            }
            CHECK(server.connection().close_reason() == fixp::FixpSbeCloseReason::StoreReserveReached);
            CHECK_EQ(alice.statistics().requests_refused_at_store_reserve, 1u);
        }
        CHECK_EQ(alice.live_order_count(), 0u);
        CHECK_EQ(alice.statistics().messages_lost_to_store_overflow, 0u);
        CHECK(!alice.store_overflowed());
    }
}

TEST_CASE(gateway_fixp_sbe_review, byte_budget_reserves_terminal_reports_at_shutdown) {
    EngineTestContext context;
    context.add_instrument("AAPL");
    auto& alice = context.add_user("alice", "secret", 1, 0, exsim::StpMode::None, false,
                                  store_settings(1000, 12 * fixp::kMaxApplicationFrameSize, 1));
    ServerConnectionHarness server(context);
    server.log_in();
    for (std::uint32_t id = 1; id < 30 && !server.connection().close_requested(); ++id) {
        server.send(limit_order(id, sbe::SideCode::Buy, kHundredDollars, 10));
    }
    REQUIRE(alice.live_order_count() > 0u);
    exsim::SetPhase closed;
    closed.phase = exsim::Phase::Closed;
    context.submit(exsim::Command(closed));
    CHECK_EQ(alice.live_order_count(), 0u);
    CHECK_EQ(alice.statistics().messages_lost_to_store_overflow, 0u);
}

// ============================================================================
// Verified behaviour (regression checks, expected to PASS)
// ============================================================================

// FIXP_SBE.md: "A larger blockLength is skipped" also for version 1 frames.
TEST_CASE(gateway_fixp_sbe_review, version1_frame_with_longer_root_block_is_accepted) {
    EngineTestContext context;
    context.add_instrument("AAPL");
    context.add_user("alice", "secret", 1);
    ServerConnectionHarness server(context);
    server.log_in();
    const std::vector<char> plain = encode_frame(limit_order(1, sbe::SideCode::Buy, kHundredDollars, 10));
    std::vector<char> extended(plain);
    extended.insert(extended.end(), {'\x7F', '\x7F', '\x7F', '\x7F'});
    exsim::protocol::wire::store32(extended.data(), static_cast<std::uint32_t>(extended.size()));
    sbe::little_endian::store16(extended.data() + sbe::kSofhSize, sbe::NewOrderSingle::kBlockLength + 4);
    CHECK_EQ(server.send_bytes(extended), extended.size());
    const std::vector<ParsedFrame> frames = server.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0].is(sbe::TemplateId::ExecutionReportNew));
}

// Garbage after establishment: wrong SOFH encoding type terminates with Terminate.
TEST_CASE(gateway_fixp_sbe_review, wrong_sofh_encoding_type_terminates_established_session) {
    EngineTestContext context;
    context.add_instrument("AAPL");
    context.add_user("alice", "secret", 1, 0, exsim::StpMode::None, /*cancel_on_disconnect=*/false);
    ServerConnectionHarness server(context);
    server.log_in();
    std::vector<char> frame = encode_frame(limit_order(1, sbe::SideCode::Buy, kHundredDollars, 10));
    exsim::protocol::wire::store16(frame.data() + 4, 0x5BE0);  // SBE big-endian encoding type
    CHECK_EQ(server.send_bytes(frame), frame.size());
    CHECK(server.connection().close_reason() == fixp::FixpSbeCloseReason::ProtocolViolation);
    const std::vector<ParsedFrame> frames = server.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0].is(sbe::TemplateId::Terminate));
    CHECK_EQ(context.submitted_commands, 0u);
}

// Unknown session template (FinishedSending, 15) terminates the transport.
TEST_CASE(gateway_fixp_sbe_review, unsupported_session_template_terminates) {
    EngineTestContext context;
    context.add_user("alice", "secret", 1);
    ServerConnectionHarness server(context);
    server.log_in();
    std::vector<char> frame = encode_frame(sbe::UnsequencedHeartbeat{});
    sbe::little_endian::store16(frame.data() + sbe::kSofhSize + 2, 15);
    server.send_bytes(frame);
    CHECK(server.connection().close_reason() == fixp::FixpSbeCloseReason::ProtocolViolation);
    const std::vector<ParsedFrame> frames = server.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0].is(sbe::TemplateId::Terminate));
}

// Retransmission is capped at the batch size and real time resumes with Sequence.
TEST_CASE(gateway_fixp_sbe_review, retransmission_is_capped_and_followed_by_sequence) {
    EngineTestContext context;
    context.add_instrument("AAPL");
    context.add_user("alice", "secret", 1);
    fixp::FixpSbeSessionSettings settings;
    settings.maximum_retransmission_batch = 2;
    ServerConnectionHarness server(context, 1, settings);
    server.log_in();
    for (std::uint64_t cl_ord_id = 1; cl_ord_id <= 5; ++cl_ord_id) {
        server.send(limit_order(cl_ord_id, sbe::SideCode::Buy, kHundredDollars, 10));
    }
    static_cast<void>(server.take_output());
    server.send(retransmit_request(context, 2, 4));
    const std::vector<ParsedFrame> frames = server.take_output();
    REQUIRE_EQ(frames.size(), 4u);
    const sbe::Retransmission retransmission = frames[0].as<sbe::Retransmission>();
    CHECK_EQ(retransmission.next_seq_no, 2u);
    CHECK_EQ(retransmission.count, 2u);
    CHECK_EQ(frames[1].as<sbe::ExecutionReportNew>().cl_ord_id, 2u);
    CHECK_EQ(frames[2].as<sbe::ExecutionReportNew>().cl_ord_id, 3u);
    CHECK_EQ(frames[3].as<sbe::Sequence>().next_seq_no, 6u);
    CHECK(server.connection().state() == fixp::FixpSbeConnectionState::Established);
}

// NotApplied consumes a server sequence number and is recoverable; duplicates
// below the high-water mark stay discarded after a transport reconnect.
TEST_CASE(gateway_fixp_sbe_review, not_applied_is_sequenced_and_high_water_survives_reconnect) {
    EngineTestContext context;
    context.add_instrument("AAPL");
    UserState& alice = context.add_user("alice", "secret", 1, 0, exsim::StpMode::None, /*cancel_on_disconnect=*/false);
    {
        ServerConnectionHarness server(context);
        server.log_in();
        server.send(limit_order(1, sbe::SideCode::Buy, kHundredDollars, 10));  // client seq 1
        server.sequence(4);                                                    // 2 and 3 never arrived
        const std::vector<ParsedFrame> frames = server.take_output();
        REQUIRE_EQ(frames.size(), 2u);
        const sbe::NotApplied not_applied = frames[1].as<sbe::NotApplied>();
        CHECK_EQ(not_applied.from_seq_no, 2u);
        CHECK_EQ(not_applied.count, 2u);
        CHECK_EQ(alice.last_server_sequence_number(), 2u);
    }
    ServerConnectionHarness reconnect(context, 2);
    reconnect.send(establish_with_credentials(context, "alice:secret"));
    reconnect.sequence(1);
    const std::uint64_t commands_before = context.submitted_commands;
    reconnect.send(limit_order(9, sbe::SideCode::Buy, kHundredDollars, 10));  // client seq 1 again: duplicate
    CHECK_EQ(context.submitted_commands, commands_before);
    static_cast<void>(reconnect.take_output());
    reconnect.send(retransmit_request(context, 2, 1));
    const std::vector<ParsedFrame> frames = reconnect.take_output();
    REQUIRE_EQ(frames.size(), 3u);
    CHECK(frames[1].is(sbe::TemplateId::NotApplied));
}

// FIXP_SBE.md: "Replace acknowledgment precedes its fills."
TEST_CASE(gateway_fixp_sbe_review, replace_acknowledgment_precedes_its_fills) {
    EngineTestContext context;
    const exsim::InstrumentId instrument = context.add_instrument("AAPL");
    context.add_user("alice", "secret", 1);
    ServerConnectionHarness server(context);
    server.log_in();
    server.send(limit_order(1, sbe::SideCode::Buy, kHundredDollars, 10));
    context.submit_resting_order(instrument, 2, 1, exsim::Side::SELL, kHundredDollars + 100, 4);
    static_cast<void>(server.take_output());
    server.send(replace_request(2, 1, kHundredDollars + 100, 10));
    const std::vector<ParsedFrame> frames = server.take_output();
    REQUIRE_EQ(frames.size(), 2u);
    CHECK(frames[0].is(sbe::TemplateId::ExecutionReportReplace));
    const sbe::ExecutionReportTrade trade = frames[1].as<sbe::ExecutionReportTrade>();
    CHECK_EQ(trade.cl_ord_id, 2u);
    CHECK_EQ(trade.last_qty, 4u);
    CHECK(trade.last_liquidity_ind == sbe::LastLiquidityIndicator::RemovedLiquidity);
}

// IOC remainder: Trade (PartiallyFilled) then Cancel with ImmediateOrCancel.
TEST_CASE(gateway_fixp_sbe_review, ioc_remainder_is_reported_after_its_fill) {
    EngineTestContext context;
    const exsim::InstrumentId instrument = context.add_instrument("AAPL");
    context.add_user("alice", "secret", 1);
    ServerConnectionHarness server(context);
    server.log_in();
    context.submit_resting_order(instrument, 2, 1, exsim::Side::SELL, kHundredDollars, 4);
    sbe::NewOrderSingle order = limit_order(1, sbe::SideCode::Buy, kHundredDollars, 10);
    order.time_in_force = sbe::TimeInForceCode::ImmediateOrCancel;
    server.send(order);
    const std::vector<ParsedFrame> frames = server.take_output();
    REQUIRE_EQ(frames.size(), 3u);
    CHECK(frames[0].is(sbe::TemplateId::ExecutionReportNew));
    const sbe::ExecutionReportTrade trade = frames[1].as<sbe::ExecutionReportTrade>();
    CHECK(trade.ord_status == sbe::OrdStatusCode::PartiallyFilled);
    CHECK_EQ(trade.leaves_qty, 6u);
    const sbe::ExecutionReportCancel cancel = frames[2].as<sbe::ExecutionReportCancel>();
    CHECK(cancel.cancel_reason == sbe::ExsimCancelReasonCode::ImmediateOrCancel);
    CHECK_EQ(cancel.canceled_qty, 6u);
    CHECK_EQ(cancel.cum_qty, 4u);
    CHECK(cancel.ord_status == sbe::OrdStatusCode::Canceled);
}

// Mass cancel for an unknown security never becomes "cancel all".
TEST_CASE(gateway_fixp_sbe_review, mass_cancel_for_unknown_security_is_rejected_by_gateway) {
    EngineTestContext context;
    context.add_instrument("AAPL");
    context.add_user("alice", "secret", 1, 0, exsim::StpMode::None, /*cancel_on_disconnect=*/false);
    ServerConnectionHarness server(context);
    server.log_in();
    server.send(limit_order(1, sbe::SideCode::Buy, kHundredDollars, 10));
    static_cast<void>(server.take_output());
    sbe::OrderMassCancelRequest request;
    request.cl_ord_id = 2;
    request.symbol = wire_symbol("MSFT");
    request.mass_cancel_request_type = sbe::MassCancelRequestTypeCode::CancelOrdersForSecurity;
    server.send(request);
    const std::vector<ParsedFrame> frames = server.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    const sbe::OrderMassCancelReport report = frames[0].as<sbe::OrderMassCancelReport>();
    CHECK(report.mass_cancel_response == sbe::MassCancelResponseCode::CancelRequestRejected);
    CHECK(report.mass_cancel_reject_reason == sbe::MassCancelRejectReasonCode::InvalidOrUnknownSecurity);
    CHECK_EQ(context.engine().live_order_count(), 1u);
}

#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if !__has_feature(address_sanitizer) && !__has_feature(thread_sanitizer)
// Paths the merged allocation test does not cover: replace, mass cancel,
// gateway reject, NotApplied, retransmission, BusinessMessageReject, keepalive.
TEST_CASE(gateway_fixp_sbe_review, replace_mass_cancel_reject_gap_and_retransmit_allocate_nothing) {
    if (!testfw::allocation_counting_available()) return;
    EngineTestContext context;
    context.add_instrument("AAPL");
    UserState& alice = context.add_user("alice", "secret", 1);
    ServerConnectionHarness server(context);
    server.log_in();

    std::vector<char> new_order = encode_frame(limit_order(1, sbe::SideCode::Buy, kHundredDollars, 100));
    std::vector<char> replace = encode_frame(replace_request(2, 1, kHundredDollars, 90));
    sbe::OrderMassCancelRequest mass_cancel_request;
    mass_cancel_request.cl_ord_id = 3;
    std::vector<char> mass_cancel = encode_frame(mass_cancel_request);
    std::vector<char> unknown_symbol = encode_frame(limit_order(4, sbe::SideCode::Buy, kHundredDollars, 1, "MISSING"));
    sbe::Sequence gap_sequence;
    gap_sequence.next_seq_no = 1;
    std::vector<char> gap = encode_frame(gap_sequence);
    std::vector<char> retransmit = encode_frame(retransmit_request(context, 1, 1));
    std::vector<char> unknown_template = encode_frame(limit_order(5, sbe::SideCode::Buy, kHundredDollars, 1));
    sbe::little_endian::store16(unknown_template.data() + sbe::kSofhSize + 2, 150);
    constexpr std::size_t kClOrdIdOffset = sbe::kFramePrefixSize;
    constexpr std::size_t kOrigClOrdIdOffset = sbe::kFramePrefixSize + 8;

    const auto run_iteration = [&](std::uint64_t iteration) {
        const std::uint64_t base = 1 + iteration * 4;
        sbe::little_endian::store64(new_order.data() + kClOrdIdOffset, base);
        sbe::little_endian::store64(replace.data() + kClOrdIdOffset, base + 1);
        sbe::little_endian::store64(replace.data() + kOrigClOrdIdOffset, base);
        sbe::little_endian::store64(mass_cancel.data() + kClOrdIdOffset, base + 2);
        sbe::little_endian::store64(unknown_symbol.data() + kClOrdIdOffset, base + 3);
        server.send_bytes(new_order);
        server.send_bytes(replace);
        server.send_bytes(mass_cancel);
        server.send_bytes(unknown_symbol);
        sbe::little_endian::store64(gap.data() + sbe::kFramePrefixSize, alice.next_expected_client_sequence_number() + 1);
        server.send_bytes(gap);
        server.send_bytes(retransmit);
        server.send_bytes(unknown_template);
        context.advance_ms(1);
        server.connection().on_timer(context);
        server.output().clear();
    };
    for (std::uint64_t iteration = 0; iteration < 16; ++iteration) run_iteration(iteration);
    std::size_t allocations = 0;
    {
        testfw::AllocationScope scope;
        for (std::uint64_t iteration = 16; iteration < 1000; ++iteration) run_iteration(iteration);
        allocations = scope.count();
    }
    CHECK_EQ(allocations, 0u);
    CHECK(server.connection().state() == fixp::FixpSbeConnectionState::Established);
    CHECK_EQ(alice.statistics().messages_lost_to_store_overflow, 0u);
    CHECK_EQ(alice.statistics().not_applied_messages_sent, 1000u);
    CHECK_EQ(alice.statistics().business_message_rejects_sent, 1000u);
    CHECK_EQ(context.engine().live_order_count(), 0u);
}
#endif
#endif
