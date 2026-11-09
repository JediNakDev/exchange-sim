#include "ipc/session_transport.hpp"
#include <algorithm>
namespace exsim::ipc {
void send_session_bytes(int socket, std::span<const char> bytes) {
    Frame header; auto size = static_cast<std::uint32_t>(bytes.size()); header.field(size); send_frame(socket,header);
    while (!bytes.empty()) {
        Frame chunk; chunk.size = std::min(bytes.size(),chunk.bytes.size());
        std::memcpy(chunk.bytes.data(),bytes.data(),chunk.size); send_frame(socket,chunk);
        bytes = bytes.subspan(chunk.size);
    }
}
void receive_session_bytes(int socket, SessionBuffer& buffer) {
    buffer.clear(); auto header = receive_frame(socket); std::uint32_t remaining = 0; header.field(remaining); header.finish();
    if (remaining > (1u << 20)) throw std::runtime_error("oversized research session transfer");
    while (remaining) {
        auto chunk = receive_frame(socket);
        if (chunk.size > remaining) throw std::runtime_error("invalid research session chunk");
        buffer.append({reinterpret_cast<const char*>(chunk.bytes.data()),chunk.size});
        remaining -= static_cast<std::uint32_t>(chunk.size);
    }
}
} // namespace exsim::ipc
