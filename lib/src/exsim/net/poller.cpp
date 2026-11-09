#include <fcntl.h>
#include <unistd.h>

#include <net/poller.hpp>
namespace exsim::net {
Poller::~Poller() {
    close();
}
ErrorCode Poller::open() noexcept {
    if (is_open()) return EALREADY;
#ifdef EXSIM_NET_POLLER_EPOLL
    poll_fd_ = epoll_create1(EPOLL_CLOEXEC);
#else
    poll_fd_ = kqueue();
    if (poll_fd_ >= 0 && fcntl(poll_fd_, F_SETFD, FD_CLOEXEC) < 0) {
        const int error = errno;
        close();
        return error;
    }
#endif
    return poll_fd_ < 0 ? errno : 0;
}
void Poller::close() noexcept {
    if (is_open()) ::close(poll_fd_);
    poll_fd_ = -1;
    ready_event_count_ = 0;
}
#ifdef EXSIM_NET_POLLER_EPOLL
namespace {
ErrorCode change(int poll_fd, int operation, int fd, std::uint64_t token, PollInterest interest) noexcept {
    epoll_event event{};
    event.data.u64 = token;
    event.events = EPOLLRDHUP;
    if (static_cast<unsigned>(interest) & 1) event.events |= EPOLLIN;
    if (static_cast<unsigned>(interest) & 2) event.events |= EPOLLOUT;
    return epoll_ctl(poll_fd, operation, fd, &event) < 0 ? errno : 0;
}
}  // namespace
ErrorCode Poller::add(int fd, std::uint64_t token, PollInterest interest) noexcept {
    return change(poll_fd_, EPOLL_CTL_ADD, fd, token, interest);
}
ErrorCode Poller::modify(int fd, std::uint64_t token, PollInterest interest) noexcept {
    return change(poll_fd_, EPOLL_CTL_MOD, fd, token, interest);
}
ErrorCode Poller::remove(int fd) noexcept {
    return epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0 ? errno : 0;
}
#else
namespace {
ErrorCode change(int poll_fd, int fd, std::uint64_t token, PollInterest interest) noexcept {
    // Install both filters so modify only changes enablement and tokens.
    for (int bit = 1; bit <= 2; ++bit) {
        struct kevent event{};
        EV_SET(&event, fd, bit == 1 ? EVFILT_READ : EVFILT_WRITE,
               EV_ADD | ((static_cast<unsigned>(interest) & bit) ? EV_ENABLE : EV_DISABLE), 0, 0,
               reinterpret_cast<void*>(static_cast<std::uintptr_t>(token)));
        if (kevent(poll_fd, &event, 1, nullptr, 0, nullptr) < 0) return errno;
    }
    return 0;
}
}  // namespace
ErrorCode Poller::add(int fd, std::uint64_t token, PollInterest interest) noexcept {
    const int error = change(poll_fd_, fd, token, interest);
    if (error) remove(fd);
    return error;
}
ErrorCode Poller::modify(int fd, std::uint64_t token, PollInterest interest) noexcept {
    return change(poll_fd_, fd, token, interest);
}
ErrorCode Poller::remove(int fd) noexcept {
    int error = 0;
    for (auto filter : {EVFILT_READ, EVFILT_WRITE}) {
        struct kevent event{};
        EV_SET(&event, fd, filter, EV_DELETE, 0, 0, nullptr);
        if (kevent(poll_fd_, &event, 1, nullptr, 0, nullptr) < 0 && errno != ENOENT) error = errno;
    }
    return error;
}
#endif
int Poller::wait(int timeout_ms) noexcept {
    ready_event_count_ = 0;
    if (timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }
#ifdef EXSIM_NET_POLLER_EPOLL
    const int count = epoll_wait(poll_fd_, kernel_events_.data(), kMaxEventsPerWait, timeout_ms);
#else
    timespec timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
    const int count =
        kevent(poll_fd_, nullptr, 0, kernel_events_.data(), kMaxEventsPerWait, timeout_ms < 0 ? nullptr : &timeout);
#endif
    if (count < 0) return errno == EINTR ? 0 : -1;
    for (int index = 0; index < count; ++index) {
        const auto& event = kernel_events_[index];
#ifdef EXSIM_NET_POLLER_EPOLL
        ready_events_[index] = {event.data.u64, bool(event.events & EPOLLIN), bool(event.events & EPOLLOUT),
                                bool(event.events & (EPOLLRDHUP | EPOLLHUP)), bool(event.events & EPOLLERR)};
#else
        ready_events_[index] = {static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(event.udata)),
                                event.filter == EVFILT_READ, event.filter == EVFILT_WRITE, bool(event.flags & EV_EOF),
                                bool(event.flags & EV_ERROR)};
#endif
    }
    ready_event_count_ = count;
    return count;
}
}  // namespace exsim::net
