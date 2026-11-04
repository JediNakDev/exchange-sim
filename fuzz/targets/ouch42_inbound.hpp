#ifndef EXSIM_FUZZ_TARGETS_OUCH42_INBOUND_HPP
#define EXSIM_FUZZ_TARGETS_OUCH42_INBOUND_HPP

// OUCH 4.2 client-to-exchange messages (O, U, X, M). The whole fuzz input is
// one candidate message; every inbound decoder sees it, so each one must
// reject the other types and wrong lengths. The input is then reused as a
// structured value source for the encode direction.

#include "fixed_layout_codec.hpp"

#include <exsim/protocol/ouch42.hpp>

namespace exsim::fuzz {

inline int fuzz_ouch42_inbound(const std::uint8_t* data, std::size_t size) {
    namespace ouch = protocol::ouch42;
    const std::span<const char> input = as_chars(data, size);
    check_fixed_layout_decode<ouch::EnterOrder>(input);
    check_fixed_layout_decode<ouch::ReplaceOrder>(input);
    check_fixed_layout_decode<ouch::CancelOrder>(input);
    check_fixed_layout_decode<ouch::ModifyOrder>(input);

    FuzzByteReader reader(data, size);
    switch (reader.take_index(2)) {
        case 0: check_wide_field_encode<ouch::EnterOrder, 64>(take_base_message<ouch::EnterOrder>(reader), reader); break;
        default: check_wide_field_encode<ouch::ReplaceOrder, 64>(take_base_message<ouch::ReplaceOrder>(reader), reader); break;
    }
    return 0;
}

}  // namespace exsim::fuzz

#endif
