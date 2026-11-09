#ifndef EXSIM_API_REQUEST_DIAGNOSTICS_HPP
#define EXSIM_API_REQUEST_DIAGNOSTICS_HPP
#include "api/canonical_event_log.hpp"
namespace exsim::api {
bool write_fifo_evidence(ApiJsonWriter& json, std::span<const CanonicalEventRecord> events, std::span<const OrderId> ids, std::uint64_t last);
void write_request_diagnostics(ApiJsonWriter& json, std::span<const CanonicalEventRecord> events);
void write_replace_transits(ApiJsonWriter& json, std::span<const CanonicalEventRecord> events, std::span<const std::uint64_t> positions);
}
#endif
