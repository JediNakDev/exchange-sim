#ifndef EXSIM_FUZZ_TARGETS_ITCH50_HPP
#define EXSIM_FUZZ_TARGETS_ITCH50_HPP

// ITCH 5.0 market data messages. Timestamps are 48 bits on the wire, prices
// 32 bits; the structured direction checks encode() refuses anything wider.

#include "fixed_layout_codec.hpp"

#include <exsim/protocol/itch50.hpp>

namespace exsim::fuzz {

inline int fuzz_itch50(const std::uint8_t* data, std::size_t size) {
    namespace itch = protocol::itch50;
    const std::span<const char> input = as_chars(data, size);
    check_fixed_layout_decode<itch::SystemEvent>(input, {{11, "OSQMEC"}});
    check_fixed_layout_decode<itch::StockDirectory>(input, {});
    check_fixed_layout_decode<itch::StockTradingAction>(input, {});
    check_fixed_layout_decode<itch::AddOrder>(input, {{19, "BS"}});
    check_fixed_layout_decode<itch::AddOrderMPID>(input, {{19, "BS"}});
    check_fixed_layout_decode<itch::OrderExecuted>(input, {});
    check_fixed_layout_decode<itch::OrderExecutedWithPrice>(input, {});
    check_fixed_layout_decode<itch::OrderCancel>(input, {});
    check_fixed_layout_decode<itch::OrderDelete>(input, {});
    check_fixed_layout_decode<itch::OrderReplace>(input, {});
    check_fixed_layout_decode<itch::Trade>(input, {{19, "BS"}});
    check_fixed_layout_decode<itch::CrossTrade>(input, {});
    check_fixed_layout_decode<itch::NetOrderImbalance>(input, {});

    FuzzByteReader reader(data, size);
    switch (reader.take_index(6)) {
        case 0: check_wide_field_encode<itch::AddOrder, 48>(take_base_message<itch::AddOrder>(reader), reader); break;
        case 1: check_wide_field_encode<itch::OrderExecutedWithPrice, 48>(take_base_message<itch::OrderExecutedWithPrice>(reader), reader); break;
        case 2: check_wide_field_encode<itch::OrderReplace, 48>(take_base_message<itch::OrderReplace>(reader), reader); break;
        case 3: check_wide_field_encode<itch::Trade, 48>(take_base_message<itch::Trade>(reader), reader); break;
        case 4: check_wide_field_encode<itch::NetOrderImbalance, 48>(take_base_message<itch::NetOrderImbalance>(reader), reader); break;
        default: check_wide_field_encode<itch::OrderDelete, 48>(take_base_message<itch::OrderDelete>(reader), reader); break;
    }
    return 0;
}

}  // namespace exsim::fuzz

#endif
