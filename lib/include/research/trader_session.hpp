#ifndef EXSIM_RESEARCH_TRADER_SESSION_HPP
#define EXSIM_RESEARCH_TRADER_SESSION_HPP
#include "research/simulation_core.hpp"
#include "ipc/session_transport.hpp"
namespace exsim::research {
class TraderSession {
public:
    explicit TraderSession(const ExperimentConfig& config);
    ~TraderSession();
    void connect();
    void tick(Timestamp now);
    void reconnect(Timestamp now);
    bool active() const;
    bool order_action(const StrategyAction& action,const StrategyVisibleState& visible);
    void accept_result(const StrategyAction& action,std::uint64_t result);
    bool has_output() const;
    // Writes a channel marker and the corresponding real protocol bytes.
    void send_request(int socket);
    void receive_control(int socket,Timestamp now);
    void receive_report(int socket,ExecutionReport& report,Timestamp now);
    void receive_market(int socket,MarketDataUpdate& update,Timestamp now);
    void finish_report(ExecutionReport& report);
    void finish_market(MarketDataUpdate& update);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace exsim::research
#endif
