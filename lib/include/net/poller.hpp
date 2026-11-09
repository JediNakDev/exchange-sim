#ifndef EXSIM_NET_POLLER_HPP
#define EXSIM_NET_POLLER_HPP

// Readiness poller: epoll on Linux, kqueue on macOS/BSD, chosen at compile
// time. Level-triggered: a socket stays ready until it is drained (read
// side) or until write interest is dropped (write side), so a loop may do one
// recv per readiness event and still never lose data.
//
// Every registration carries a 64-bit user token that comes back in each
// PollEvent. By convention the top 8 bits are an owner tag (make_poll_token),
// so several components (TCP servers, UDP sockets, clients) can share one
// Poller and the loop routes each event with poll_token_tag(event.token).
//
// wait() writes into a fixed internal array: no allocation per call. A
// timeout of 0 returns immediately, which is how a busy-poll loop spins.

#include <array>
#include <cstdint>
#include <net/socket.hpp>
#include <span>

#if defined(__linux__)
#include <sys/epoll.h>
#define EXSIM_NET_POLLER_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/event.h>
#define EXSIM_NET_POLLER_KQUEUE 1
#else
#error "exsim::net::Poller needs epoll (Linux) or kqueue (macOS/BSD)"
#endif

namespace exsim::net {

static_assert(sizeof(void*) == 8, "poll tokens are carried in a pointer-sized field");

inline constexpr unsigned kPollTokenTagShift = 56;
inline constexpr std::uint64_t kPollTokenValueMask = (std::uint64_t{1} << kPollTokenTagShift) - 1;

constexpr std::uint64_t make_poll_token(std::uint8_t owner_tag, std::uint64_t value) noexcept {
    return (std::uint64_t{owner_tag} << kPollTokenTagShift) | (value & kPollTokenValueMask);
}
constexpr std::uint8_t poll_token_tag(std::uint64_t token) noexcept {
    return static_cast<std::uint8_t>(token >> kPollTokenTagShift);
}
constexpr std::uint64_t poll_token_value(std::uint64_t token) noexcept {
    return token & kPollTokenValueMask;
}

enum class PollInterest : std::uint8_t {
    Read = 1,
    Write = 2,
    ReadWrite = 3,
};

struct PollEvent {
    std::uint64_t token;
    bool readable;
    bool writable;
    // Peer shut down or the connection broke. Readable data may still be
    // queued; the next recv/send reports the details (0 = EOF, or an errno).
    bool hangup;
    bool error;
};

// With kqueue, read and write readiness of one fd arrive as two separate
// events; with epoll as one event with both flags. Handle each flag on its own.
class Poller {
   public:
    static constexpr int kMaxEventsPerWait = 256;

    Poller() noexcept = default;
    ~Poller();
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    // Creates the epoll/kqueue instance.
    ErrorCode open() noexcept;
    bool is_open() const noexcept { return poll_fd_ >= 0; }
    void close() noexcept;

    ErrorCode add(int fd, std::uint64_t token, PollInterest interest) noexcept;
    // Changes the interest (and token) of a registered fd.
    ErrorCode modify(int fd, std::uint64_t token, PollInterest interest) noexcept;
    // Unregister before closing the fd.
    ErrorCode remove(int fd) noexcept;

    // Waits up to timeout_ms (0 = poll and return immediately, -1 = no
    // timeout). Returns the number of ready events, now in events(); 0 on
    // timeout or EINTR; -1 on failure with errno set.
    int wait(int timeout_ms) noexcept;

    // Events from the last wait(), valid until the next one.
    std::span<const PollEvent> events() const noexcept {
        return {ready_events_.data(), static_cast<std::size_t>(ready_event_count_)};
    }

   private:
    int poll_fd_ = -1;
    int ready_event_count_ = 0;
#if defined(EXSIM_NET_POLLER_EPOLL)
    std::array<epoll_event, kMaxEventsPerWait> kernel_events_{};
#else
    std::array<struct kevent, kMaxEventsPerWait> kernel_events_{};
#endif
    std::array<PollEvent, kMaxEventsPerWait> ready_events_{};
};

}  // namespace exsim::net

#endif
