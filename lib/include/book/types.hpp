#ifndef EXSIM_BOOK_TYPES_HPP
#define EXSIM_BOOK_TYPES_HPP

#include <cstdint>

namespace exsim::book {

enum class Side : uint8_t { BUY, SELL };

using OrderId = uint64_t;
using Quantity = uint64_t;
using Timestamp = uint64_t;
using Price = int64_t;

struct Fill {
    OrderId buy_order_id;
    OrderId sell_order_id;
    Price price;
    Quantity quantity;
};

}

#endif
