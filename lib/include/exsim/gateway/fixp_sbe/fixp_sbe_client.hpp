#ifndef EXSIM_GATEWAY_FIXP_SBE_FIXP_SBE_CLIENT_HPP
#define EXSIM_GATEWAY_FIXP_SBE_FIXP_SBE_CLIENT_HPP

// Client side of exsim binary order entry (server contract section 2.5,
// historical design notes): FIXP 1.0 Negotiate and Establish, an idempotent
// client flow with implicit sequence numbers, Sequence keepalives, gap
// detection on the recoverable server flow with one RetransmitRequest in
// flight at a time, and typed callbacks. A pure state machine over a byte
// sink; the caller owns the socket and the clock (Unix epoch nanoseconds).
// Nothing allocates after construction.
//
// Handler callbacks are all optional; the client calls those the handler
// declares:
//   on_negotiation_response(const sbe::NegotiationResponse&)
//   on_negotiation_reject(const sbe::NegotiationReject&)
//   on_establishment_ack(const sbe::EstablishmentAck&)
//   on_establishment_reject(const sbe::EstablishmentReject&)
//   on_retransmit_reject(const sbe::RetransmitReject&)
//   on_terminate(const sbe::Terminate&)
//   on_application_message(const FixpSbeReceivedMessageInfo&, const M&)
//       for M in ExecutionReportNew/Trade/Cancel/Replace/Reject,
//       OrderCancelReject, OrderMassCancelReport, BusinessMessageReject and
//       NotApplied (sequenced on the server flow)
//   on_unknown_application_message(const FixpSbeReceivedMessageInfo&, const sbe::FrameView&)
// Server flow messages are delivered exactly once and in sequence order;
// messages received beyond a gap are dropped and recovered by retransmission.

#include "exsim/gateway/fixp_sbe/byte_sink.hpp"
#include "exsim/protocol/sbe_order_entry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace exsim::gateway::fixp_sbe {

namespace sbe = protocol::sbe_order_entry;

// RFC 4122 version 4 UUID from 128 caller-supplied random bits.
[[nodiscard]] constexpr sbe::SessionUuid version4_uuid_from_random_bits(std::uint64_t high_bits,
                                                                        std::uint64_t low_bits) noexcept {
    sbe::SessionUuid uuid{};
    for (std::size_t index = 0; index < 8; ++index) {
        uuid[index] = static_cast<std::uint8_t>(high_bits >> (56 - 8 * index));
        uuid[8 + index] = static_cast<std::uint8_t>(low_bits >> (56 - 8 * index));
    }
    uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0F) | 0x40);  // version 4
    uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3F) | 0x80);  // RFC 4122 variant
    return uuid;
}

struct FixpSbeClientSettings {
    std::string_view credentials;  // "username:password"; the storage must outlive the client
    std::uint32_t keepalive_interval_ms = 1000;
    // Server silence longer than multiplier x the server's keepalive interval
    // ends the session (FIXP 4.4 leniency).
    std::uint32_t keepalive_expiry_multiplier = 2;
    // Largest Count the client asks for in one RetransmitRequest.
    std::uint32_t maximum_retransmit_request_count = 1000;
};

enum class FixpSbeClientState : std::uint8_t {
    Disconnected,                 // no transport, or a fresh one before Negotiate/Establish
    AwaitingNegotiationResponse,
    Negotiated,
    AwaitingEstablishmentAck,
    Established,
    TerminateSent,                // waiting for the server's Terminate
    Terminated,                   // the transport should be closed
};

enum class FixpSbeClientFailure : std::uint8_t {
    None,
    NegotiationRejected,
    EstablishmentRejected,
    ServerTerminated,   // the server sent Terminate first
    ProtocolViolation,  // malformed or unexpected frame from the server
    SequenceRegression, // Sequence below the next expected server sequence number
    KeepaliveExpired,
    SendBufferFull,
};

struct FixpSbeReceivedMessageInfo {
    std::uint64_t server_sequence_number = 0;
    bool retransmitted = false;
};

struct FixpSbeClientStatistics {
    std::uint64_t application_messages_delivered = 0;
    std::uint64_t duplicate_server_messages_ignored = 0;
    std::uint64_t server_messages_dropped_beyond_gap = 0;
    std::uint64_t retransmit_requests_sent = 0;
    std::uint64_t retransmitted_messages_received = 0;
};

template <FixpSbeByteSink ByteSink, class Handler>
class FixpSbeClient {
public:
    FixpSbeClient(ByteSink sink, Handler& handler, const FixpSbeClientSettings& settings) noexcept
        : sink_(std::move(sink)), handler_(handler), settings_(settings) {}

    FixpSbeClient(const FixpSbeClient&) = delete;
    FixpSbeClient& operator=(const FixpSbeClient&) = delete;

    // Starts a new logical session on the current transport; both flows
    // restart at sequence number 1 once the server accepts it (FIXP 4.2).
    bool negotiate(const sbe::SessionUuid& session_uuid, std::uint64_t now_unix_ns) noexcept {
        if (state_ != FixpSbeClientState::Disconnected) return false;
        session_uuid_ = session_uuid;
        sbe::Negotiate negotiate;
        negotiate.session_id = session_uuid;
        negotiate.timestamp_ns = now_unix_ns;
        negotiate.client_flow = sbe::FlowType::Idempotent;
        negotiate.credentials = settings_.credentials;
        if (!write_message(negotiate, now_unix_ns)) return false;
        negotiation_timestamp_ns_ = now_unix_ns;
        state_ = FixpSbeClientState::AwaitingNegotiationResponse;
        negotiated_on_this_transport_ = false;
        return true;
    }

    // Binds the negotiated logical session to the current transport. On a new
    // transport (reconnect) the credentials are sent again (FIXP 4.3.1).
    bool establish(std::uint64_t now_unix_ns) noexcept {
        if (state_ != FixpSbeClientState::Negotiated && state_ != FixpSbeClientState::Disconnected) return false;
        sbe::Establish establish;
        establish.session_id = session_uuid_;
        establish.timestamp_ns = now_unix_ns;
        establish.keepalive_interval_ms = settings_.keepalive_interval_ms;
        establish.next_seq_no = sbe::kNullSequenceNumber;  // the client flow is idempotent, not recoverable
        if (!negotiated_on_this_transport_) establish.credentials = settings_.credentials;
        if (!write_message(establish, now_unix_ns)) return false;
        establishment_timestamp_ns_ = now_unix_ns;
        state_ = FixpSbeClientState::AwaitingEstablishmentAck;
        return true;
    }

    // Sends one application message; returns its client sequence number, or 0
    // when not established or the sink is full. The first application message
    // on a transport is preceded by Sequence (FIXP 4.8).
    template <class Message>
    std::uint64_t send(const Message& message, std::uint64_t now_unix_ns) noexcept {
        static_assert(sbe::template_id_value(Message::kTemplateId) >= sbe::kFirstApplicationTemplateId);
        if (state_ != FixpSbeClientState::Established ||
            next_client_sequence_number_ >= sbe::kNullSequenceNumber - 1) return 0;
        if (client_sequence_message_required_) {
            sbe::Sequence sequence;
            sequence.next_seq_no = next_client_sequence_number_;
            if (!write_message(sequence, now_unix_ns)) return 0;
            client_sequence_message_required_ = false;
        }
        if (!write_message(message, now_unix_ns)) return 0;
        return next_client_sequence_number_++;
    }

    // Sends a message again under an earlier client sequence number. The
    // server drops numbers below its high-water mark, including NotApplied gaps.
    // Retry a NotApplied request with send() under a fresh number (FIXP 4.8).
    template <class Message>
    bool resend(std::uint64_t client_sequence_number, const Message& message, std::uint64_t now_unix_ns) noexcept {
        static_assert(sbe::template_id_value(Message::kTemplateId) >= sbe::kFirstApplicationTemplateId);
        if (state_ != FixpSbeClientState::Established || client_sequence_number == 0) return false;
        sbe::Sequence sequence;
        sequence.next_seq_no = client_sequence_number;
        if (!write_message(sequence, now_unix_ns) || !write_message(message, now_unix_ns)) return false;
        client_sequence_message_required_ = true;  // the next send restates its own number
        return true;
    }

    // FIXP 4.4.1: the peer answers with Terminate.
    bool terminate(sbe::TerminationCode code, std::string_view reason, std::uint64_t now_unix_ns) noexcept {
        if (state_ != FixpSbeClientState::Established && state_ != FixpSbeClientState::Negotiated) return false;
        send_terminate(code, reason, now_unix_ns);
        if (state_ != FixpSbeClientState::Terminated) state_ = FixpSbeClientState::TerminateSent;
        return true;
    }

    // Consumes whole frames; returns the bytes consumed.
    std::size_t on_data(std::span<const char> bytes, std::uint64_t now_unix_ns) noexcept {
        std::size_t consumed = 0;
        while (consumed < bytes.size() && state_ != FixpSbeClientState::Terminated) {
            sbe::FrameView view;
            const sbe::DecodeStatus status = sbe::decode_frame(bytes.data() + consumed, bytes.size() - consumed, view);
            if (status == sbe::DecodeStatus::Incomplete) break;
            last_inbound_unix_ns_ = now_unix_ns;
            if (status != sbe::DecodeStatus::Ok) {
                fail(FixpSbeClientFailure::ProtocolViolation, "malformed frame", now_unix_ns);
                break;
            }
            consumed += view.frame_size;
            handle_frame(view, now_unix_ns);
        }
        return state_ == FixpSbeClientState::Terminated ? bytes.size() : consumed;
    }

    // Sequence keepalive (FIXP 4.5) and server silence detection.
    void on_timer(std::uint64_t now_unix_ns) noexcept {
        if (state_ != FixpSbeClientState::Established) return;
        const std::uint64_t server_silence_limit_ns =
            std::uint64_t{server_keepalive_interval_ms_} * 1'000'000ull * settings_.keepalive_expiry_multiplier;
        if (now_unix_ns > last_inbound_unix_ns_ && now_unix_ns - last_inbound_unix_ns_ > server_silence_limit_ns) {
            fail(FixpSbeClientFailure::KeepaliveExpired, "keepalive interval expired", now_unix_ns);
            return;
        }
        const std::uint64_t keepalive_ns = std::uint64_t{settings_.keepalive_interval_ms} * 1'000'000ull;
        if (now_unix_ns >= last_outbound_unix_ns_ && now_unix_ns - last_outbound_unix_ns_ >= keepalive_ns) {
            sbe::Sequence sequence;
            sequence.next_seq_no = next_client_sequence_number_;
            if (write_message(sequence, now_unix_ns)) client_sequence_message_required_ = false;
        }
    }

    // New transport for the same logical session (reconnect). Sequence numbers
    // of both flows are kept; call establish() next.
    void reset_transport(ByteSink sink) noexcept {
        sink_ = std::move(sink);
        state_ = FixpSbeClientState::Disconnected;
        failure_ = FixpSbeClientFailure::None;
        negotiated_on_this_transport_ = false;
        client_sequence_message_required_ = true;
        retransmit_request_in_flight_ = false;
        retransmitted_messages_remaining_ = 0;
    }

    [[nodiscard]] FixpSbeClientState state() const noexcept { return state_; }
    [[nodiscard]] FixpSbeClientFailure failure() const noexcept { return failure_; }
    [[nodiscard]] const sbe::SessionUuid& session_uuid() const noexcept { return session_uuid_; }
    [[nodiscard]] std::uint64_t next_client_sequence_number() const noexcept { return next_client_sequence_number_; }
    [[nodiscard]] std::uint64_t next_expected_server_sequence_number() const noexcept {
        return next_expected_server_sequence_number_;
    }
    [[nodiscard]] bool retransmit_request_in_flight() const noexcept { return retransmit_request_in_flight_; }
    [[nodiscard]] std::uint32_t server_keepalive_interval_ms() const noexcept { return server_keepalive_interval_ms_; }
    [[nodiscard]] const FixpSbeClientStatistics& statistics() const noexcept { return statistics_; }
    [[nodiscard]] ByteSink& sink() noexcept { return sink_; }

private:
    void handle_frame(const sbe::FrameView& view, std::uint64_t now_unix_ns) noexcept {
        const std::uint16_t template_id = view.header.template_id;
        if (template_id >= sbe::kFirstApplicationTemplateId) {
            handle_sequenced_frame(view, now_unix_ns);
            return;
        }
        switch (static_cast<sbe::TemplateId>(template_id)) {
            case sbe::TemplateId::NegotiationResponse: {
                sbe::NegotiationResponse response;
                if (!decode_or_fail(view, response, now_unix_ns)) return;
                if (state_ != FixpSbeClientState::AwaitingNegotiationResponse || response.session_id != session_uuid_ ||
                    response.request_timestamp_ns != negotiation_timestamp_ns_ ||
                    response.server_flow != sbe::FlowType::Recoverable) {
                    return fail(FixpSbeClientFailure::ProtocolViolation, "unexpected NegotiationResponse", now_unix_ns);
                }
                state_ = FixpSbeClientState::Negotiated;
                negotiated_on_this_transport_ = true;
                highest_known_server_sequence_number_ = 0;
                next_realtime_server_sequence_number_ = 1;
                next_client_sequence_number_ = 1;
                next_expected_server_sequence_number_ = 1;
                client_sequence_message_required_ = true;
                if constexpr (requires { handler_.on_negotiation_response(response); }) {
                    handler_.on_negotiation_response(response);
                }
                return;
            }
            case sbe::TemplateId::NegotiationReject: {
                sbe::NegotiationReject reject;
                if (!decode_or_fail(view, reject, now_unix_ns)) return;
                if (state_ != FixpSbeClientState::AwaitingNegotiationResponse ||
                    reject.session_id != session_uuid_ || reject.request_timestamp_ns != negotiation_timestamp_ns_) {
                    return fail(FixpSbeClientFailure::ProtocolViolation, "unexpected NegotiationReject", now_unix_ns);
                }
                state_ = FixpSbeClientState::Terminated;
                failure_ = FixpSbeClientFailure::NegotiationRejected;
                if constexpr (requires { handler_.on_negotiation_reject(reject); }) handler_.on_negotiation_reject(reject);
                return;
            }
            case sbe::TemplateId::EstablishmentAck: {
                sbe::EstablishmentAck ack;
                if (!decode_or_fail(view, ack, now_unix_ns)) return;
                if (state_ != FixpSbeClientState::AwaitingEstablishmentAck || ack.session_id != session_uuid_ ||
                    ack.next_seq_no == sbe::kNullSequenceNumber || ack.next_seq_no == 0 ||
                    ack.keepalive_interval_ms == 0 || ack.request_timestamp_ns != establishment_timestamp_ns_) {
                    return fail(FixpSbeClientFailure::ProtocolViolation, "unexpected EstablishmentAck", now_unix_ns);
                }
                state_ = FixpSbeClientState::Established;
                server_keepalive_interval_ms_ = ack.keepalive_interval_ms;
                client_sequence_message_required_ = true;
                if constexpr (requires { handler_.on_establishment_ack(ack); }) handler_.on_establishment_ack(ack);
                // FIXP 4.3.2: NextSeqNo tells the client what it missed while away.
                restate_server_sequence(ack.next_seq_no, now_unix_ns);
                return;
            }
            case sbe::TemplateId::EstablishmentReject: {
                sbe::EstablishmentReject reject;
                if (!decode_or_fail(view, reject, now_unix_ns)) return;
                if (state_ != FixpSbeClientState::AwaitingEstablishmentAck ||
                    reject.session_id != session_uuid_ || reject.request_timestamp_ns != establishment_timestamp_ns_) {
                    return fail(FixpSbeClientFailure::ProtocolViolation, "unexpected EstablishmentReject", now_unix_ns);
                }
                state_ = FixpSbeClientState::Terminated;
                failure_ = FixpSbeClientFailure::EstablishmentRejected;
                if constexpr (requires { handler_.on_establishment_reject(reject); }) handler_.on_establishment_reject(reject);
                return;
            }
            case sbe::TemplateId::Sequence: {
                sbe::Sequence sequence;
                if (!decode_or_fail(view, sequence, now_unix_ns)) return;
                if (state_ != FixpSbeClientState::Established && state_ != FixpSbeClientState::TerminateSent) {
                    return fail(FixpSbeClientFailure::ProtocolViolation, "unexpected Sequence", now_unix_ns);
                }
                if (retransmitted_messages_remaining_ != 0) {
                    return fail(FixpSbeClientFailure::ProtocolViolation, "Sequence inside a retransmission", now_unix_ns);
                }
                restate_server_sequence(sequence.next_seq_no, now_unix_ns);
                return;
            }
            case sbe::TemplateId::Retransmission: {
                sbe::Retransmission retransmission;
                if (!decode_or_fail(view, retransmission, now_unix_ns)) return;
                if (!retransmit_request_in_flight_ || retransmission.session_id != session_uuid_ ||
                    retransmission.count == 0 || retransmitted_messages_remaining_ != 0 ||
                    retransmission.request_timestamp_ns != retransmit_timestamp_ns_ ||
                    retransmission.next_seq_no != requested_from_seq_no_ ||
                    retransmission.count > requested_count_) {
                    return fail(FixpSbeClientFailure::ProtocolViolation, "unexpected Retransmission", now_unix_ns);
                }
                next_retransmitted_server_sequence_number_ = retransmission.next_seq_no;
                retransmitted_messages_remaining_ = retransmission.count;
                return;
            }
            case sbe::TemplateId::RetransmitReject: {
                sbe::RetransmitReject reject;
                if (!decode_or_fail(view, reject, now_unix_ns)) return;
                if (!retransmit_request_in_flight_ || reject.session_id != session_uuid_ ||
                    reject.request_timestamp_ns != retransmit_timestamp_ns_) {
                    return fail(FixpSbeClientFailure::ProtocolViolation, "unexpected RetransmitReject", now_unix_ns);
                }
                retransmit_request_in_flight_ = false;
                if constexpr (requires { handler_.on_retransmit_reject(reject); }) handler_.on_retransmit_reject(reject);
                return;
            }
            case sbe::TemplateId::Terminate: {
                sbe::Terminate terminate;
                if (!decode_or_fail(view, terminate, now_unix_ns)) return;
                if (terminate.session_id != session_uuid_) {
                    return fail(FixpSbeClientFailure::ProtocolViolation, "Terminate SessionId mismatch", now_unix_ns);
                }
                if (state_ != FixpSbeClientState::TerminateSent) {
                    failure_ = FixpSbeClientFailure::ServerTerminated;
                    send_terminate(sbe::TerminationCode::Finished, "terminate acknowledged", now_unix_ns);
                }
                state_ = FixpSbeClientState::Terminated;
                if constexpr (requires { handler_.on_terminate(terminate); }) handler_.on_terminate(terminate);
                return;
            }
            case sbe::TemplateId::NotApplied:
                handle_sequenced_frame(view, now_unix_ns);
                return;
            case sbe::TemplateId::UnsequencedHeartbeat:
                return;
            default:
                return fail(FixpSbeClientFailure::ProtocolViolation, "unexpected session message", now_unix_ns);
        }
    }

    // Sequence or EstablishmentAck: the next real-time message carries
    // next_server_sequence_number (FIXP 4.5).
    void restate_server_sequence(std::uint64_t next_server_sequence_number, std::uint64_t now_unix_ns) noexcept {
        if (next_server_sequence_number == sbe::kNullSequenceNumber ||
            next_server_sequence_number < next_expected_server_sequence_number_) {
            return fail(FixpSbeClientFailure::SequenceRegression, "NextSeqNo below the expected sequence number",
                        now_unix_ns);
        }
        next_realtime_server_sequence_number_ = next_server_sequence_number;
        highest_known_server_sequence_number_ =
            std::max(highest_known_server_sequence_number_, next_server_sequence_number - 1);
        request_missing_messages(now_unix_ns);
    }

    void handle_sequenced_frame(const sbe::FrameView& view, std::uint64_t now_unix_ns) noexcept {
        if (state_ != FixpSbeClientState::Established && state_ != FixpSbeClientState::TerminateSent) {
            return fail(FixpSbeClientFailure::ProtocolViolation, "application message before EstablishmentAck", now_unix_ns);
        }
        FixpSbeReceivedMessageInfo info;
        if (retransmitted_messages_remaining_ != 0) {
            info.server_sequence_number = next_retransmitted_server_sequence_number_++;
            info.retransmitted = true;
            ++statistics_.retransmitted_messages_received;
            if (--retransmitted_messages_remaining_ == 0) retransmit_request_in_flight_ = false;
        } else {
            info.server_sequence_number = next_realtime_server_sequence_number_++;
            highest_known_server_sequence_number_ =
                std::max(highest_known_server_sequence_number_, info.server_sequence_number);
        }
        if (info.server_sequence_number < next_expected_server_sequence_number_) {
            ++statistics_.duplicate_server_messages_ignored;
        } else if (info.server_sequence_number > next_expected_server_sequence_number_) {
            ++statistics_.server_messages_dropped_beyond_gap;
        } else {
            ++next_expected_server_sequence_number_;
            ++statistics_.application_messages_delivered;
            deliver(info, view, now_unix_ns);
        }
        if (!retransmit_request_in_flight_) request_missing_messages(now_unix_ns);
    }

    void deliver(const FixpSbeReceivedMessageInfo& info, const sbe::FrameView& view, std::uint64_t now_unix_ns) noexcept {
        switch (static_cast<sbe::TemplateId>(view.header.template_id)) {
            case sbe::TemplateId::NotApplied: return deliver_as<sbe::NotApplied>(info, view, now_unix_ns);
            case sbe::TemplateId::ExecutionReportNew: return deliver_as<sbe::ExecutionReportNew>(info, view, now_unix_ns);
            case sbe::TemplateId::ExecutionReportTrade: return deliver_as<sbe::ExecutionReportTrade>(info, view, now_unix_ns);
            case sbe::TemplateId::ExecutionReportCancel: return deliver_as<sbe::ExecutionReportCancel>(info, view, now_unix_ns);
            case sbe::TemplateId::ExecutionReportReplace:
                return deliver_as<sbe::ExecutionReportReplace>(info, view, now_unix_ns);
            case sbe::TemplateId::ExecutionReportReject: return deliver_as<sbe::ExecutionReportReject>(info, view, now_unix_ns);
            case sbe::TemplateId::OrderCancelReject: return deliver_as<sbe::OrderCancelReject>(info, view, now_unix_ns);
            case sbe::TemplateId::OrderMassCancelReport: return deliver_as<sbe::OrderMassCancelReport>(info, view, now_unix_ns);
            case sbe::TemplateId::BusinessMessageReject: return deliver_as<sbe::BusinessMessageReject>(info, view, now_unix_ns);
            default:
                if constexpr (requires { handler_.on_unknown_application_message(info, view); }) {
                    handler_.on_unknown_application_message(info, view);
                }
                return;
        }
    }

    template <class Message>
    void deliver_as(const FixpSbeReceivedMessageInfo& info, const sbe::FrameView& view, std::uint64_t now_unix_ns) noexcept {
        Message message;
        if (!decode_or_fail(view, message, now_unix_ns)) return;
        if constexpr (requires { handler_.on_application_message(info, message); }) {
            handler_.on_application_message(info, message);
        }
    }

    // One RetransmitRequest in flight at a time; later batches are requested
    // as earlier ones complete (FIXP 4.6.2).
    void request_missing_messages(std::uint64_t now_unix_ns) noexcept {
        if (retransmit_request_in_flight_ || state_ != FixpSbeClientState::Established) return;
        if (settings_.maximum_retransmit_request_count == 0) return;
        if (highest_known_server_sequence_number_ < next_expected_server_sequence_number_) return;
        const std::uint64_t missing_count =
            highest_known_server_sequence_number_ - next_expected_server_sequence_number_ + 1;
        sbe::RetransmitRequest request;
        request.session_id = session_uuid_;
        request.timestamp_ns = now_unix_ns;
        request.from_seq_no = next_expected_server_sequence_number_;
        request.count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(missing_count, settings_.maximum_retransmit_request_count));
        if (!write_message(request, now_unix_ns)) return;
        retransmit_timestamp_ns_ = now_unix_ns;
        requested_from_seq_no_ = request.from_seq_no;
        requested_count_ = request.count;
        retransmit_request_in_flight_ = true;
        ++statistics_.retransmit_requests_sent;
    }

    template <class Message>
    [[nodiscard]] bool decode_or_fail(const sbe::FrameView& view, Message& message, std::uint64_t now_unix_ns) noexcept {
        if (sbe::decode(view, message) == sbe::DecodeStatus::Ok) return true;
        fail(FixpSbeClientFailure::ProtocolViolation, "malformed message", now_unix_ns);
        return false;
    }

    void fail(FixpSbeClientFailure failure, std::string_view reason, std::uint64_t now_unix_ns) noexcept {
        if (state_ == FixpSbeClientState::Terminated) return;
        if (state_ == FixpSbeClientState::Established || state_ == FixpSbeClientState::Negotiated) {
            send_terminate(sbe::TerminationCode::UnspecifiedError, reason, now_unix_ns);
        }
        failure_ = failure;
        state_ = FixpSbeClientState::Terminated;
    }

    void send_terminate(sbe::TerminationCode code, std::string_view reason, std::uint64_t now_unix_ns) noexcept {
        sbe::Terminate terminate;
        terminate.session_id = session_uuid_;
        terminate.code = code;
        terminate.reason = reason;
        write_message(terminate, now_unix_ns);
    }

    template <class Message>
    bool write_message(const Message& message, std::uint64_t now_unix_ns) noexcept {
        const std::size_t frame_size = sbe::encoded_frame_size(message);
        char* out = sink_.prepare(frame_size);
        if (out == nullptr || sbe::encode(message, out, frame_size) != frame_size) {
            failure_ = FixpSbeClientFailure::SendBufferFull;
            state_ = FixpSbeClientState::Terminated;
            return false;
        }
        sink_.commit(frame_size);
        last_outbound_unix_ns_ = now_unix_ns;
        return true;
    }

    ByteSink sink_;
    Handler& handler_;
    FixpSbeClientSettings settings_;
    FixpSbeClientState state_ = FixpSbeClientState::Disconnected;
    FixpSbeClientFailure failure_ = FixpSbeClientFailure::None;
    FixpSbeClientStatistics statistics_;
    sbe::SessionUuid session_uuid_{};
    bool negotiated_on_this_transport_ = false;
    std::uint32_t server_keepalive_interval_ms_ = 0;
    std::uint64_t negotiation_timestamp_ns_ = 0;
    std::uint64_t establishment_timestamp_ns_ = 0;
    std::uint64_t retransmit_timestamp_ns_ = 0;
    std::uint64_t requested_from_seq_no_ = 0;
    std::uint32_t requested_count_ = 0;
    std::uint64_t last_inbound_unix_ns_ = 0;
    std::uint64_t last_outbound_unix_ns_ = 0;

    // Idempotent client flow.
    std::uint64_t next_client_sequence_number_ = 1;
    bool client_sequence_message_required_ = true;

    // Recoverable server flow.
    std::uint64_t next_expected_server_sequence_number_ = 1;
    std::uint64_t next_realtime_server_sequence_number_ = 1;
    std::uint64_t highest_known_server_sequence_number_ = 0;
    bool retransmit_request_in_flight_ = false;
    std::uint64_t next_retransmitted_server_sequence_number_ = 0;
    std::uint32_t retransmitted_messages_remaining_ = 0;
};

}  // namespace exsim::gateway::fixp_sbe

#endif
