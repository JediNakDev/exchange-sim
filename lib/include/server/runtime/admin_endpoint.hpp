#ifndef EXSIM_SERVER_RUNTIME_ADMIN_ENDPOINT_HPP
#define EXSIM_SERVER_RUNTIME_ADMIN_ENDPOINT_HPP

// Admin console (server contract 4): a line protocol over TCP, loopback by
// default. Each received line goes to core.execute_line; the reply lines
// (ending with OK or ERR) are queued on the same connection. This is the
// operator path: it may allocate, and it runs on the server thread between
// gateway events, so admin commands are ordered with order entry.

#include "net/poller.hpp"
#include "net/tcp_server.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace exsim::server::runtime {

template <class Core>
class AdminEndpoint {
public:
    // Longest admin line accepted; a longer one closes the connection.
    static constexpr std::size_t kMaxLineBytes = 4096;

    AdminEndpoint(const net::TcpServerConfig& tcp_config, net::Poller& poller, std::uint8_t poll_tag, Core& core)
        : tcp_server_(tcp_config, poller, poll_tag), core_(core), pending_lines_(tcp_config.max_connections) {}

    [[nodiscard]] net::ErrorCode open() noexcept { return tcp_server_.open(); }
    [[nodiscard]] net::Ipv4Endpoint listen_endpoint() const noexcept { return tcp_server_.listen_endpoint(); }
    void handle_event(const net::PollEvent& event) { tcp_server_.handle_event(event, *this); }
    void flush() { tcp_server_.flush_pending(*this); }
    [[nodiscard]] std::uint64_t commands_executed() const noexcept { return commands_executed_; }

    // ---- net::TcpServerHandler ------------------------------------------------------
    void on_connection_opened(net::ConnectionId id, net::Ipv4Endpoint) noexcept { pending_lines_[id.slot].clear(); }

    // TcpServer calls this from its noexcept event path, so an exception from
    // the operator path (std::bad_alloc while buffering a line or building a
    // reply) is contained here: that connection gets ERR and is closed, the
    // reactor keeps running.
    void on_connection_data(net::ConnectionId id, net::ReceiveBuffer& received) noexcept {
        try {
            execute_received_lines(id, received);
        } catch (...) {
            pending_lines_[id.slot].clear();
            received.consume(received.size());
            tcp_server_.send(id, std::string_view("ERR internal error\n"));
            tcp_server_.close_after_flush(id);
        }
    }

    void on_connection_closed(net::ConnectionId id, net::CloseReason, net::ErrorCode) noexcept {
        pending_lines_[id.slot].clear();
    }

private:
    void execute_received_lines(net::ConnectionId id, net::ReceiveBuffer& received) {
        std::string& pending = pending_lines_[id.slot];
        const std::span<const char> bytes = received.readable();
        pending.append(bytes.data(), bytes.size());
        received.consume(bytes.size());
        std::size_t line_start = 0;
        for (;;) {
            const std::size_t newline = pending.find('\n', line_start);
            if (newline == std::string::npos) break;
            if (newline - line_start > kMaxLineBytes) {
                tcp_server_.send(id, std::string_view("ERR line too long\n"));
                tcp_server_.close_after_flush(id);
                pending.clear();
                return;
            }
            std::string_view line(pending.data() + line_start, newline - line_start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            line_start = newline + 1;
            if (line.find_first_not_of(" \t") == std::string_view::npos) continue;
            reply_.clear();
            core_.execute_line(line, reply_);
            ++commands_executed_;
            if (!tcp_server_.send(id, reply_)) {
                tcp_server_.close_now(id);
                pending.clear();
                return;
            }
        }
        pending.erase(0, line_start);
        if (pending.size() > kMaxLineBytes) {
            tcp_server_.send(id, std::string_view("ERR line too long\n"));
            tcp_server_.close_after_flush(id);
            pending.clear();
        }
    }

    net::TcpServer tcp_server_;
    Core& core_;
    std::vector<std::string> pending_lines_;  // per connection slot
    std::string reply_;
    std::uint64_t commands_executed_ = 0;
};

}  // namespace exsim::server::runtime

#endif
