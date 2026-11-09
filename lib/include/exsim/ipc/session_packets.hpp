#ifndef EXSIM_IPC_SESSION_PACKETS_HPP
#define EXSIM_IPC_SESSION_PACKETS_HPP
#include "exsim/ipc/session_transport.hpp"
#include <algorithm>
#include "exsim/protocol/wire.hpp"
namespace exsim::ipc {
// FIFO chunks keyed by the existing scheduler's delivery sequence. Stream
// chunks may split a frame; each market-data chunk is one complete datagram.
class SessionPackets {
    struct Packet { std::uint64_t key = 0; std::uint16_t size = 0; std::array<char,1024> bytes{}; };
public:
    explicit SessionPackets(std::size_t capacity) : packets_(std::make_unique<Packet[]>(capacity)), capacity_(capacity) {}
    bool append(std::uint64_t key, std::span<const char> bytes) noexcept {
        if (bytes.empty()) return true;
        if (bytes.size() > 1024 || used_ == capacity_) { failed_ = true; return false; }
        auto& packet = packets_[(first_ + used_) % capacity_];
        packet.key = key; packet.size = static_cast<std::uint16_t>(bytes.size());
        std::memcpy(packet.bytes.data(),bytes.data(),bytes.size()); ++used_; return true;
    }
    void stream(std::uint64_t key, std::span<const char> bytes) noexcept {
        while (!bytes.empty()) { const auto size = std::min<std::size_t>(1024,bytes.size()); if (!append(key,bytes.first(size))) return; bytes = bytes.subspan(size); }
    }
    bool send(int socket, std::uint64_t key,bool drop_first = false,bool datagram = false) {
        if (failed_ || (used_ && packets_[first_].key < key)) throw std::runtime_error("research protocol delivery queue failed");
        std::uint32_t count = 0;
        while (count < used_ && packets_[(first_ + count) % capacity_].key == key) ++count;
        const bool dropped = drop_first && count != 0;
        std::array<char,20> heartbeat{};
        if (dropped && datagram) {
            const auto& last = packets_[(first_+count-1)%capacity_];
            if (last.size < 20) throw std::runtime_error("invalid queued MoldUDP64 packet");
            std::memcpy(heartbeat.data(),last.bytes.data(),20);
            const auto next = protocol::wire::load<8>(heartbeat.data()+10) + protocol::wire::load<2>(heartbeat.data()+18);
            protocol::wire::store<8>(heartbeat.data()+10,next); protocol::wire::store<2>(heartbeat.data()+18,0);
        }
        auto sent_count = count - (dropped ? 1u : 0u) + (dropped && datagram ? 1u : 0u);
        Frame header; header.field(sent_count); send_frame(socket,header);
        bool first_packet = true;
        while (count--) {
            const auto& packet = packets_[first_];
            if (!(first_packet && dropped)) send_session_bytes(socket,{packet.bytes.data(),packet.size});
            first_packet = false;
            first_ = (first_ + 1) % capacity_; --used_;
        }
        // Advertise only the end of THIS delivered batch, never the publisher's
        // current (possibly future) head. The receiver detects and requests
        // the missing datagram through its ordinary MoldUDP64 recovery path.
        if (dropped && datagram) send_session_bytes(socket,heartbeat);
        return dropped;
    }
    bool empty() const noexcept { return used_ == 0; }
    std::uint64_t last_key() const noexcept { return packets_[(first_+used_-1)%capacity_].key; }
    void clear() noexcept { first_ = used_ = 0; }
    bool failed() const noexcept { return failed_; }
private:
    std::unique_ptr<Packet[]> packets_;
    std::size_t capacity_, first_ = 0, used_ = 0;
    bool failed_ = false;
};
template<class Consumer>
void receive_session_packets(int socket, SessionBuffer& buffer, Consumer&& consume) {
    auto header = receive_frame(socket); std::uint32_t count = 0; header.field(count); header.finish();
    if (count > 65536) throw std::runtime_error("oversized research packet batch");
    while (count--) { receive_session_bytes(socket,buffer); consume(buffer.bytes()); }
}
} // namespace exsim::ipc
#endif
