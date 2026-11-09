#ifndef EXSIM_RESEARCH_RUN_SUMMARY_HPP
#define EXSIM_RESEARCH_RUN_SUMMARY_HPP

// summary.json: end-of-run accounting, order/fill statistics, markouts,
// execution quality and latency, computed from the core's exact integer
// accumulators. Floating-point values are derived here, after the run.
// Every field is documented in research runner.

#include "research/simulation_core.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace exsim::research {

inline constexpr std::string_view kSummarySchema = "exsim.research.summary/1";

struct StrategyIdentity {
    std::string_view name;
    std::uint32_t version = 0;
};

// End-of-run valuation with the declared mark convention, exact integers in
// doubled price units (the exchange-mid mark may fall on half a price unit).
struct FinalValuation {
    bool has_mark = false;
    std::int64_t mark_times_two = 0;
    bool has_equity = false;        // mark available, or flat
    std::int64_t unrealized_pnl_times_two = 0;
    std::int64_t equity_times_two = 0;
    std::int64_t net_pnl_times_two = 0;  // equity - initial_cash
    std::int64_t open_cost_basis = 0;    // price units, signed: sum of open lot price * qty (negative when short)
};

[[nodiscard]] FinalValuation final_valuation(const SimulationCore& core) noexcept;

// Accounting identities checked at the end of every run (reported in summary.json).
struct ReconciliationResult {
    bool cash_matches_fills = false;      // cash == initial - buy notional + sell notional - fees
    bool position_matches_fills = false;  // position == bought - sold
    bool fees_match_fills = false;        // ledger fees == maker + taker + auction fees
    // equity - initial == realized + unrealized - fees, and (for a mark on the
    // price grid) unrealized equals the ledger's own FIFO lot valuation.
    bool pnl_identity_holds = false;
};

[[nodiscard]] ReconciliationResult reconcile(const SimulationCore& core) noexcept;

[[nodiscard]] std::string render_summary_json(const SimulationCore& core, const StrategyIdentity& strategy,
                                              const std::string& run_id, bool replay_verified);

}  // namespace exsim::research

#endif
