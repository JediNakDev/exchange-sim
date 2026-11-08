#include <exsim/net/tcp_server.hpp>
namespace exsim::net {
const char* close_reason_name(CloseReason reason) noexcept {
    switch (reason) {
        case CloseReason::PeerClosed:
            return "peer closed";
        case CloseReason::ConnectionError:
            return "connection error";
        case CloseReason::ReceiveBufferFull:
            return "receive buffer full";
        case CloseReason::SendBufferOverflow:
            return "send buffer overflow";
    }
    return "unknown";
}
TcpServer::TcpServer(const TcpServerConfig& config, Poller& poller, std::uint8_t tag)
    : config_(config),
      poller_(poller),
      token_tag_(tag),
      free_slot_stack_(config.max_connections),
      flush_queue_(config.max_connections) {
    slots_.reserve(config.max_connections);
    for (std::uint32_t index = 0; index < config.max_connections; ++index) {
        slots_.emplace_back(config.receive_buffer_capacity, config.send_buffer_capacity);
        free_slot_stack_[index] = config.max_connections - index - 1;
    }
    free_slot_count_ = config.max_connections;
}
TcpServer::~TcpServer() {
    close();
}
ErrorCode TcpServer::open() noexcept {
    if (listener_.is_open()) return EALREADY;
    if (slots_.empty() || !config_.receive_buffer_capacity || !config_.send_buffer_capacity) return EINVAL;
    int error = open_tcp_listener(config_.listen_endpoint, config_.listen_backlog, config_.socket_send_buffer_bytes,
                                  config_.socket_receive_buffer_bytes, listener_);
    if (error) return error;
    error = local_endpoint(listener_.fd(), listen_endpoint_);
    if (!error)
        error = poller_.add(listener_.fd(), make_poll_token(token_tag_, kListenerSlotIndex), PollInterest::Read);
    if (error) listener_.close();
    return error;
}
void TcpServer::close() noexcept {
    if (listener_.is_open()) {
        poller_.remove(listener_.fd());
        listener_.close();
    }
    for (std::uint32_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index].state != SlotState::Free) release_slot(index);
        slots_[index].queued_for_flush = false;
    }
    flush_queue_head_ = 0;
    flush_queue_count_ = 0;
}
TcpServer::AcceptOutcome TcpServer::accept_one(ConnectionId& id, Ipv4Endpoint& peer) noexcept {
    Socket accepted;
    int error = accept_connection(listener_.fd(), accepted, peer);
    if (error) return is_would_block(error) ? AcceptOutcome::NothingPending : AcceptOutcome::Failed;
    if (!free_slot_count_) {
        ++rejected_connection_count_;
        return AcceptOutcome::Rejected;
    }
    if ((error = set_tcp_no_delay(accepted.fd(), config_.tcp_no_delay))) return AcceptOutcome::Failed;
    const auto index = free_slot_stack_[--free_slot_count_];
    Slot& slot = slots_[index];
    if ((error = poller_.add(accepted.fd(), slot_token(index), PollInterest::Read))) {
        free_slot_stack_[free_slot_count_++] = index;
        return AcceptOutcome::Failed;
    }
    slot.connection.attach(std::move(accepted));
    slot.state = SlotState::Open;
    slot.write_interest = false;
    ++connection_count_;
    id = {index, slot.generation};
    return AcceptOutcome::Accepted;
}
void TcpServer::release_slot(std::uint32_t index) noexcept {
    Slot& slot = slots_[index];
    poller_.remove(slot.connection.fd());
    slot.connection.close();
    slot.state = SlotState::Free;
    slot.write_interest = false;
    // Keep queued_for_flush until its existing queue entry is popped. A new
    // occupant can share that entry without filling the ring with stale entries.
    --connection_count_;
    // Retire exhausted generations rather than ever making a stale id valid.
    if (slot.generation < kGenerationMask) {
        ++slot.generation;
        free_slot_stack_[free_slot_count_++] = index;
    }
}
bool TcpServer::close_now(ConnectionId id) noexcept {
    if (!live_slot(id)) return false;
    release_slot(id.slot);
    return true;
}
bool TcpServer::close_after_flush(ConnectionId id) noexcept {
    Slot* slot = open_slot(id);
    if (!slot) return false;
    slot->state = SlotState::Closing;
    queue_for_flush(id.slot);
    return true;
}
void TcpServer::finish_graceful_close(std::uint32_t index) noexcept {
    // close() with unread input sends RST instead of FIN, and the kernel then
    // drops queued output the peer has not received. Discard the input that
    // has arrived first. Bounded, so a flooding peer cannot hold the loop;
    // input arriving after the last read can still turn the close into RST.
    TcpConnection& connection = slots_[index].connection;
    for (int read = 0; read < kMaxDiscardReadsBeforeClose; ++read) {
        connection.receive_buffer().clear();
        if (connection.receive().status != ReceiveStatus::Received) break;
    }
    release_slot(index);
}
TcpServer::FlushOutcome TcpServer::flush_slot(std::uint32_t index) noexcept {
    Slot& slot = slots_[index];
    if (slot.connection.send_buffer().overflowed()) return {true, CloseReason::SendBufferOverflow, ENOBUFS};
    const auto flushed = slot.connection.flush();
    if (flushed.status == FlushStatus::Error) return {true, CloseReason::ConnectionError, flushed.error};
    if (flushed.status == FlushStatus::Complete && slot.state == SlotState::Closing) {
        finish_graceful_close(index);
        return {false, CloseReason::PeerClosed, 0};
    }
    const bool pending = slot.connection.send_buffer().has_pending();
    if (pending != slot.write_interest || slot.state == SlotState::Closing) {
        const int error =
            poller_.modify(slot.connection.fd(), slot_token(index),
                           slot.state == SlotState::Closing ? PollInterest::Write
                                                            : (pending ? PollInterest::ReadWrite : PollInterest::Read));
        if (error) return {true, CloseReason::ConnectionError, error};
        slot.write_interest = pending;
    }
    return {false, CloseReason::PeerClosed, 0};
}
}  // namespace exsim::net
