#ifndef EXSIM_FUZZ_TARGETS_OUCH42_OUTBOUND_HPP
#define EXSIM_FUZZ_TARGETS_OUCH42_OUTBOUND_HPP

// OUCH 4.2 exchange-to-client messages (S, A, C, E, J, P, I, M, U), as a
// client or the integration tests decode them. Same properties as inbound.

#include "fixed_layout_codec.hpp"

#include <protocol/ouch42.hpp>

namespace exsim::fuzz {

inline int fuzz_ouch42_outbound(const std::uint8_t* data, std::size_t size) {
    namespace ouch = protocol::ouch42;
    const std::span<const char> input = as_chars(data, size);
    check_fixed_layout_decode<ouch::SystemEvent>(input, {{9, "SE"}});
    check_fixed_layout_decode<ouch::OrderAccepted>(input, {{23, "BSTE"}, {64, "LD"}});
    check_fixed_layout_decode<ouch::OrderCanceled>(input, {});
    check_fixed_layout_decode<ouch::OrderExecuted>(input, {});
    check_fixed_layout_decode<ouch::OrderRejected>(input, {});
    check_fixed_layout_decode<ouch::CancelPending>(input, {});
    check_fixed_layout_decode<ouch::CancelReject>(input, {});
    check_fixed_layout_decode<ouch::OrderModified>(input, {{23, "BSTE"}});
    check_fixed_layout_decode<ouch::OrderReplaced>(input, {{23, "BSTE"}, {64, "LD"}});

    FuzzByteReader reader(data, size);
    switch (reader.take_index(4)) {
        case 0: check_wide_field_encode<ouch::OrderAccepted, 64>(take_base_message<ouch::OrderAccepted>(reader), reader); break;
        case 1: check_wide_field_encode<ouch::OrderExecuted, 64>(take_base_message<ouch::OrderExecuted>(reader), reader); break;
        case 2: check_wide_field_encode<ouch::OrderReplaced, 64>(take_base_message<ouch::OrderReplaced>(reader), reader); break;
        default: check_wide_field_encode<ouch::SystemEvent, 64>(take_base_message<ouch::SystemEvent>(reader), reader); break;
    }
    return 0;
}

}  // namespace exsim::fuzz

#endif
