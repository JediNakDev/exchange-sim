#ifndef EXSIM_TEST_SUPPORT_ITCH_DISPATCH_HPP
#define EXSIM_TEST_SUPPORT_ITCH_DISPATCH_HPP

// Typed ITCH 5.0 delivery: decodes one message by its type byte and calls
// the matching `handler.on_itch(sequence, const itch50::<Type>&)` overload.
// Overloads are optional; a handler without one for a type ignores it.
// Unknown type bytes go to the optional `handler.on_unknown_itch(sequence,
// message)`.
//
//   struct Printer {
//       void on_itch(std::uint64_t seq, const itch50::AddOrder& m);
//       void on_itch(std::uint64_t seq, const itch50::OrderDelete& m);
//   };
//   Printer printer;
//   TypedItchHandler<Printer> typed{printer};
//   MoldUdp64Receiver<TypedItchHandler<Printer>, Sink> receiver(config, typed, sink);
//
// No allocation; decoding uses exsim::protocol::itch50.

#include <protocol/itch50.hpp>

#include <cstdint>
#include <span>

namespace exsim::md {

namespace detail {
template <class Message, class Handler>
protocol::Status decode_and_deliver(std::uint64_t sequence, std::span<const char> bytes, Handler& handler) {
    Message message;
    const protocol::Status status = protocol::itch50::decode(bytes, message);
    if (status != protocol::Status::ok) return status;
    if constexpr (requires { handler.on_itch(sequence, message); }) {
        handler.on_itch(sequence, message);
    }
    return protocol::Status::ok;
}
}  // namespace detail

template <class Handler>
protocol::Status dispatch_itch_message(std::uint64_t sequence, std::span<const char> bytes, Handler& handler) {
    namespace itch = protocol::itch50;
    if (bytes.empty()) return protocol::Status::invalid_length;
    switch (bytes[0]) {
        case itch::SystemEvent::type: return detail::decode_and_deliver<itch::SystemEvent>(sequence, bytes, handler);
        case itch::StockDirectory::type:
            return detail::decode_and_deliver<itch::StockDirectory>(sequence, bytes, handler);
        case itch::StockTradingAction::type:
            return detail::decode_and_deliver<itch::StockTradingAction>(sequence, bytes, handler);
        case itch::AddOrder::type: return detail::decode_and_deliver<itch::AddOrder>(sequence, bytes, handler);
        case itch::AddOrderMPID::type: return detail::decode_and_deliver<itch::AddOrderMPID>(sequence, bytes, handler);
        case itch::OrderExecuted::type:
            return detail::decode_and_deliver<itch::OrderExecuted>(sequence, bytes, handler);
        case itch::OrderExecutedWithPrice::type:
            return detail::decode_and_deliver<itch::OrderExecutedWithPrice>(sequence, bytes, handler);
        case itch::OrderCancel::type: return detail::decode_and_deliver<itch::OrderCancel>(sequence, bytes, handler);
        case itch::OrderDelete::type: return detail::decode_and_deliver<itch::OrderDelete>(sequence, bytes, handler);
        case itch::OrderReplace::type: return detail::decode_and_deliver<itch::OrderReplace>(sequence, bytes, handler);
        case itch::Trade::type: return detail::decode_and_deliver<itch::Trade>(sequence, bytes, handler);
        case itch::CrossTrade::type: return detail::decode_and_deliver<itch::CrossTrade>(sequence, bytes, handler);
        case itch::NetOrderImbalance::type:
            return detail::decode_and_deliver<itch::NetOrderImbalance>(sequence, bytes, handler);
        default:
            if constexpr (requires { handler.on_unknown_itch(sequence, bytes); }) {
                handler.on_unknown_itch(sequence, bytes);
            }
            return protocol::Status::invalid_type;
    }
}

// Adapts a typed handler to MoldUdp64Receiver's on_message interface and
// forwards its optional end-of-session / lost-messages callbacks.
template <class TypedHandler>
struct TypedItchHandler {
    TypedHandler& typed;
    std::uint64_t decode_failures = 0;

    void on_message(std::uint64_t sequence, std::span<const char> bytes) {
        if (dispatch_itch_message(sequence, bytes, typed) != protocol::Status::ok) ++decode_failures;
    }
    void on_end_of_session(std::uint64_t next_sequence) {
        if constexpr (requires { typed.on_end_of_session(next_sequence); }) typed.on_end_of_session(next_sequence);
    }
    void on_messages_lost(std::uint64_t first_sequence, std::uint64_t count) {
        if constexpr (requires { typed.on_messages_lost(first_sequence, count); })
            typed.on_messages_lost(first_sequence, count);
    }
};

}  // namespace exsim::md

#endif
