#ifndef EXSIM_COMMAND_HPP
#define EXSIM_COMMAND_HPP

// Inbound commands: the protocol-neutral representation every order-entry
// gateway (OUCH, FIX, ...) decodes into. All structs are trivially copyable
// so they can be passed through SPSC queues and written verbatim to the
// journal.

#include "types.hpp"
#include <type_traits>

namespace exsim {

struct NewOrder {
    Timestamp ts = 0;
    InstrumentId instrument = 0;
    SessionId session = 0;
    OwnerId owner = 0;
    ClOrdId cl_ord_id = 0;
    Side side = Side::BUY;
    OrdType type = OrdType::Limit;
    TimeInForce tif = TimeInForce::Day;
    StpMode stp = StpMode::None;
    bool post_only = false;
    Price price = 0;  // ignored for market orders
    Quantity qty = 0;
};

struct CancelOrder {
    Timestamp ts = 0;
    SessionId session = 0;
    ClOrdId orig_cl_ord_id = 0;  // token of the live order to cancel
    ClOrdId cl_ord_id = 0;       // token of this request (echoed back; may be 0)
};

struct ReplaceOrder {
    Timestamp ts = 0;
    SessionId session = 0;
    ClOrdId orig_cl_ord_id = 0;  // token of the live order to replace
    ClOrdId cl_ord_id = 0;       // new token for the order; 0 = keep orig
    Price price = 0;             // new limit price
    Quantity qty = 0;            // new TOTAL quantity (incl. already filled)
};

struct MassCancel {
    Timestamp ts = 0;
    SessionId session = 0;
    InstrumentId instrument = 0;  // 0 = all instruments
    ClOrdId cl_ord_id = 0;        // token of this request (echoed back)
    bool cancel_on_disconnect = false;  // selects CancelReason
};

struct SetPhase {
    Timestamp ts = 0;
    InstrumentId instrument = 0;  // 0 = all instruments
    Phase phase = Phase::Continuous;
};

struct Command {
    CommandType type = CommandType::NewOrder;
    union {
        NewOrder new_order;
        CancelOrder cancel;
        ReplaceOrder replace;
        MassCancel mass_cancel;
        SetPhase set_phase;
    };

    Command() noexcept : new_order() {}
    Command(const NewOrder& c) noexcept : type(CommandType::NewOrder), new_order(c) {}
    Command(const CancelOrder& c) noexcept : type(CommandType::Cancel), cancel(c) {}
    Command(const ReplaceOrder& c) noexcept : type(CommandType::Replace), replace(c) {}
    Command(const MassCancel& c) noexcept : type(CommandType::MassCancel), mass_cancel(c) {}
    Command(const SetPhase& c) noexcept : type(CommandType::SetPhase), set_phase(c) {}

    [[nodiscard]] Timestamp ts() const noexcept {
        switch (type) {
            case CommandType::NewOrder: return new_order.ts;
            case CommandType::Cancel: return cancel.ts;
            case CommandType::Replace: return replace.ts;
            case CommandType::MassCancel: return mass_cancel.ts;
            case CommandType::SetPhase: return set_phase.ts;
        }
        return 0;
    }
};

static_assert(std::is_trivially_copyable_v<Command>);

}  // namespace exsim

#endif
