#ifndef EXSIM_API_MARKOUT_VALUES_HPP
#define EXSIM_API_MARKOUT_VALUES_HPP
#include "api/api_json.hpp"
#include "research/run_metrics.hpp"
#include <optional>
namespace exsim::api {
inline std::optional<std::string> markout_total_value(const research::MarkoutAccumulator& row) {
    if (!row.measured_qty) return std::nullopt;
    return format_doubled_price_units(row.total_times_two);
}
inline std::optional<std::string> markout_per_share_value(const research::MarkoutAccumulator& row) {
    if (!row.measured_qty) return std::nullopt;
    return format_derived_decimal(static_cast<double>(row.total_times_two) / 20000.0 / static_cast<double>(row.measured_qty));
}
}
#endif
