#ifndef EXSIM_IPC_SESSION_TRANSPORT_HPP
#define EXSIM_IPC_SESSION_TRANSPORT_HPP

#include "exsim/ipc/strategy_wire.hpp"
#include <cstring>
#include <memory>

namespace exsim::ipc {
// Bounded byte storage shared by the existing transport-independent gateways.
class SessionBuffer {
public:
    explicit SessionBuffer(std::size_t capacity = 1u << 20)
        : storage_(std::make_unique<char[]>(capacity)), capacity_(capacity) {}
    char* prepare(std::size_t size) noexcept {
        if (!size || size > capacity_ - used_) { failed_ = true; return nullptr; }
        return storage_.get() + used_;
    }
    void commit(std::size_t size) noexcept { used_ += size; }
    void append(std::span<const char> bytes) {
        if (bytes.empty()) return;
        char* out = prepare(bytes.size());
        if (!out) throw std::runtime_error("research session byte capacity exceeded");
        std::memcpy(out,bytes.data(),bytes.size()); commit(bytes.size());
    }
    std::span<const char> bytes() const noexcept { return {storage_.get(),used_}; }
    bool failed() const noexcept { return failed_; }
    void clear() noexcept { used_ = 0; }
private:
    std::unique_ptr<char[]> storage_;
    std::size_t capacity_, used_ = 0;
    bool failed_ = false;
};
struct SessionSink {
    SessionBuffer* buffer = nullptr;
    char* prepare(std::size_t size) noexcept { return buffer->prepare(size); }
    void commit(std::size_t size) noexcept { buffer->commit(size); }
};
// The synchronization envelope preserves stream bytes and datagram boundaries;
// session/application framing remains SoupBinTCP, FIX, SOFH/FIXP and MoldUDP64.
void send_session_bytes(int socket, std::span<const char> bytes);
void receive_session_bytes(int socket, SessionBuffer& buffer);
} // namespace exsim::ipc
#endif
