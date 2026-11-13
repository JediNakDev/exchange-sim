#ifndef EXSIM_SERVER_RUNTIME_ORDER_ENTRY_ENDPOINT_HPP
#define EXSIM_SERVER_RUNTIME_ORDER_ENTRY_ENDPOINT_HPP

// One gateway's TCP listener and its connection slots.
// Every slot's gateway connection object is built at startup; accepting a
// connection only reinitialises it. The endpoint
//   * feeds received bytes to the gateway, keeping an unconsumed tail
//     (partial FIX frame) in the receive buffer;
//   * applies `fault user X delay <us>` by holding received bytes of a
//     logged-in user in the receive buffer until their release time, in
//     arrival order (gateways never see the delay);
//   * closes connections the gateway asks to close, flushing first unless
//     the gateway says the output is lost anyway, then calls the gateway's
//     on_closed (unbind, cancel-on-disconnect);
//   * closes a connection on the core's request (`fault user X disconnect`).
//
// The core's ConnectionId packs the endpoint's poll tag and the net
// connection: tag << 56 | generation << 32 | slot.

#include "net/poller.hpp"
#include "net/tcp_server.hpp"
#include "server/core/policies.hpp"
#include "server/runtime/tcp_send_sink.hpp"
#include "types.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace exsim::server::runtime {

[[nodiscard]] constexpr server::ConnectionId pack_core_connection_id(std::uint8_t endpoint_tag,
                                                                     net::ConnectionId connection) noexcept {
    return (std::uint64_t{endpoint_tag} << 56) | (std::uint64_t{connection.generation & 0x00FFFFFFu} << 32) |
           std::uint64_t{connection.slot};
}
[[nodiscard]] constexpr std::uint8_t core_connection_endpoint_tag(server::ConnectionId id) noexcept {
    return static_cast<std::uint8_t>(id >> 56);
}
[[nodiscard]] constexpr net::ConnectionId core_connection_net_id(server::ConnectionId id) noexcept {
    return net::ConnectionId{static_cast<std::uint32_t>(id), static_cast<std::uint32_t>((id >> 32) & 0x00FFFFFFu)};
}

template <class Gateway, class Core>
class OrderEntryEndpoint {
public:
    using Connection = typename Gateway::template Connection<Core>;

    // Delayed-release marks a connection can hold; beyond this, new bytes
    // join the newest mark (released no earlier than it).
    static constexpr std::size_t kMaxDelayMarks = 256;

    OrderEntryEndpoint(const net::TcpServerConfig& tcp_config, net::Poller& poller, std::uint8_t poll_tag,
                       typename Gateway::UserDirectory& users, Core& core)
        : tcp_server_(tcp_config, poller, poll_tag),
          core_(core),
          poll_tag_(poll_tag),
          slot_count_(tcp_config.max_connections),
          slots_(std::make_unique<Slot[]>(slot_count_)),
          connections_(std::make_unique<std::unique_ptr<Connection>[]>(slot_count_)),
          delay_marks_(std::make_unique<DelayMark[]>(std::size_t{slot_count_} * kMaxDelayMarks)),
          open_slot_words_(std::make_unique<std::uint64_t[]>(open_slot_word_count())) {
        for (std::size_t index = 0; index < slot_count_; ++index) {
            connections_[index] = std::make_unique<Connection>(users);
        }
    }

    OrderEntryEndpoint(const OrderEntryEndpoint&) = delete;
    OrderEntryEndpoint& operator=(const OrderEntryEndpoint&) = delete;

    [[nodiscard]] net::ErrorCode open() noexcept { return tcp_server_.open(); }
    [[nodiscard]] net::Ipv4Endpoint listen_endpoint() const noexcept { return tcp_server_.listen_endpoint(); }
    [[nodiscard]] std::uint8_t poll_tag() const noexcept { return poll_tag_; }
    // Open plus closing-and-flushing connections.
    [[nodiscard]] std::uint32_t transport_connection_count() const noexcept { return tcp_server_.connection_count(); }

    void handle_event(const net::PollEvent& event) noexcept { tcp_server_.handle_event(event, *this); }
    void flush() noexcept { tcp_server_.flush_pending(*this); }

    // Once per loop iteration, over the open slots in slot order: delayed
    // input, gateway timers, requested closes.
    void run_timers() noexcept {
        if (open_connections_ == 0) return;
        const Timestamp now = core_.now();
        for (std::size_t word_index = 0; word_index < open_slot_word_count(); ++word_index) {
            for (std::uint64_t open_bits = open_slot_words_[word_index]; open_bits != 0; open_bits &= open_bits - 1) {
                const std::uint32_t index =
                    static_cast<std::uint32_t>(word_index * 64 + static_cast<std::size_t>(std::countr_zero(open_bits)));
                Slot& slot = slots_[index];
                if (!slot.open) continue;  // closed by an earlier callback of this pass
                if (slot.delay_mark_count != 0) {
                    release_due_input(index, now);
                    if (!slot.open) continue;
                }
                Connection& connection = *connections_[index];
                connection.on_timer(core_);
                if (connection.wants_close()) close_slot(index);
            }
        }
    }

    // `fault user X disconnect`: closes now (no flush) and unbinds.
    bool close_core_connection(server::ConnectionId core_connection) noexcept {
        if (core_connection_endpoint_tag(core_connection) != poll_tag_) return false;
        const net::ConnectionId net_id = core_connection_net_id(core_connection);
        if (net_id.slot >= slot_count_) return false;
        const Slot& slot = slots_[net_id.slot];
        if (!slot.open || slot.net_id != net_id) return false;
        tcp_server_.close_now(net_id);
        finish_close(net_id.slot);
        return true;
    }

    // Shutdown: every open connection ends its session; connections accepted
    // from now on are ended as soon as they open.
    void close_for_end_of_day() noexcept {
        ending_day_ = true;
        for (std::uint32_t index = 0; index < slot_count_; ++index) {
            if (!slots_[index].open) continue;
            Connection& connection = *connections_[index];
            connection.close_for_end_of_day(core_);
            if (connection.wants_close()) close_slot(index);
        }
    }

    // Shutdown deadline passed: drop whatever is left.
    void close_all_now() noexcept {
        for (std::uint32_t index = 0; index < slot_count_; ++index) {
            if (!slots_[index].open) continue;
            tcp_server_.close_now(slots_[index].net_id);
            finish_close(index);
        }
    }

    // ---- net::TcpServerHandler ------------------------------------------------------
    void on_connection_opened(net::ConnectionId id, net::Ipv4Endpoint) noexcept {
        slots_[id.slot] = Slot{.net_id = id, .open = true};
        open_slot_words_[id.slot / 64] |= std::uint64_t{1} << (id.slot % 64);
        ++open_connections_;
        Connection& connection = *connections_[id.slot];
        connection.open(TcpSendSink{&tcp_server_, id}, pack_core_connection_id(poll_tag_, id), core_);
        if (ending_day_) {
            connection.close_for_end_of_day(core_);
            if (connection.wants_close()) close_slot(id.slot);
        }
    }

    void on_connection_data(net::ConnectionId id, net::ReceiveBuffer& received) noexcept {
        Slot& slot = slots_[id.slot];
        if (!slot.open) {
            received.consume(received.size());
            return;
        }
        slot.bytes_received = slot.bytes_consumed + received.size();
        const SessionId session = connections_[id.slot]->bound_session();
        const std::uint64_t delay_ns = session == 0 ? 0 : core_.inbound_delay_ns(session);
        const Timestamp now = core_.now();
        if (delay_ns != 0 || slot.delay_mark_count != 0) {
            add_delay_mark(id.slot, now + delay_ns);
            release_due_input(id.slot, now);
            return;
        }
        slot.bytes_released = slot.bytes_received;
        feed_released_input(id.slot, received);
    }

    void on_connection_closed(net::ConnectionId id, net::CloseReason, net::ErrorCode) noexcept {
        const Slot& slot = slots_[id.slot];
        if (slot.open && slot.net_id == id) finish_close(id.slot);
    }

private:
    struct DelayMark {
        std::uint64_t stream_end;  // bytes_received when the mark was made
        Timestamp release_at;
    };

    // What the loop reads for every open slot; the gateway connection and
    // the delay marks live in their own arrays.
    struct Slot {
        net::ConnectionId net_id{};
        bool open = false;
        std::uint16_t delay_mark_head = 0;
        std::uint16_t delay_mark_count = 0;
        std::uint64_t bytes_received = 0;  // stream offsets since the connection opened
        std::uint64_t bytes_consumed = 0;
        std::uint64_t bytes_released = 0;  // may be fed to the gateway up to here
    };
    static_assert(kMaxDelayMarks <= std::numeric_limits<std::uint16_t>::max());

    [[nodiscard]] std::size_t open_slot_word_count() const noexcept { return (std::size_t{slot_count_} + 63) / 64; }

    [[nodiscard]] DelayMark& delay_mark(std::uint32_t index, std::size_t ring_position) noexcept {
        return delay_marks_[std::size_t{index} * kMaxDelayMarks + ring_position % kMaxDelayMarks];
    }

    void add_delay_mark(std::uint32_t index, Timestamp release_at) noexcept {
        Slot& slot = slots_[index];
        if (slot.delay_mark_count == kMaxDelayMarks) {
            DelayMark& newest = delay_mark(index, slot.delay_mark_head + slot.delay_mark_count - 1u);
            newest.stream_end = slot.bytes_received;
            if (release_at > newest.release_at) newest.release_at = release_at;
            return;
        }
        delay_mark(index, slot.delay_mark_head + slot.delay_mark_count) = DelayMark{slot.bytes_received, release_at};
        ++slot.delay_mark_count;
    }

    // Releases marks in arrival order: a later mark never overtakes an
    // earlier one, even if its delay is shorter.
    void release_due_input(std::uint32_t index, Timestamp now) noexcept {
        Slot& slot = slots_[index];
        bool released_any = false;
        while (slot.delay_mark_count != 0) {
            const DelayMark& oldest = delay_mark(index, slot.delay_mark_head);
            if (oldest.release_at > now) break;
            slot.bytes_released = oldest.stream_end;
            slot.delay_mark_head = static_cast<std::uint16_t>((slot.delay_mark_head + 1u) % kMaxDelayMarks);
            --slot.delay_mark_count;
            released_any = true;
        }
        if (!released_any) return;
        net::TcpConnection* connection = tcp_server_.connection(slot.net_id);
        if (connection == nullptr) return;
        feed_released_input(index, connection->receive_buffer());
    }

    void feed_released_input(std::uint32_t index, net::ReceiveBuffer& received) noexcept {
        Slot& slot = slots_[index];
        const std::uint64_t releasable = slot.bytes_released - slot.bytes_consumed;
        if (releasable == 0) return;
        Connection& connection = *connections_[index];
        const std::span<const char> bytes = received.readable().first(static_cast<std::size_t>(releasable));
        const std::size_t consumed = connection.on_data(bytes, core_);
        received.consume(consumed);
        // An unconsumed tail (partial frame) stays buffered and is offered
        // again, with whatever follows it, on the next release.
        slot.bytes_consumed += consumed;
        if (slot.open && connection.wants_close()) close_slot(index);
    }

    void close_slot(std::uint32_t index) noexcept {
        const net::ConnectionId net_id = slots_[index].net_id;
        if (connections_[index]->flush_before_close()) {
            tcp_server_.close_after_flush(net_id);
        } else {
            tcp_server_.close_now(net_id);
        }
        finish_close(index);
    }

    void finish_close(std::uint32_t index) noexcept {
        slots_[index].open = false;
        slots_[index].delay_mark_count = 0;
        open_slot_words_[index / 64] &= ~(std::uint64_t{1} << (index % 64));
        --open_connections_;
        connections_[index]->on_closed(core_);
    }

    net::TcpServer tcp_server_;
    Core& core_;
    std::uint8_t poll_tag_;
    std::uint32_t slot_count_;
    std::unique_ptr<Slot[]> slots_;                           // index = net slot
    std::unique_ptr<std::unique_ptr<Connection>[]> connections_;  // index = net slot, built at startup
    std::unique_ptr<DelayMark[]> delay_marks_;                // kMaxDelayMarks ring per slot
    std::unique_ptr<std::uint64_t[]> open_slot_words_;       // bit per slot: open
    std::uint32_t open_connections_ = 0;
    bool ending_day_ = false;
};

}  // namespace exsim::server::runtime

#endif
