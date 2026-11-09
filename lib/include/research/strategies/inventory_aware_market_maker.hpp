#ifndef EXSIM_RESEARCH_STRATEGIES_INVENTORY_AWARE_MARKET_MAKER_HPP
#define EXSIM_RESEARCH_STRATEGIES_INVENTORY_AWARE_MARKET_MAKER_HPP

// Baseline: the fixed-spread quoter with an inventory skew, using only the
// position the strategy KNOWS (delivered fills).
//   center = visible mid - skew_ticks_per_unit * known_position / skew_unit ticks
//            (truncated toward zero; long inventory lowers both quotes)
//   with scale_size_with_inventory, the side that adds inventory is scaled by
//   (limit - |inventory on that side|) / limit; the reducing side keeps quote_size.

#include "research/experiment_config.hpp"
#include "research/strategies/fixed_spread_market_maker.hpp"
#include "research/strategies/price_grid.hpp"
#include "research/strategies/quote_manager.hpp"
#include "research/strategy_context.hpp"
#include "research/strategy_messages.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace exsim::research::strategies {

class InventoryAwareMarketMaker {
public:
    static constexpr std::string_view kStrategyName = "inventory_aware_market_maker";
    static constexpr std::uint32_t kStrategyVersion = 1;

    explicit InventoryAwareMarketMaker(const StrategyConfig& config) noexcept
        : parameters_(config.quoting), inventory_(config.inventory), quotes_(config.quoting) {}

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
    [[nodiscard]] Quantity scaled_size(std::int64_t inventory_on_side, std::int64_t limit, Quantity lot) const noexcept {
        if (!inventory_.scale_size_with_inventory || inventory_on_side <= 0) return parameters_.quote_size;
        if (limit <= 0 || inventory_on_side >= limit) return 0;
        const std::int64_t scaled = static_cast<std::int64_t>(parameters_.quote_size) * (limit - inventory_on_side) / limit;
        return round_down_to_lot(scaled, lot);
    }

    void requote(StrategyContext& context) noexcept {
        if (!context.has_seen_two_sided_book()) return;
        const auto& instrument = context.instrument();
        const std::int64_t position = context.known_position();
        const std::int64_t skew_ticks =
            position / static_cast<std::int64_t>(inventory_.skew_unit) * static_cast<std::int64_t>(inventory_.skew_ticks_per_unit);
        const std::int64_t center_times_two = context.last_two_sided_mid_times_two() - 2 * skew_ticks * instrument.tick_size;
        const auto [bid_price, ask_price] =
            quote_prices_around(center_times_two, parameters_.half_spread_ticks, context, parameters_.post_only);
        const auto& risk = context.risk_limits();
        const Quantity lot = instrument.lot_size;
        const Quantity bid_size = scaled_size(position, risk.max_long_position, lot);
        const Quantity ask_size = scaled_size(-position, risk.max_short_position, lot);
        const Quantity bid_qty =
            round_down_to_lot(std::min<std::int64_t>(bid_size, context.position_headroom(Side::BUY, quotes_.bid_order())), lot);
        const Quantity ask_qty =
            round_down_to_lot(std::min<std::int64_t>(ask_size, context.position_headroom(Side::SELL, quotes_.ask_order())), lot);
        quotes_.apply(context, QuoteTarget{bid_price, bid_qty}, QuoteTarget{ask_price, ask_qty});
    }

    QuotingParameters parameters_;
    InventorySkewParameters inventory_;
    QuoteManager quotes_;
};

}  // namespace exsim::research::strategies

#endif
