#ifndef EXSIM_FUZZ_TARGETS_STREAM_BUFFERS_HPP
#define EXSIM_FUZZ_TARGETS_STREAM_BUFFERS_HPP

// net::ReceiveBuffer and net::SendBuffer driven by a fuzz-chosen sequence of
// operations and compared with a model of their documented offsets:
//
//  * ReceiveBuffer: readable() is exactly the committed and not yet consumed
//    bytes, writable_tail() starts right after them and moves the unread
//    bytes to the front only when less than half the capacity is free at the
//    tail; consuming everything resets both offsets.
//  * SendBuffer: reserve(n) gives exactly n bytes right after pending(),
//    compacting first when needed; a request that cannot fit even after
//    compaction sets the sticky overflowed() flag and queues nothing.
//  * Both are driven against a non-blocking UNIX socket pair, so receive_from
//    and flush_to see real partial reads, would-block, peer close and EPIPE.
//
// Input: capacity byte for each buffer, then one opcode byte per operation
// followed by its arguments.

#include "fuzz_support.hpp"

#include <net/socket.hpp>
#include <net/stream_buffers.hpp>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace exsim::fuzz {

namespace stream_buffers_fuzz {

using net::FlushStatus;
using net::ReceiveStatus;

// Owns both ends of a socket pair; closes whatever is still open.
struct SocketPairForFuzzing {
    int buffer_side_fd = -1;  // the end the buffers under test use
    int peer_side_fd = -1;    // the end the harness plays as the remote host

    SocketPairForFuzzing() {
        int file_descriptors[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, file_descriptors) != 0) return;
        buffer_side_fd = file_descriptors[0];
        peer_side_fd = file_descriptors[1];
        (void)net::set_non_blocking(buffer_side_fd);
        (void)net::set_non_blocking(peer_side_fd);
        (void)net::set_no_sigpipe(buffer_side_fd);
        (void)net::set_no_sigpipe(peer_side_fd);
    }
    SocketPairForFuzzing(const SocketPairForFuzzing&) = delete;
    SocketPairForFuzzing& operator=(const SocketPairForFuzzing&) = delete;
    ~SocketPairForFuzzing() {
        if (buffer_side_fd >= 0) ::close(buffer_side_fd);
        close_peer();
    }
    [[nodiscard]] bool usable() const noexcept { return buffer_side_fd >= 0; }
    [[nodiscard]] bool peer_open() const noexcept { return peer_side_fd >= 0; }
    void close_peer() noexcept {
        if (peer_side_fd >= 0) ::close(peer_side_fd);
        peer_side_fd = -1;
    }
};

struct ReceiveBufferModel {
    std::size_t capacity = 0;
    std::size_t read_offset = 0;
    std::size_t write_offset = 0;
    std::string unread_bytes;
};

struct SendBufferModel {
    std::size_t capacity = 0;
    std::size_t read_offset = 0;
    std::size_t write_offset = 0;
    bool overflowed = false;
    std::string pending_bytes;
};

inline void check_receive_buffer_matches(const net::ReceiveBuffer& buffer, const ReceiveBufferModel& model) {
    EXSIM_FUZZ_REQUIRE(buffer.size() == model.unread_bytes.size(), "receive size is the unread byte count");
    EXSIM_FUZZ_REQUIRE(buffer.empty() == model.unread_bytes.empty(), "receive empty() agrees with size()");
    EXSIM_FUZZ_REQUIRE(buffer.capacity() == model.capacity, "receive capacity is fixed");
    EXSIM_FUZZ_REQUIRE(same_bytes(buffer.readable(), {model.unread_bytes.data(), model.unread_bytes.size()}),
                       "readable() holds exactly the unread bytes in order");
}

inline void check_send_buffer_matches(const net::SendBuffer& buffer, const SendBufferModel& model) {
    EXSIM_FUZZ_REQUIRE(buffer.pending_bytes() == model.pending_bytes.size(), "pending size");
    EXSIM_FUZZ_REQUIRE(buffer.has_pending() == !model.pending_bytes.empty(), "has_pending agrees");
    EXSIM_FUZZ_REQUIRE(buffer.overflowed() == model.overflowed, "overflowed is sticky until clear()");
    EXSIM_FUZZ_REQUIRE(same_bytes(buffer.pending(), {model.pending_bytes.data(), model.pending_bytes.size()}),
                       "pending() holds exactly the queued bytes in order");
}

// Takes the writable tail as the model says it must look, then commits a
// fuzz-chosen prefix of it filled with fuzz bytes.
inline void receive_buffer_write(net::ReceiveBuffer& buffer, ReceiveBufferModel& model, FuzzByteReader& reader) {
    if (model.read_offset != 0 &&
        (model.capacity - model.write_offset < model.capacity / 2 || model.write_offset == model.capacity)) {
        model.write_offset -= model.read_offset;
        model.read_offset = 0;
    }
    const std::span<char> tail = buffer.writable_tail();
    EXSIM_FUZZ_REQUIRE(tail.size() == model.capacity - model.write_offset,
                       "writable_tail compacts exactly when less than half the capacity is free");
    EXSIM_FUZZ_REQUIRE(tail.data() == buffer.readable().data() + buffer.size(),
                       "the tail starts right after the unread bytes");
    check_receive_buffer_matches(buffer, model);
    const std::string_view bytes = reader.take_text(tail.size());
    if (!bytes.empty()) std::memcpy(tail.data(), bytes.data(), bytes.size());
    buffer.commit(bytes.size());
    model.write_offset += bytes.size();
    model.unread_bytes.append(bytes);
}

inline void receive_buffer_consume(net::ReceiveBuffer& buffer, ReceiveBufferModel& model, FuzzByteReader& reader) {
    const std::size_t bytes = reader.take_index(model.unread_bytes.size() + 1);
    buffer.consume(bytes);
    model.read_offset += bytes;
    model.unread_bytes.erase(0, bytes);
    if (model.read_offset == model.write_offset) {
        model.read_offset = 0;
        model.write_offset = 0;
    }
}

inline void receive_buffer_receive_from(net::ReceiveBuffer& buffer, ReceiveBufferModel& model,
                                        SocketPairForFuzzing& sockets, std::string& bytes_in_flight_to_buffer) {
    if (model.read_offset != 0 &&
        (model.capacity - model.write_offset < model.capacity / 2 || model.write_offset == model.capacity)) {
        model.write_offset -= model.read_offset;
        model.read_offset = 0;
    }
    const std::size_t tail_size = model.capacity - model.write_offset;
    const net::ReceiveResult result = buffer.receive_from(sockets.buffer_side_fd);
    if (tail_size == 0) {
        EXSIM_FUZZ_REQUIRE(result.status == ReceiveStatus::BufferFull && result.bytes == 0,
                           "no free space is BufferFull, never a zero-length recv");
        return;
    }
    if (bytes_in_flight_to_buffer.empty()) {
        const ReceiveStatus expected = sockets.peer_open() ? ReceiveStatus::WouldBlock : ReceiveStatus::PeerClosed;
        EXSIM_FUZZ_REQUIRE(result.status == expected && result.bytes == 0,
                           "nothing in flight is WouldBlock, or PeerClosed after the peer closed");
        return;
    }
    EXSIM_FUZZ_REQUIRE(result.status == ReceiveStatus::Received, "bytes in flight are received");
    EXSIM_FUZZ_REQUIRE(result.bytes > 0 && result.bytes <= tail_size && result.bytes <= bytes_in_flight_to_buffer.size(),
                       "received count is bounded by the tail and the bytes in flight");
    model.write_offset += result.bytes;
    model.unread_bytes.append(bytes_in_flight_to_buffer, 0, result.bytes);
    bytes_in_flight_to_buffer.erase(0, result.bytes);
}

inline void send_buffer_reserve(net::SendBuffer& buffer, SendBufferModel& model, FuzzByteReader& reader) {
    const std::size_t requested = reader.take_index(model.capacity + 8);
    const std::span<char> space = buffer.reserve(requested);
    bool fits = !model.overflowed;
    if (fits && model.capacity - model.write_offset < requested) {
        model.write_offset -= model.read_offset;
        model.read_offset = 0;
        if (model.capacity - model.write_offset < requested) {
            model.overflowed = true;
            fits = false;
        }
    }
    if (!fits) {
        EXSIM_FUZZ_REQUIRE(space.empty(), "a reserve that does not fit returns an empty span");
        check_send_buffer_matches(buffer, model);
        return;
    }
    EXSIM_FUZZ_REQUIRE(space.size() == requested, "reserve returns exactly the requested size");
    EXSIM_FUZZ_REQUIRE(space.data() == buffer.pending().data() + buffer.pending_bytes(),
                       "reserved space starts right after the pending bytes");
    const std::string_view bytes = reader.take_text(requested);
    if (!bytes.empty()) std::memcpy(space.data(), bytes.data(), bytes.size());
    buffer.commit(bytes.size());
    model.write_offset += bytes.size();
    model.pending_bytes.append(bytes);
}

inline void send_buffer_append(net::SendBuffer& buffer, SendBufferModel& model, FuzzByteReader& reader) {
    const std::string_view bytes = reader.take_text(model.capacity + 8);
    const bool appended = buffer.append({bytes.data(), bytes.size()});
    bool fits = !model.overflowed;
    if (fits && model.capacity - model.write_offset < bytes.size()) {
        model.write_offset -= model.read_offset;
        model.read_offset = 0;
        if (model.capacity - model.write_offset < bytes.size()) {
            model.overflowed = true;
            fits = false;
        }
    }
    EXSIM_FUZZ_REQUIRE(appended == fits, "append succeeds exactly when the bytes fit");
    if (fits) {
        model.write_offset += bytes.size();
        model.pending_bytes.append(bytes);
    }
}

inline void send_buffer_flush(net::SendBuffer& buffer, SendBufferModel& model, SocketPairForFuzzing& sockets,
                              std::string& bytes_in_flight_to_peer) {
    const net::FlushResult result = buffer.flush_to(sockets.buffer_side_fd);
    if (model.pending_bytes.empty()) {
        EXSIM_FUZZ_REQUIRE(result.status == FlushStatus::Complete && result.bytes_sent == 0,
                           "nothing pending flushes as Complete without a send");
        return;
    }
    if (!sockets.peer_open()) {
        EXSIM_FUZZ_REQUIRE(result.status == FlushStatus::Error && result.bytes_sent == 0 && result.error != 0,
                           "sending to a closed peer reports an error, not a signal");
        return;
    }
    if (result.status == FlushStatus::Error) {
        report_property_violation(__FILE__, __LINE__, "unexpected flush error on an open socket pair");
    }
    EXSIM_FUZZ_REQUIRE(result.bytes_sent <= model.pending_bytes.size(), "cannot send more than is pending");
    bytes_in_flight_to_peer.append(model.pending_bytes, 0, result.bytes_sent);
    model.pending_bytes.erase(0, result.bytes_sent);
    model.read_offset += result.bytes_sent;
    if (model.read_offset == model.write_offset) {
        model.read_offset = 0;
        model.write_offset = 0;
    }
    EXSIM_FUZZ_REQUIRE((result.status == FlushStatus::Complete) == model.pending_bytes.empty(),
                       "Complete exactly when nothing is left pending");
}

// The harness reads what flush_to sent and checks it is the pending prefix.
inline void peer_drain(SocketPairForFuzzing& sockets, std::string& bytes_in_flight_to_peer) {
    if (!sockets.peer_open()) return;
    char received[256];
    for (;;) {
        const ssize_t count = ::recv(sockets.peer_side_fd, received, sizeof received, 0);
        if (count <= 0) break;
        const auto received_size = static_cast<std::size_t>(count);
        EXSIM_FUZZ_REQUIRE(received_size <= bytes_in_flight_to_peer.size() &&
                               same_bytes({received, received_size}, {bytes_in_flight_to_peer.data(), received_size}),
                           "the peer receives exactly the flushed bytes in order");
        bytes_in_flight_to_peer.erase(0, received_size);
    }
    EXSIM_FUZZ_REQUIRE(bytes_in_flight_to_peer.empty(), "everything flushed reached the peer");
}

inline void peer_send(SocketPairForFuzzing& sockets, std::string& bytes_in_flight_to_buffer, FuzzByteReader& reader) {
    const std::string_view bytes = reader.take_text(96);
    if (!sockets.peer_open() || bytes.empty()) return;
    const ssize_t sent = net::send_some(sockets.peer_side_fd, bytes.data(), bytes.size());
    if (sent > 0) bytes_in_flight_to_buffer.append(bytes.substr(0, static_cast<std::size_t>(sent)));
}

}  // namespace stream_buffers_fuzz

inline int fuzz_stream_buffers(const std::uint8_t* data, std::size_t size) {
    using namespace stream_buffers_fuzz;
    FuzzByteReader reader(data, size);
    ReceiveBufferModel receive_model;
    receive_model.capacity = reader.take_byte() % 64;
    SendBufferModel send_model;
    send_model.capacity = reader.take_byte() % 64;
    net::ReceiveBuffer receive_buffer(receive_model.capacity);
    net::SendBuffer send_buffer(send_model.capacity);
    SocketPairForFuzzing sockets;
    std::string bytes_in_flight_to_buffer;
    std::string bytes_in_flight_to_peer;

    for (std::size_t operation_count = 0; !reader.exhausted() && operation_count < 512; ++operation_count) {
        const std::uint8_t opcode = reader.take_byte() % 10;
        if (opcode >= 6 && !sockets.usable()) continue;
        switch (opcode) {
            case 0: receive_buffer_write(receive_buffer, receive_model, reader); break;
            case 1: receive_buffer_consume(receive_buffer, receive_model, reader); break;
            case 2:
                receive_buffer.clear();
                receive_model.read_offset = receive_model.write_offset = 0;
                receive_model.unread_bytes.clear();
                break;
            case 3: send_buffer_reserve(send_buffer, send_model, reader); break;
            case 4: send_buffer_append(send_buffer, send_model, reader); break;
            case 5:
                send_buffer.clear();
                send_model.read_offset = send_model.write_offset = 0;
                send_model.overflowed = false;
                send_model.pending_bytes.clear();
                break;
            case 6: peer_send(sockets, bytes_in_flight_to_buffer, reader); break;
            case 7:
                receive_buffer_receive_from(receive_buffer, receive_model, sockets, bytes_in_flight_to_buffer);
                break;
            case 8:
                send_buffer_flush(send_buffer, send_model, sockets, bytes_in_flight_to_peer);
                peer_drain(sockets, bytes_in_flight_to_peer);
                break;
            default:
                // Close only once the peer has nothing left to read, so a
                // later recv sees an orderly shutdown and a send sees EPIPE.
                if (reader.take_byte() % 4 == 0) {
                    peer_drain(sockets, bytes_in_flight_to_peer);
                    sockets.close_peer();
                }
                break;
        }
        check_receive_buffer_matches(receive_buffer, receive_model);
        check_send_buffer_matches(send_buffer, send_model);
    }
    return 0;
}

}  // namespace exsim::fuzz

#endif
