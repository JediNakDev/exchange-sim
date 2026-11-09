#ifndef EXSIM_IPC_STRATEGY_WIRE_HPP
#define EXSIM_IPC_STRATEGY_WIRE_HPP

#include "exsim/research/simulation_core.hpp"
#include <array>
#include <bit>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace exsim::ipc {
// Research synchronization protocol v1. All integers are explicitly encoded
// big endian. This is a research protocol, not OUCH or ITCH session framing.
class Frame {
public:
    std::array<unsigned char, 1024> bytes{};
    std::size_t size = 0;
    std::size_t cursor = 0;
    bool reading = false;

    template<class T> requires (std::is_integral_v<T> || std::is_enum_v<T>)
    void field(T& value) {
        constexpr auto width = sizeof(T);
        if (reading) {
            if (cursor + width > size) throw std::runtime_error("truncated strategy frame");
            std::uint64_t bits = 0;
            for (std::size_t i = 0; i < width; ++i) bits = (bits << 8) | bytes[cursor++];
            if constexpr (std::is_same_v<T, bool>) {
                if (bits > 1) throw std::runtime_error("invalid wire boolean");
                value = bits != 0;
            } else if constexpr (std::is_signed_v<T>) value = std::bit_cast<T>(static_cast<std::make_unsigned_t<T>>(bits));
            else value = static_cast<T>(bits);
        } else {
            if (size + width > bytes.size()) throw std::runtime_error("strategy frame capacity exceeded");
            const auto bits = static_cast<std::uint64_t>(value);
            for (std::size_t i = 0; i < width; ++i) bytes[size++] = static_cast<unsigned char>(bits >> (8 * (width - i - 1)));
        }
    }
    template<class... T> void fields(T&... values) { (field(values), ...); }
    void finish() const { if (reading && cursor != size) throw std::runtime_error("trailing strategy frame bytes"); }
    void field(Symbol& v) { for (auto& c : v.chars) field(c); }
    void field(InstrumentConfig& v) { fields(v.symbol,v.tick_size,v.lot_size,v.max_order_qty,v.min_price,v.max_price,v.reference_price,v.initial_phase); }
    void field(research::QuotingParameters& v) { fields(v.half_spread_ticks,v.quote_size,v.requote_interval_ns,v.requote_threshold_ticks,v.post_only); }
    void field(research::InventorySkewParameters& v) { fields(v.skew_ticks_per_unit,v.skew_unit,v.scale_size_with_inventory); }
    void field(research::ParentOrderParameters& v) { fields(v.side,v.parent_qty,v.start_ns,v.execution_duration_ns,v.slice_interval_ns,v.limit_offset_ticks); }
    void field(research::StrategyConfig& v) { fields(v.kind,v.stp,v.quoting,v.inventory,v.parent); }
    void field(research::RiskLimits& v) { fields(v.max_long_position,v.max_short_position,v.max_order_qty,v.max_open_orders); }
    void field(research::TopOfBook& v) { fields(v.has_bid,v.has_ask,v.bid_price,v.ask_price,v.bid_qty,v.ask_qty); }
    void field(research::MarketDataUpdate& v) { fields(v.sequence,v.exchange_time,v.top,v.trade_count,v.traded_qty,v.last_trade_price,v.last_trade_aggressor); }
    void field(research::ExecutionReport& v) { fields(v.kind,v.exchange_time,v.cl_ord_id,v.order_id,v.side,v.price,v.qty,v.leaves_qty,v.cum_qty,v.liquidity,v.match_id,v.rejected_command,v.reject_reason,v.cancel_reason,v.priority_kept); }
    void field(research::StrategyOrderView& v) { fields(v.live,v.state,v.pending,v.cancel_after_ack,v.side,v.tif,v.cl_ord_id,v.order_id,v.price,v.total_qty,v.leaves_qty,v.cum_qty,v.requested_price,v.requested_total_qty,v.submitted_at); }
    void field(research::ExecutionDecision& v);
};

void send_frame(int socket, const Frame& frame);
void action_fields(Frame& frame, research::StrategyAction& action);
Frame receive_frame(int socket, bool idle = false);
// Resolve sibling executables without relying on cwd or PATH.
std::string daemon_path(const char* name);

class TraderProcess {
public:
    TraderProcess();
    ~TraderProcess();
    TraderProcess(const TraderProcess&) = delete;
    TraderProcess& operator=(const TraderProcess&) = delete;
    int socket() const noexcept { return socket_; }
    int pid() const noexcept { return pid_; }
private:
    int socket_ = -1;
    int pid_ = -1;
};

void send_visible(int socket, const research::StrategyVisibleState& state);
void receive_visible(int socket, research::StrategyVisibleState& state);

} // namespace exsim::ipc
#endif
