#ifndef EXSIM_RESEARCH_STRATEGIES_FIXED_SPREAD_MARKET_MAKER_HPP
#define EXSIM_RESEARCH_STRATEGIES_FIXED_SPREAD_MARKET_MAKER_HPP

// Baseline: quote one bid and one ask half_spread_ticks around the most
// recent two-sided mid the strategy has SEEN (delivered market data).
//   bid = floor_to_tick(mid - half_spread), ask = ceil_to_tick(mid + half_spread)
// Post-only quotes are kept one tick inside the visible opposite side.
// Sizes are quote_size, reduced to the pre-trade position headroom.
// Requotes on every delivered market-data update, on fills/cancels/rejects,
// and every requote_interval (wakeup).

#include "research/experiment_config.hpp"
#include "research/strategies/price_grid.hpp"
#include "research/strategies/quote_manager.hpp"
#include "research/strategy_context.hpp"
#include "research/strategy_messages.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace exsim::research::strategies {

// Bid/ask around a (doubled) center, half_spread ticks away, inside the band
// and (for post-only quotes) strictly away from the visible opposite side.
[[nodiscard]] inline std::pair<Price, Price> quote_prices_around(std::int64_t center_times_two, std::uint32_t half_spread_ticks,
                                                                 const StrategyContext& context, bool post_only) noexcept {
    const auto& instrument = context.instrument();
    const Price tick = instrument.tick_size;
    const std::int64_t offset_times_two = 2 * static_cast<std::int64_t>(half_spread_ticks) * tick;
    Price bid = floor_half_to_tick(center_times_two - offset_times_two, tick);
    Price ask = ceil_half_to_tick(center_times_two + offset_times_two, tick);
    const TopOfBook& visible = context.top_of_book();
    if (post_only) {
        if (visible.has_ask) bid = std::min(bid, visible.ask_price - tick);
        if (visible.has_bid) ask = std::max(ask, visible.bid_price + tick);
    }
    return {clamp_to_band(bid, instrument), clamp_to_band(ask, instrument)};
}

class FixedSpreadMarketMaker {
public:
    static constexpr std::string_view kStrategyName = "fixed_spread_market_maker";
    static constexpr std::uint32_t kStrategyVersion = 1;

    explicit FixedSpreadMarketMaker(const StrategyConfig& config) noexcept : parameters_(config.quoting), quotes_(config.quoting) {}

    void on_start(StrategyContext& context) noexcept { context.wake_at(context.now() + parameters_.requote_interval_ns); }
    void on_market_data(const MarketDataUpdate&, StrategyContext& context) noexcept { requote(context); }
    void on_execution_report(const ExecutionReport& report, StrategyContext& context) noexcept {
        if (report.kind == ReportKind::Fill || report.kind == ReportKind::Canceled || report.kind == ReportKind::Rejected)
            requote(context);
    }
    void on_wakeup(StrategyContext& context) noexcept {
        requote(context);
        context.wake_at(context.now() + parameters_.requote_interval_ns);
    }

private:
    void requote(StrategyContext& context) noexcept {
        if (!context.has_seen_two_sided_book()) return;
        const auto [bid_price, ask_price] =
            quote_prices_around(context.last_two_sided_mid_times_two(), parameters_.half_spread_ticks, context, parameters_.post_only);
        const Quantity lot = context.instrument().lot_size;
        const Quantity bid_qty = round_down_to_lot(
            std::min<std::int64_t>(parameters_.quote_size, context.position_headroom(Side::BUY, quotes_.bid_order())), lot);
        const Quantity ask_qty = round_down_to_lot(
            std::min<std::int64_t>(parameters_.quote_size, context.position_headroom(Side::SELL, quotes_.ask_order())), lot);
        quotes_.apply(context, QuoteTarget{bid_price, bid_qty}, QuoteTarget{ask_price, ask_qty});
    }

    QuotingParameters parameters_;
    QuoteManager quotes_;
};

}  // namespace exsim::research::strategies

#endif
