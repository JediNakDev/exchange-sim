#ifndef EXSIM_RESEARCH_EXCHANGE_SESSION_HPP
#define EXSIM_RESEARCH_EXCHANGE_SESSION_HPP
#include "exsim/research/simulation_core.hpp"
#include "exsim/ipc/session_transport.hpp"
namespace exsim::research {
struct SessionStatistics {
    bool established = false;
    std::uint64_t decoded_orders = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t report_bytes = 0;
    std::uint64_t itch_messages = 0;
    std::uint64_t retransmission_requests = 0;
    std::uint64_t reports_dropped = 0;
    std::uint64_t packets_dropped = 0;
};
// Full exchange gateway/session state, attached to the research scheduler.
class ExchangeSession {
public:
    explicit ExchangeSession(SimulationCore& simulation);
    ~ExchangeSession();
    ExchangeSession(const ExchangeSession&) = delete;
    ExchangeSession& operator=(const ExchangeSession&) = delete;
    std::uint64_t receive_request(int socket);
    void send_control(int socket);
    void send_report(int socket, std::uint64_t sequence);
    void send_market(int socket, std::uint64_t sequence);
    void check() const;
    void tick();
    SessionStatistics statistics() const noexcept;
    void drop_next_report() noexcept;
    void drop_next_market_packet() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace exsim::research
#endif
