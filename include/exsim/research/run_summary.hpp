#ifndef EXSIM_RESEARCH_RUN_SUMMARY_HPP
#define EXSIM_RESEARCH_RUN_SUMMARY_HPP

// summary.json: end-of-run accounting, order/fill statistics, markouts,
// execution quality and latency, computed from the core's exact integer
// accumulators. Floating-point values are derived here, after the run.
// Every field is documented in docs/RESEARCH_RUNNER.md.

#include "exsim/research/simulation_core.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace exsim::research {

inline constexpr std::string_view kSummarySchema = "exsim.research.summary/1";

struct StrategyIdentity {
    std::string_view name;
    std::uint32_t version = 0;
};

// End-of-run valuation with the declared mark convention, exact integers.
struct FinalValuation {
    bool has_mark = false;
    Price mark_price = 0;
    bool has_equity = false;        // mark available, or flat
    std::int64_t unrealized_pnl = 0;
    std::int64_t equity = 0;
    std::int64_t net_pnl = 0;       // equity - initial_cash
    std::int64_t open_cost_basis = 0;  // signed: sum of open lot price * qty (negative when short)
};

[[nodiscard]] FinalValuation final_valuation(const SimulationCore& core) noexcept;

// Accounting identities checked at the end of every run (reported in summary.json).
struct ReconciliationResult {
    bool cash_matches_fills = false;      // cash == initial - buy notional + sell notional - fees
    bool position_matches_fills = false;  // position == bought - sold
    bool fees_match_fills = false;        // ledger fees == maker + taker + auction fees
    bool pnl_identity_holds = false;      // equity - initial == realized + unrealized - fees (or no equity)
};

[[nodiscard]] ReconciliationResult reconcile(const SimulationCore& core) noexcept;

[[nodiscard]] std::string render_summary_json(const SimulationCore& core, const StrategyIdentity& strategy,
                                              const std::string& run_id, bool replay_verified);

}  // namespace exsim::research

#endif
