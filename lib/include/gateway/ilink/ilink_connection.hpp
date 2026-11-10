#ifndef EXSIM_GATEWAY_ILINK_CONNECTION_HPP
#define EXSIM_GATEWAY_ILINK_CONNECTION_HPP
#include "gateway/ilink/ilink_user.hpp"

namespace exsim::gateway::ilink {
template<class Sink> class IlinkConnection {
public:
    explicit IlinkConnection(IlinkUsers<Sink>& users) : users_(users) {}
    template<class Core> void open(Sink sink, server::ConnectionId id, Core& core) noexcept {
        sink_ = sink; id_ = id; user_ = nullptr; established_ = false; close_ = overflow_ = warned_ = false;
        last_received_ = last_sent_ = core.now(); keepalive_ns_ = 5000000000;
    }
    template<class Core> std::size_t on_data(std::span<const char> bytes, Core& core) noexcept {
        std::size_t consumed = 0;
        while (!bytes.empty() && !wants_close()) {
            protocol::cme::MessageView message;
            const auto status = protocol::cme::decode_ilink(bytes, message);
            if (status == protocol::Status::incomplete) break;
            if (status != protocol::Status::ok || message.header.schema_id != schema::id || message.header.version != schema::version) {
                terminate("invalid frame or schema", core); return consumed + bytes.size();
            }
            last_received_ = core.now(); warned_ = false;
            switch (message.header.template_id) {
                case 500: authenticate<schema::Negotiate500>(message, core); break;
                case 503: authenticate<schema::Establish503>(message, core); break;
                case 506: sequence(message, core); break;
                case 507: terminate("peer terminated", core); break;
                case 508: retransmit(message, core); break;
                case 514: application<schema::NewOrderSingle514>(message, core); break;
                case 515: application<schema::OrderCancelReplaceRequest515>(message, core); break;
                case 516: application<schema::OrderCancelRequest516>(message, core); break;
                case 529: application<schema::OrderMassActionRequest529>(message, core); break;
                default: terminate("unsupported template", core); break;
            }
            consumed += message.frame_size; bytes = bytes.subspan(message.frame_size);
        }
        return consumed;
    }
    template<class Core> void on_timer(Core& core) noexcept {
        if (wants_close()) return;
        const auto now = core.now();
        const auto elapsed = now >= last_received_ ? now - last_received_ : 0;
        if (!established_) { if (elapsed >= 5000000000) terminate("establishment timeout", core); return; }
        if (elapsed >= 2 * keepalive_ns_) { terminate("keepalive timeout", core); return; }
        if ((elapsed >= keepalive_ns_ && !warned_) || now - std::max(last_sent_, user_->last_send_time) >= keepalive_ns_) {
            using M = schema::Sequence506;
            wire::Root<M> response;
            response.set(M::UUID, user_->uuid); response.set(M::NextSeqNo, static_cast<std::uint32_t>(user_->store.next_seq()));
            response.set(M::FaultToleranceIndicator, 1);
            response.set(M::KeepAliveIntervalLapsed, elapsed >= keepalive_ns_ ? 1 : 0);
            send(response, core); warned_ = elapsed >= keepalive_ns_;
        }
    }
    template<class Core> void on_closed(Core& core) noexcept {
        if (user_ && user_->reserved_by == id_) {
            user_->sink.reset(); user_->reserved_by = 0;
            if (established_) core.unbind(user_->config.session_id, id_);
        }
        user_ = nullptr; established_ = false;
    }
    template<class Core> void shutdown(Core& core) noexcept { terminate("end of day", core); }
    [[nodiscard]] SessionId bound_session() const noexcept { return established_ && user_ ? user_->config.session_id : 0; }
    [[nodiscard]] bool wants_close() const noexcept { return close_ || (user_ && user_->output_failed); }
    [[nodiscard]] bool flush_before_close() const noexcept { return !overflow_ && (!user_ || !user_->output_failed); }
private:
    template<class M, class Core> void send(const wire::Root<M>& message, Core& core,
                                           std::span<const char> tail = M::empty_tail) noexcept {
        std::array<char, 2048> bytes{};
        const auto size = message.encode(bytes, tail);
        char* output = size ? sink_.prepare(size) : nullptr;
        if (!output) { close_ = overflow_ = true; return; }
        std::memcpy(output, bytes.data(), size); sink_.commit(size); last_sent_ = core.now();
    }
    template<class Core> void terminate(std::string_view reason, Core& core) noexcept {
        if (close_) return;
        using M = schema::Terminate507;
        wire::Root<M> message;
        message.set(M::Reason, reason); message.set(M::UUID, user_ ? user_->uuid : 0);
        message.set(M::RequestTimestamp, core.realtime_ns()); message.set(M::ErrorCodes, 0);
        send(message, core); close_ = true;
    }
    template<class M, class Core> void authentication_reject(std::uint64_t uuid, std::uint64_t timestamp,
                                                             std::string_view reason, Core& core) noexcept {
        wire::Root<M> response;
        response.set(M::UUID, uuid); response.set(M::RequestTimestamp, timestamp);
        response.set(M::Reason, reason); response.set(M::ErrorCodes, 1);
        response.set(M::FaultToleranceIndicator, 1);
        response.set(M::EnvironmentIndicator, 0);
        if constexpr (requires { M::NextSeqNo; }) response.set(M::NextSeqNo, user_ ? user_->expected_sequence : 1);
        send(response, core); close_ = true;
    }
    template<class M, class Core> void authenticate(const protocol::cme::MessageView& message, Core& core) noexcept {
        constexpr bool negotiate = M::id == 500;
        using Reject = std::conditional_t<negotiate, schema::NegotiationReject502, schema::EstablishmentReject505>;
        if (!wire::valid<M>(message) || message.trailing().size() != 2 || message.trailing()[0] || message.trailing()[1]) {
            terminate("invalid establishment block or credentials", core); return;
        }
        const auto uuid = wire::get(message, M::UUID), timestamp = wire::get(message, M::RequestTimestamp);
        const auto access = wire::get(message, M::AccessKeyID), session = wire::get(message, M::Session), firm = wire::get(message, M::Firm);
        auto* candidate = users_.credentials(access, session, firm);
        const auto epoch = core.realtime_ns();
        bool valid = !established_ && uuid && uuid != UINT64_MAX && candidate && !candidate->output_failed &&
            wire::valid_text(message, M::AccessKeyID) && wire::valid_text(message, M::Session) && wire::valid_text(message, M::Firm) &&
            (timestamp > epoch ? timestamp - epoch : epoch - timestamp) <= 60000000000 &&
            timestamp > candidate->last_auth_timestamp && (!candidate->reserved_by || candidate->reserved_by == id_) &&
            (!user_ || user_ == candidate);
        wire::SigningText text; text.append(timestamp); text.append(uuid); text.append(session); text.append(firm);
        if constexpr (!negotiate) {
            valid = valid && wire::valid_text(message, M::TradingSystemName) && wire::valid_text(message, M::TradingSystemVersion) &&
                wire::valid_text(message, M::TradingSystemVendor);
            text.append(wire::get(message, M::TradingSystemName)); text.append(wire::get(message, M::TradingSystemVersion));
            text.append(wire::get(message, M::TradingSystemVendor)); text.append(wire::get(message, M::NextSeqNo));
            text.append(wire::get(message, M::KeepAliveInterval));
        }
        if (valid) {
            const auto signature = wire::hmac_sha256(candidate->key, text.view());
            valid = text.good && wire::signature_equal(signature, std::span<const char, 32>(message.body.data() + M::HMACSignature.offset, 32));
        }
        if (!valid) { authentication_reject<Reject>(uuid, timestamp, "authentication failed or session in use", core); return; }
        if constexpr (negotiate) {
            if (candidate->uuid && uuid <= candidate->uuid) { authentication_reject<Reject>(uuid, timestamp, "UUID must increase for negotiation", core); return; }
            candidate->negotiate(uuid);
            candidate->reserved_by = id_; candidate->last_auth_timestamp = timestamp; user_ = candidate;
            using Response = schema::NegotiationResponse501;
            wire::Root<Response> response;
            response.set(Response::UUID, uuid); response.set(Response::RequestTimestamp, timestamp);
            response.set(Response::FaultToleranceIndicator, 1); response.set(Response::EnvironmentIndicator, 0);
            response.set(Response::PreviousSeqNo, static_cast<std::uint32_t>(candidate->previous_store.last_seq()));
            response.set(Response::PreviousUUID, candidate->previous_uuid);
            send(response, core);
        } else {
            const auto interval = wire::get(message, M::KeepAliveInterval);
            if (uuid != candidate->uuid || interval < 5000 || interval > 60000 ||
                wire::get(message, M::NextSeqNo) != candidate->expected_sequence || !core.bind(candidate->config.session_id, id_)) {
                authentication_reject<Reject>(uuid, timestamp, "unknown UUID, sequence, keepalive or session in use", core); return;
            }
            candidate->reserved_by = id_; candidate->last_auth_timestamp = timestamp; user_ = candidate;
            candidate->sink = sink_; established_ = true;
            keepalive_ns_ = std::uint64_t{interval} * 1000000;
            using Response = schema::EstablishmentAck504;
            wire::Root<Response> response;
            response.set(Response::UUID, uuid); response.set(Response::RequestTimestamp, timestamp);
            response.set(Response::NextSeqNo, static_cast<std::uint32_t>(candidate->store.next_seq()));
            response.set(Response::PreviousSeqNo, static_cast<std::uint32_t>(candidate->previous_store.last_seq()));
            response.set(Response::PreviousUUID, candidate->previous_uuid); response.set(Response::KeepAliveInterval, interval);
            response.set(Response::FaultToleranceIndicator, 1); response.set(Response::EnvironmentIndicator, 0);
            send(response, core);
        }
    }
    template<class M, class Core> void application(const protocol::cme::MessageView& message, Core& core) noexcept {
        if (!established_ || !wire::valid<M>(message) || !message.trailing().empty()) { terminate("application before establishment or invalid block", core); return; }
        const auto sequence = wire::get(message, M::SeqNum);
        if (sequence < user_->expected_sequence || !sequence || sequence > 999999999) { terminate("invalid or duplicate sequence", core); return; }
        if (sequence > user_->expected_sequence) {
            using Response = schema::NotApplied513;
            wire::Root<Response> response;
            response.set(Response::UUID, user_->uuid); response.set(Response::FromSeqNo, user_->expected_sequence);
            response.set(Response::MsgCount, sequence - user_->expected_sequence);
            send(response, core); user_->gap_pending = true; return;
        }
        ++user_->expected_sequence;
        user_->application(message, core);
    }
    template<class Core> void sequence(const protocol::cme::MessageView& message, Core& core) noexcept {
        using M = schema::Sequence506;
        if (!established_ || !wire::valid<M>(message) || !message.trailing().empty() || wire::get(message, M::UUID) != user_->uuid ||
            wire::get(message, M::KeepAliveIntervalLapsed) > 1 ||
            (wire::get(message, M::FaultToleranceIndicator) != 1 && wire::get(message, M::FaultToleranceIndicator) != UINT8_MAX)) {
            terminate("invalid sequence message", core); return;
        }
        const auto next = wire::get(message, M::NextSeqNo);
        if (next < user_->expected_sequence || next > 1000000000 || (!user_->gap_pending && next != user_->expected_sequence)) {
            terminate("unexpected sequence reset", core); return;
        }
        user_->expected_sequence = next; user_->gap_pending = false;
        if (wire::get(message, M::KeepAliveIntervalLapsed)) {
            wire::Root<M> response; response.set(M::UUID, user_->uuid);
            response.set(M::NextSeqNo, static_cast<std::uint32_t>(user_->store.next_seq()));
            response.set(M::FaultToleranceIndicator, 1); response.set(M::KeepAliveIntervalLapsed, 0);
            send(response, core);
        }
    }
    static void retransmission_flag(std::span<char> bytes) noexcept {
        const auto id = protocol::cme::load_le<std::uint16_t>(bytes.data() + 6);
        std::size_t offset = 0;
        switch (id) {
            case 521: offset = schema::BusinessReject521::PossRetransFlag.offset; break;
            case 522: offset = schema::ExecutionReportNew522::PossRetransFlag.offset; break;
            case 523: offset = schema::ExecutionReportReject523::PossRetransFlag.offset; break;
            case 524: offset = schema::ExecutionReportElimination524::PossRetransFlag.offset; break;
            case 525: offset = schema::ExecutionReportTradeOutright525::PossRetransFlag.offset; break;
            case 531: offset = schema::ExecutionReportModify531::PossRetransFlag.offset; break;
            case 534: offset = schema::ExecutionReportCancel534::PossRetransFlag.offset; break;
            case 535: offset = schema::OrderCancelReject535::PossRetransFlag.offset; break;
            case 536: offset = schema::OrderCancelReplaceReject536::PossRetransFlag.offset; break;
            case 562: offset = schema::OrderMassActionReport562::PossRetransFlag.offset; break;
        }
        if (offset && 12 + offset < bytes.size()) bytes[12 + offset] = 1;
    }
    template<class Core> void retransmit(const protocol::cme::MessageView& message, Core& core) noexcept {
        using M = schema::RetransmitRequest508;
        if (!established_ || !wire::valid<M>(message) || !message.trailing().empty() || wire::get(message, M::UUID) != user_->uuid) {
            terminate("invalid retransmit request", core); return;
        }
        const auto last_uuid = wire::get(message, M::LastUUID);
        const auto* store = last_uuid == UINT64_MAX || last_uuid == user_->uuid ? &user_->store :
                            user_->previous_uuid && last_uuid == user_->previous_uuid ? &user_->previous_store : nullptr;
        const auto begin = wire::get(message, M::FromSeqNo);
        std::uint64_t count = wire::get(message, M::MsgCount);
        const bool unsupported_previous_all = store == &user_->previous_store && count == 0;
        if (store && !count && begin <= store->next_seq()) count = std::min(std::uint64_t{2500}, store->next_seq() - begin);
        if (!store || unsupported_previous_all || !begin || count > 2500 || begin > store->next_seq() || count > store->next_seq() - begin) {
            using Reject = schema::RetransmitReject510;
            wire::Root<Reject> response; response.set(Reject::UUID, user_->uuid); response.set(Reject::LastUUID, last_uuid);
            response.set(Reject::RequestTimestamp, wire::get(message, M::RequestTimestamp));
            response.set(Reject::Reason, "requested history unavailable or range exceeds 2500"); response.set(Reject::ErrorCodes, 1);
            send(response, core); return;
        }
        using Response = schema::Retransmission509;
        wire::Root<Response> response; response.set(Response::UUID, user_->uuid); response.set(Response::LastUUID, last_uuid);
        response.set(Response::RequestTimestamp, wire::get(message, M::RequestTimestamp)); response.set(Response::FromSeqNo, begin);
        response.set(Response::MsgCount, static_cast<std::uint16_t>(count)); send(response, core);
        for (std::uint64_t sequence = begin; sequence < begin + count && !wants_close(); ++sequence) {
            const auto original = store->get(sequence).bytes;
            char* output = sink_.prepare(original.size());
            if (!output) { close_ = overflow_ = true; return; }
            std::memcpy(output, original.data(), original.size());
            retransmission_flag({output, original.size()}); sink_.commit(original.size());
        }
        last_sent_ = core.now();
    }
    IlinkUsers<Sink>& users_;
    Sink sink_{};
    IlinkUser<Sink>* user_ = nullptr;
    server::ConnectionId id_ = 0;
    Timestamp last_received_ = 0, last_sent_ = 0;
    std::uint64_t keepalive_ns_ = 5000000000;
    bool established_ = false, close_ = false, overflow_ = false, warned_ = false;
};
}  // namespace exsim::gateway::ilink
#endif
