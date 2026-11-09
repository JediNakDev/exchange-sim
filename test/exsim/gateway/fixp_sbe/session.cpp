#include "fixp_sbe_test_support.hpp"
#include <test/framework/alloc_counter.hpp>
using namespace fixp_sbe_test;

TEST_CASE(fixp_sbe, order_lifecycle_with_real_engine) {
    EngineTestContext context;
    auto instrument=context.add_instrument("AAPL");
    context.add_user("alice","secret",1);
    ServerConnectionHarness server(context);
    server.log_in();
    server.send(limit_order(1,sbe::SideCode::Buy,10000,100));
    auto frames=server.take_output();
    REQUIRE_EQ(count_frames(frames,sbe::TemplateId::ExecutionReportNew),1u);
    CHECK_EQ(frames[0].as<sbe::ExecutionReportNew>().leaves_qty,100u);
    context.submit_resting_order(instrument,2,1,exsim::Side::SELL,10000,30);
    frames=server.take_output();
    REQUIRE_EQ(count_frames(frames,sbe::TemplateId::ExecutionReportTrade),1u);
    auto trade=frames[0].as<sbe::ExecutionReportTrade>();
    CHECK_EQ(trade.last_qty,30u); CHECK_EQ(trade.cum_qty,30u); CHECK_EQ(trade.leaves_qty,70u);
    CHECK(trade.last_liquidity_ind==sbe::LastLiquidityIndicator::AddedLiquidity);
    CHECK(trade.trd_match_id!=0);
    server.send(cancel_request(2,1,80));
    frames=server.take_output();
    REQUIRE_EQ(count_frames(frames,sbe::TemplateId::ExecutionReportCancel),1u);
    auto canceled=frames[0].as<sbe::ExecutionReportCancel>();
    CHECK_EQ(canceled.leaves_qty,50u); CHECK_EQ(canceled.cum_qty,30u);
    server.send(replace_request(3,1,10000,70));
    frames=server.take_output();
    REQUIRE_EQ(count_frames(frames,sbe::TemplateId::ExecutionReportReplace),1u);
    auto replaced=frames[0].as<sbe::ExecutionReportReplace>();
    CHECK_EQ(replaced.leaves_qty,40u); CHECK_EQ(replaced.cum_qty,30u);
    CHECK(replaced.priority_retained==sbe::BooleanCode::True);
    server.send(replace_request(4,3,10000,70,true));
    frames=server.take_output();
    REQUIRE_EQ(count_frames(frames,sbe::TemplateId::ExecutionReportReplace),1u);
    CHECK(frames[0].as<sbe::ExecutionReportReplace>().priority_retained==sbe::BooleanCode::False);
    sbe::OrderMassCancelRequest mass; mass.cl_ord_id=5;
    server.send(mass); frames=server.take_output();
    CHECK_EQ(count_frames(frames,sbe::TemplateId::ExecutionReportCancel),1u);
    CHECK_EQ(count_frames(frames,sbe::TemplateId::OrderMassCancelReport),1u);
    CHECK_EQ(context.insufficient_output_results,0u);
}
TEST_CASE(fixp_sbe, rejects_and_idempotent_gaps) {
    EngineTestContext context; context.add_instrument("AAPL");
    auto& user=context.add_user("alice","secret",1);
    ServerConnectionHarness server(context); server.log_in();
    server.send(limit_order(1,sbe::SideCode::Buy,10000,100)); static_cast<void>(server.take_output());
    auto before=context.submitted_commands;
    server.sequence(1); server.send(limit_order(1,sbe::SideCode::Buy,10000,100));
    CHECK_EQ(context.submitted_commands,before);
    CHECK_EQ(user.statistics().duplicate_client_messages_dropped,1u);
    server.sequence(5); auto frames=server.take_output();
    REQUIRE_EQ(count_frames(frames,sbe::TemplateId::NotApplied),1u);
    auto gap=frames[0].as<sbe::NotApplied>(); CHECK_EQ(gap.from_seq_no,2u); CHECK_EQ(gap.count,3u);
    server.send(limit_order(1,sbe::SideCode::Buy,10000,100));
    frames=server.take_output(); CHECK_EQ(count_frames(frames,sbe::TemplateId::ExecutionReportReject),1u);
    server.send(cancel_request(2,999)); frames=server.take_output();
    CHECK_EQ(count_frames(frames,sbe::TemplateId::OrderCancelReject),1u);
    server.send(limit_order(3,sbe::SideCode::Buy,10000,100,"MISSING")); frames=server.take_output();
    CHECK_EQ(count_frames(frames,sbe::TemplateId::ExecutionReportReject),1u);
}
TEST_CASE(fixp_sbe, authentication_timeout_and_terminate) {
    EngineTestContext context; context.add_user("alice","secret",1);
    { ServerConnectionHarness server(context); server.negotiate(kFirstSessionUuid,"alice:wrong");
      CHECK(server.connection().close_requested());
      CHECK_EQ(count_frames(server.take_output(),sbe::TemplateId::NegotiationReject),1u); }
    { ServerConnectionHarness server(context); server.log_in();
      context.advance_ms(2100); server.connection().on_timer(context);
      CHECK(server.connection().close_reason()==fixp::FixpSbeCloseReason::KeepaliveExpired); }
    { ServerConnectionHarness server(context); server.establish(kFirstSessionUuid,1000,"alice:secret");
      REQUIRE(server.connection().state()==fixp::FixpSbeConnectionState::Established);
      sbe::Terminate terminate; terminate.session_id=kFirstSessionUuid; server.send(terminate);
      CHECK(server.connection().close_reason()==fixp::FixpSbeCloseReason::PeerTerminated); }
}
TEST_CASE(fixp_sbe, reconnect_recovers_completed_order) {
    EngineTestContext context; context.add_instrument("AAPL");
    auto& user=context.add_user("alice","secret",1,0,exsim::StpMode::None,false);
    { ServerConnectionHarness server(context); server.log_in();
      server.send(limit_order(1,sbe::SideCode::Buy,10000,100));
      server.send(cancel_request(2,1)); }
    CHECK_EQ(user.last_server_sequence_number(),2u);
    ServerConnectionHarness server(context);
    server.establish(kFirstSessionUuid,1000,"alice:secret"); static_cast<void>(server.take_output());
    sbe::RetransmitRequest request; request.session_id=kFirstSessionUuid;
    request.timestamp_ns=context.realtime_ns(); request.from_seq_no=1; request.count=2;
    server.send(request); auto frames=server.take_output();
    REQUIRE_EQ(frames.size(),4u);
    CHECK(frames[0].is(sbe::TemplateId::Retransmission));
    CHECK(frames[1].is(sbe::TemplateId::ExecutionReportNew));
    CHECK(frames[2].is(sbe::TemplateId::ExecutionReportCancel));
    CHECK_EQ(frames[2].as<sbe::ExecutionReportCancel>().leaves_qty,0u);
    CHECK(frames[3].is(sbe::TemplateId::Sequence));
}
struct CountingHandler {
    unsigned delivered=0;
    template<class Message> void on_application_message(const fixp::FixpSbeReceivedMessageInfo&,const Message&) noexcept {
        ++delivered;
    }
};
TEST_CASE(fixp_sbe, client_recovery_and_zero_allocation) {
    EngineTestContext context; context.add_instrument("AAPL");
    context.add_user("alice","secret",1,0,exsim::StpMode::None,false);
    ServerConnectionHarness server(context);
    MemoryBuffer outbound; CountingHandler handler;
    fixp::FixpSbeClientSettings settings; settings.credentials="alice:secret";
    fixp::FixpSbeClient client(MemorySink(&outbound),handler,settings);
    auto to_server=[&] { auto consumed=server.send_bytes(outbound.bytes()); outbound.consume(consumed); };
    auto to_client=[&] { auto consumed=client.on_data(server.output().bytes(),context.realtime_ns()); server.output().consume(consumed); };
    REQUIRE(client.negotiate(kFirstSessionUuid,context.realtime_ns())); to_server(); to_client();
    REQUIRE(client.establish(context.realtime_ns())); to_server(); to_client();
    REQUIRE(client.state()==fixp::FixpSbeClientState::Established);
    context.inject_drop_outs(1,1);
    client.send(limit_order(1,sbe::SideCode::Buy,10000,100),context.realtime_ns()); to_server(); to_client();
    client.send(cancel_request(2,1),context.realtime_ns()); to_server(); to_client();
    // Sequence after the dropped report triggers recovery; then recover both reports.
    for(unsigned iteration=0; iteration<4; ++iteration) { to_server(); to_client(); }
    CHECK_EQ(handler.delivered,2u);
    CHECK_EQ(client.next_expected_server_sequence_number(),3u);
    CHECK(client.statistics().retransmitted_messages_received>=1);
    if(testfw::allocation_counting_available()) {
        testfw::AllocationScope allocations;
        for(unsigned index=0; index<100; ++index) {
            client.send(limit_order(10+index*2,sbe::SideCode::Buy,10000,100),context.realtime_ns()); to_server(); to_client();
            client.send(cancel_request(11+index*2,10+index*2),context.realtime_ns()); to_server(); to_client();
        }
        CHECK_EQ(allocations.count(),0u);
    }
    REQUIRE(client.terminate(sbe::TerminationCode::Finished,"done",context.realtime_ns())); to_server(); to_client();
    CHECK(client.state()==fixp::FixpSbeClientState::Terminated);
}

TEST_CASE(fixp_sbe, framing_unknown_template_and_backpressure) {
    EngineTestContext context; context.add_instrument("AAPL");
    auto& user=context.add_user("alice","secret",1,0,exsim::StpMode::None,false);
    ServerConnectionHarness server(context); server.log_in();
    auto bytes=encode_frame(limit_order(1,sbe::SideCode::Buy,10000,100));
    CHECK_EQ(server.send_bytes(std::span<const char>(bytes.data(),bytes.size()-1)),0u);
    CHECK_EQ(context.submitted_commands,0u);
    sbe::little_endian::store16(bytes.data()+8,999);
    server.send_bytes(bytes); auto frames=server.take_output();
    REQUIRE_EQ(count_frames(frames,sbe::TemplateId::BusinessMessageReject),1u);
    CHECK_EQ(frames[0].as<sbe::BusinessMessageReject>().ref_msg_type,999u);
    server.output().refuse_writes=true;
    server.send(limit_order(2,sbe::SideCode::Buy,10000,100));
    CHECK(server.connection().close_reason()==fixp::FixpSbeCloseReason::SendBufferFull);
    CHECK_EQ(user.last_server_sequence_number(),2u);
    CHECK(!context.is_bound(1));
}
TEST_CASE(fixp_sbe, negotiation_validation_and_duplicate_binding) {
    EngineTestContext context; context.add_user("alice","secret",1);
    { ServerConnectionHarness server(context); sbe::Negotiate request;
      request.timestamp_ns=context.realtime_ns(); request.credentials="alice:secret";
      server.send(request); auto frames=server.take_output();
      REQUIRE_EQ(count_frames(frames,sbe::TemplateId::NegotiationReject),1u); }
    ServerConnectionHarness first(context); first.log_in();
    ServerConnectionHarness second(context); second.establish(kFirstSessionUuid,1000,"alice:secret");
    auto frames=second.take_output();
    REQUIRE_EQ(count_frames(frames,sbe::TemplateId::EstablishmentReject),1u);
    CHECK(frames[0].as<sbe::EstablishmentReject>().code==sbe::EstablishmentRejectCode::AlreadyEstablished);
    CHECK(first.connection().state()==fixp::FixpSbeConnectionState::Established);
}
TEST_CASE(fixp_sbe, invalid_establish_and_handshake_deadline) {
    EngineTestContext context; context.add_user("alice","secret",1);
    { ServerConnectionHarness server(context); server.negotiate(kFirstSessionUuid); static_cast<void>(server.take_output());
      sbe::Establish request; request.session_id=kFirstSessionUuid; request.timestamp_ns=1;
      request.keepalive_interval_ms=1000; server.send(request);
      CHECK(server.connection().close_reason()==fixp::FixpSbeCloseReason::EstablishmentRejected); }
    { ServerConnectionHarness server(context); context.advance_ms(5000); server.connection().on_timer(context);
      CHECK(server.connection().close_reason()==fixp::FixpSbeCloseReason::EstablishmentTimeout); }
}
TEST_CASE(fixp_sbe, reconnect_client_and_cancel_on_disconnect) {
    EngineTestContext context; context.add_instrument("AAPL");
    auto& user=context.add_user("alice","secret",1);
    MemoryBuffer outbound; CountingHandler handler;
    fixp::FixpSbeClientSettings settings; settings.credentials="alice:secret";
    fixp::FixpSbeClient client(MemorySink(&outbound),handler,settings);
    { ServerConnectionHarness server(context);
      REQUIRE(client.negotiate(kFirstSessionUuid,context.realtime_ns())); server.send_bytes(outbound.bytes()); outbound.clear();
      client.on_data(server.output().bytes(),context.realtime_ns()); server.output().clear();
      REQUIRE(client.establish(context.realtime_ns())); server.send_bytes(outbound.bytes()); outbound.clear();
      client.on_data(server.output().bytes(),context.realtime_ns()); server.output().clear();
      client.send(limit_order(1,sbe::SideCode::Buy,10000,100),context.realtime_ns());
      server.send_bytes(outbound.bytes()); outbound.clear();
      // Lose both the New report and the disconnect cancellation.
    }
    CHECK_EQ(user.last_server_sequence_number(),3u); // New, Canceled, MassCancelReport
    client.reset_transport(MemorySink(&outbound));
    ServerConnectionHarness server(context);
    REQUIRE(client.establish(context.realtime_ns())); server.send_bytes(outbound.bytes()); outbound.clear();
    for(unsigned iteration=0; iteration<5; ++iteration) {
        client.on_data(server.output().bytes(),context.realtime_ns()); server.output().clear();
        server.send_bytes(outbound.bytes()); outbound.clear();
    }
    CHECK_EQ(handler.delivered,3u);
    CHECK_EQ(client.next_expected_server_sequence_number(),4u);
}
TEST_CASE(fixp_sbe, engine_rejects_no_effect_and_replace_to_cumulative) {
    EngineTestContext context; auto instrument=context.add_instrument("AAPL");
    context.add_user("alice","secret",1); ServerConnectionHarness server(context); server.log_in();
    server.send(limit_order(1,sbe::SideCode::Buy,10001,100));
    auto frames=server.take_output(); REQUIRE_EQ(count_frames(frames,sbe::TemplateId::ExecutionReportReject),1u);
    CHECK_EQ(frames[0].as<sbe::ExecutionReportReject>().cl_ord_id,1u);
    server.send(limit_order(2,sbe::SideCode::Buy,10000,100)); static_cast<void>(server.take_output());
    server.send(cancel_request(3,2,100)); frames=server.take_output();
    REQUIRE_EQ(count_frames(frames,sbe::TemplateId::OrderCancelReject),1u);
    context.submit_resting_order(instrument,2,1,exsim::Side::SELL,10000,30); static_cast<void>(server.take_output());
    server.send(replace_request(4,2,10000,20)); frames=server.take_output();
    REQUIRE_EQ(count_frames(frames,sbe::TemplateId::ExecutionReportCancel),1u);
    auto canceled=frames[0].as<sbe::ExecutionReportCancel>();
    CHECK_EQ(canceled.cl_ord_id,4u); CHECK_EQ(canceled.orig_cl_ord_id,2u); CHECK_EQ(canceled.leaves_qty,0u);
}
TEST_CASE(fixp_sbe, bounded_uuid_history_does_not_forget_ids) {
    EngineTestContext context; context.add_user("alice","secret",1,0,exsim::StpMode::None,false);
    for(unsigned index=0; index<64; ++index) {
        ServerConnectionHarness server(context);
        auto uuid=fixp::version4_uuid_from_random_bits(index,100);
        server.negotiate(uuid);
        REQUIRE(server.connection().state()==fixp::FixpSbeConnectionState::Negotiated);
    }
    { ServerConnectionHarness server(context);
      server.negotiate(fixp::version4_uuid_from_random_bits(65,100));
      CHECK(server.connection().close_reason()==fixp::FixpSbeCloseReason::NegotiationRejected); }
    { ServerConnectionHarness server(context);
      server.negotiate(fixp::version4_uuid_from_random_bits(0,100));
      auto frames=server.take_output(); REQUIRE_EQ(frames.size(),1u);
      CHECK(frames[0].as<sbe::NegotiationReject>().code==sbe::NegotiationRejectCode::DuplicateId); }
}
TEST_CASE(fixp_sbe, retransmit_out_of_range_and_server_heartbeat) {
    EngineTestContext context; context.add_user("alice","secret",1);
    ServerConnectionHarness server(context); server.log_in(); context.advance_ms(1000);
    server.connection().on_timer(context); auto frames=server.take_output();
    REQUIRE_EQ(count_frames(frames,sbe::TemplateId::Sequence),1u);
    CHECK_EQ(frames[0].as<sbe::Sequence>().next_seq_no,1u);
    sbe::RetransmitRequest request; request.session_id=kFirstSessionUuid;
    request.timestamp_ns=context.realtime_ns(); request.from_seq_no=1; request.count=1;
    server.send(request);
    CHECK(server.connection().close_reason()==fixp::FixpSbeCloseReason::RetransmitOutOfBounds);
}
TEST_CASE(fixp_sbe, client_rejects_wrong_handshake_echo) {
    MemoryBuffer outbound; CountingHandler handler;
    fixp::FixpSbeClientSettings settings; settings.credentials="alice:secret";
    fixp::FixpSbeClient client(MemorySink(&outbound),handler,settings);
    REQUIRE(client.negotiate(kFirstSessionUuid,EngineTestContext::kRealtimeOriginUnixNs));
    sbe::NegotiationResponse response; response.session_id=kFirstSessionUuid;
    response.request_timestamp_ns=EngineTestContext::kRealtimeOriginUnixNs+1;
    auto bytes=encode_frame(response);
    client.on_data(bytes,EngineTestContext::kRealtimeOriginUnixNs+1);
    CHECK(client.failure()==fixp::FixpSbeClientFailure::ProtocolViolation);
}
TEST_CASE(fixp_sbe, client_keepalive_timeout) {
    MemoryBuffer outbound; CountingHandler handler;
    fixp::FixpSbeClientSettings settings; settings.credentials="alice:secret";
    fixp::FixpSbeClient client(MemorySink(&outbound),handler,settings);
    constexpr auto now=EngineTestContext::kRealtimeOriginUnixNs;
    REQUIRE(client.negotiate(kFirstSessionUuid,now));
    sbe::NegotiationResponse response; response.session_id=kFirstSessionUuid; response.request_timestamp_ns=now;
    auto bytes=encode_frame(response); client.on_data(bytes,now);
    REQUIRE(client.establish(now));
    sbe::EstablishmentAck ack; ack.session_id=kFirstSessionUuid; ack.request_timestamp_ns=now;
    ack.next_seq_no=1; ack.keepalive_interval_ms=1000;
    bytes=encode_frame(ack); client.on_data(bytes,now); outbound.clear();
    client.on_timer(now+1'000'000'000ull);
    CHECK_EQ(count_frames(parse_frames(outbound.bytes()),sbe::TemplateId::Sequence),1u);
    client.on_timer(now+2'000'000'001ull);
    CHECK(client.failure()==fixp::FixpSbeClientFailure::KeepaliveExpired);
}
