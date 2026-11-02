#ifndef LOB_PROTOCOL_ITCH_HPP
#define LOB_PROTOCOL_ITCH_HPP

#include "../compiler.hpp"
#include "../types.hpp"
#include <cstdint>
#include <cstring>

namespace lob::itch {

// ITCH 5.0 message types (subset relevant to order book management)
enum class MessageType : char {
    AddOrder        = 'A',  // Add Order (no MPID)
    AddOrderMPID    = 'F',  // Add Order with MPID attribution
    OrderExecuted   = 'E',  // Order Executed
    OrderCancel     = 'X',  // Order Cancel
    OrderDelete     = 'D',  // Order Delete
    OrderReplace    = 'U',  // Order Replace
};

// Parsed message bodies. Zero-copy: fields are extracted directly from the
// wire buffer.
struct AddOrderBody {
    uint64_t order_ref;
    uint64_t timestamp_ns;
    Side     side;
    uint32_t shares;
    int64_t  price;     // price in fixed-point (price * 10000)
};

struct OrderExecutedBody {
    uint64_t order_ref;
    uint64_t timestamp_ns;
    uint32_t executed_shares;
    uint64_t match_number;
};

struct OrderCancelBody {
    uint64_t order_ref;
    uint64_t timestamp_ns;
    uint32_t cancelled_shares;
};

struct OrderDeleteBody {
    uint64_t order_ref;
    uint64_t timestamp_ns;
};

struct OrderReplaceBody {
    uint64_t original_order_ref;
    uint64_t new_order_ref;
    uint64_t timestamp_ns;
    uint32_t shares;
    int64_t  price;
};

// Parsed message — tagged union over the bodies above.
struct Message {
    MessageType type;

    union {
        AddOrderBody      add_order;
        OrderExecutedBody order_executed;
        OrderCancelBody   order_cancel;
        OrderDeleteBody   order_delete;
        OrderReplaceBody  order_replace;
    };
};

// Network byte-order helpers (ITCH uses big-endian)
namespace detail {

inline uint16_t read_be16(const char* p) noexcept {
    const auto* u = reinterpret_cast<const uint8_t*>(p);
    return static_cast<uint16_t>((u[0] << 8) | u[1]);
}

inline uint32_t read_be32(const char* p) noexcept {
    const auto* u = reinterpret_cast<const uint8_t*>(p);
    return (static_cast<uint32_t>(u[0]) << 24) |
           (static_cast<uint32_t>(u[1]) << 16) |
           (static_cast<uint32_t>(u[2]) << 8)  |
           static_cast<uint32_t>(u[3]);
}

inline uint64_t read_be48(const char* p) noexcept {
    const auto* u = reinterpret_cast<const uint8_t*>(p);
    return (static_cast<uint64_t>(u[0]) << 40) |
           (static_cast<uint64_t>(u[1]) << 32) |
           (static_cast<uint64_t>(u[2]) << 24) |
           (static_cast<uint64_t>(u[3]) << 16) |
           (static_cast<uint64_t>(u[4]) << 8)  |
           static_cast<uint64_t>(u[5]);
}

inline uint64_t read_be64(const char* p) noexcept {
    const auto* u = reinterpret_cast<const uint8_t*>(p);
    return (static_cast<uint64_t>(u[0]) << 56) |
           (static_cast<uint64_t>(u[1]) << 48) |
           (static_cast<uint64_t>(u[2]) << 40) |
           (static_cast<uint64_t>(u[3]) << 32) |
           (static_cast<uint64_t>(u[4]) << 24) |
           (static_cast<uint64_t>(u[5]) << 16) |
           (static_cast<uint64_t>(u[6]) << 8)  |
           static_cast<uint64_t>(u[7]);
}

} // namespace detail

// ITCH 5.0 message sizes (bytes)
// Add Order (A):        36 bytes
// Add Order MPID (F):   40 bytes
// Order Executed (E):   31 bytes
// Order Cancel (X):     23 bytes
// Order Delete (D):     19 bytes
// Order Replace (U):    35 bytes

// Returns the expected message size for a given type, or 0 if unknown.
inline int message_size(char type) noexcept {
    switch (type) {
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        default:  return 0;
    }
}

// Parse a single ITCH 5.0 message from a buffer.
// Returns true on success, false if the message type is unsupported.
// Caller must ensure `buf` has at least `message_size(buf[0])` bytes.
//
// ITCH 5.0 Add Order (type 'A') layout:
//   Offset  Size  Field
//   0       1     Message Type ('A')
//   1       2     Stock Locate
//   3       2     Tracking Number
//   5       6     Timestamp (nanoseconds since midnight)
//   11      8     Order Reference Number
//   19      1     Buy/Sell Indicator ('B' or 'S')
//   20      4     Shares
//   24      8     Stock (ASCII, space-padded)
//   32      4     Price (fixed-point, 4 decimal places)
inline bool parse(const char* buf, Message& msg) noexcept {
    msg.type = static_cast<MessageType>(buf[0]);

    switch (buf[0]) {
    case 'A':
    case 'F': {
        msg.add_order.timestamp_ns = detail::read_be48(buf + 5);
        msg.add_order.order_ref    = detail::read_be64(buf + 11);
        msg.add_order.side         = (buf[19] == 'B') ? Side::BUY : Side::SELL;
        msg.add_order.shares       = detail::read_be32(buf + 20);
        msg.add_order.price        = static_cast<int64_t>(detail::read_be32(buf + 32));
        return true;
    }
    case 'E': {
        msg.order_executed.timestamp_ns    = detail::read_be48(buf + 5);
        msg.order_executed.order_ref       = detail::read_be64(buf + 11);
        msg.order_executed.executed_shares = detail::read_be32(buf + 19);
        msg.order_executed.match_number    = detail::read_be64(buf + 23);
        return true;
    }
    case 'X': {
        msg.order_cancel.timestamp_ns     = detail::read_be48(buf + 5);
        msg.order_cancel.order_ref        = detail::read_be64(buf + 11);
        msg.order_cancel.cancelled_shares = detail::read_be32(buf + 19);
        return true;
    }
    case 'D': {
        msg.order_delete.timestamp_ns = detail::read_be48(buf + 5);
        msg.order_delete.order_ref    = detail::read_be64(buf + 11);
        return true;
    }
    case 'U': {
        msg.order_replace.timestamp_ns       = detail::read_be48(buf + 5);
        msg.order_replace.original_order_ref = detail::read_be64(buf + 11);
        msg.order_replace.new_order_ref      = detail::read_be64(buf + 19);
        msg.order_replace.shares             = detail::read_be32(buf + 27);
        msg.order_replace.price              = static_cast<int64_t>(detail::read_be32(buf + 31));
        return true;
    }
    default:
        return false;
    }
}

} // namespace lob::itch

#endif
