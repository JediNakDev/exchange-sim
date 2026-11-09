#include "ipc/strategy_wire.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;

namespace exsim::ipc {
namespace {
void transfer(int socket, unsigned char* bytes, std::size_t length, bool writing, bool idle = false) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (length) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (!idle && remaining <= 0) throw std::runtime_error("trader transport deadline exceeded");
        pollfd descriptor{socket, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        const int ready = ::poll(&descriptor, 1, idle ? -1 : static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) throw std::runtime_error("trader transport unavailable");
        int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        const auto count = writing ? ::send(socket, bytes, length, flags) : ::recv(socket, bytes, length, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) throw std::runtime_error("trader transport closed");
        if (idle) deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        idle = false;
        bytes += count;
        length -= static_cast<std::size_t>(count);
    }
}
void visible_fields(Frame& frame, research::StrategyVisibleState& v) {
    frame.fields(v.now,v.instrument,v.risk,v.top_of_book,v.last_market_data,v.has_seen_two_sided_book,
                 v.last_two_sided_mid_times_two,v.known_position,v.known_bought_qty,v.known_sold_qty,v.open_order_count,v.last_risk_reject);
}
}

void Frame::field(research::ExecutionDecision& v) {
    fields(v.time,v.source_time,v.source_sequence,v.target,v.filled,v.requested,v.headroom,v.child_cap,v.submitted,
           v.outstanding,v.child,v.touch,v.limit,v.has_touch,v.report,v.result);
    // Recorder rows retain static strings, never pointers into a receive buffer.
    static constexpr const char* reasons[] = {
        "submitted","child_in_flight","target_satisfied","no_delivered_touch","no_headroom_or_lot",
        "none","invalid_quantity","invalid_price","max_order_quantity","max_open_orders","max_long_position",
        "max_short_position","unknown_order","request_in_flight","not_yet_acknowledged","run_not_active","queue_capacity",
        "accepted","rejected","rested","fill","canceled","replaced"
    };
    std::uint8_t index = 0;
    if (!reading) {
        for (; index < std::size(reasons); ++index) if (std::strcmp(v.reason,reasons[index]) == 0) break;
    }
    field(index);
    if (index >= std::size(reasons)) throw std::runtime_error("invalid decision reason");
    if (reading) v.reason = reasons[index];
}

void send_frame(int socket, const Frame& frame) {
    Frame header;
    std::uint32_t length = static_cast<std::uint32_t>(frame.size);
    header.field(length);
    transfer(socket,header.bytes.data(),header.size,true);
    // send does not modify the supplied bytes.
    transfer(socket,const_cast<unsigned char*>(frame.bytes.data()),frame.size,true);
}
Frame receive_frame(int socket, bool idle) {
    Frame frame;
    transfer(socket,frame.bytes.data(),4,false,idle);
    frame.size = 4; frame.reading = true;
    std::uint32_t length = 0; frame.field(length);
    if (length == 0 || length > frame.bytes.size()) throw std::runtime_error("invalid strategy frame size");
    transfer(socket,frame.bytes.data(),length,false);
    frame.size = length; frame.cursor = 0;
    return frame;
}
std::string daemon_path(const char* name) {
#ifdef __APPLE__
    std::uint32_t length = 0;
    _NSGetExecutablePath(nullptr,&length);
    std::string executable(length,'\0');
    if (_NSGetExecutablePath(executable.data(),&length) != 0) throw std::runtime_error("cannot locate daemon directory");
    return (std::filesystem::canonical(executable.c_str()).parent_path()/name).string();
#else
    return (std::filesystem::canonical("/proc/self/exe").parent_path()/name).string();
#endif
}
TraderProcess::TraderProcess() {
    int sockets[2];
    if (::socketpair(AF_UNIX,SOCK_STREAM,0,sockets) != 0) throw std::runtime_error("cannot create trader socket");
    socket_ = sockets[0];
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    bool actions_ready = false, attributes_ready = false;
    const auto check = [](int error) { if (error) throw std::runtime_error("cannot configure trader process"); };
    try {
        for (int descriptor : sockets) {
            check(::fcntl(descriptor,F_SETFD,FD_CLOEXEC) < 0 ? errno : 0);
#ifdef __APPLE__
            int enabled = 1;
            check(::setsockopt(descriptor,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled)) < 0 ? errno : 0);
#endif
        }
        // Reserve a source descriptor above fd 3 before adding spawn actions.
        const int child_socket = ::fcntl(sockets[1],F_DUPFD_CLOEXEC,10);
        if (child_socket < 0) throw std::runtime_error("cannot reserve trader descriptor");
        ::close(sockets[1]); sockets[1] = child_socket;
        check(::posix_spawn_file_actions_init(&actions)); actions_ready = true;
        check(::posix_spawnattr_init(&attributes)); attributes_ready = true;
        check(::posix_spawn_file_actions_addclose(&actions,sockets[0]));
        check(::posix_spawn_file_actions_adddup2(&actions,sockets[1],3));
        short flags = POSIX_SPAWN_SETSIGMASK;
        sigset_t signals; sigemptyset(&signals);
        check(::posix_spawnattr_setsigmask(&attributes,&signals));
#ifdef __APPLE__
        flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
        check(::posix_spawn_file_actions_addinherit_np(&actions,STDERR_FILENO));
#else
        check(::posix_spawn_file_actions_addclosefrom_np(&actions,4));
#endif
        check(::posix_spawnattr_setflags(&attributes,flags));
        const auto executable = daemon_path("exsim_traderd");
        char* args[] = {const_cast<char*>(executable.c_str()),const_cast<char*>("--strategy-fd"),const_cast<char*>("3"),nullptr};
        // Trader receives no PostgreSQL credentials.
        std::vector<char*> environment;
        for (char** entry = environ; *entry; ++entry) {
            const std::string_view value(*entry);
            if (!value.starts_with("EXSIM_DATABASE_URL=") && !value.starts_with("PGPASSWORD=") && !value.starts_with("PGPASSFILE=")) environment.push_back(*entry);
        }
        environment.push_back(nullptr);
        pid_t child;
        check(::posix_spawn(&child,executable.c_str(),&actions,&attributes,args,environment.data()));
        pid_ = child;
    } catch (...) {
        if (attributes_ready) ::posix_spawnattr_destroy(&attributes);
        if (actions_ready) ::posix_spawn_file_actions_destroy(&actions);
        ::close(sockets[0]); ::close(sockets[1]); socket_ = -1;
        throw;
    }
    ::posix_spawnattr_destroy(&attributes); ::posix_spawn_file_actions_destroy(&actions);
    ::close(sockets[1]);
}
TraderProcess::~TraderProcess() {
    if (socket_ >= 0) ::close(socket_);
    if (pid_ > 0) {
        // The peer has no persistent state; closing/killing cannot lose an
        // exchange artifact. Always reap, including setup and transport errors.
        ::kill(pid_,SIGKILL);
        while (::waitpid(pid_,nullptr,0) < 0 && errno == EINTR) {}
    }
}
void send_visible(int socket, const research::StrategyVisibleState& state) {
    Frame frame;
    visible_fields(frame,const_cast<research::StrategyVisibleState&>(state));
    send_frame(socket,frame);
    std::uint32_t count = 0;
    for (std::size_t i = 0; i < state.order_capacity; ++i) if (state.orders[i].live) ++count;
    Frame count_frame; count_frame.field(count); send_frame(socket,count_frame);
    for (std::size_t i = 0; i < state.order_capacity; ++i) if (state.orders[i].live) {
        Frame order; order.field(state.orders[i]); send_frame(socket,order);
    }
}
void receive_visible(int socket, research::StrategyVisibleState& state) {
    auto frame = receive_frame(socket); visible_fields(frame,state); frame.finish();
    auto count_frame = receive_frame(socket); std::uint32_t count = 0; count_frame.field(count); count_frame.finish();
    if (count > state.risk.max_open_orders || count > state.order_capacity) throw std::runtime_error("invalid visible order count");
    for (std::size_t i = 0; i < state.order_capacity; ++i) state.orders[i] = {};
    for (std::uint32_t i = 0; i < count; ++i) {
        auto order_frame = receive_frame(socket); research::StrategyOrderView order;
        order_frame.field(order); order_frame.finish();
        if (!order.live || order.cl_ord_id == 0) throw std::runtime_error("invalid visible order");
        auto& slot = state.orders[order.cl_ord_id & (state.order_capacity - 1)];
        if (slot.live) throw std::runtime_error("duplicate visible order slot");
        slot = order;
    }
}
} // namespace exsim::ipc

namespace exsim::ipc {
void action_fields(Frame& frame, research::StrategyAction& action) {
    frame.fields(action.kind,action.side,action.price,action.qty,action.tif,action.post_only,action.token,action.time);
    if (action.kind == research::StrategyActionKind::Decision) frame.field(action.decision);
    if (action.kind > research::StrategyActionKind::Done || static_cast<unsigned>(action.side) > 1 ||
        static_cast<unsigned>(action.tif) > 3) throw std::runtime_error("invalid strategy action");
}
} // namespace exsim::ipc
