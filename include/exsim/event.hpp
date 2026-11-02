#ifndef EXSIM_EVENT_HPP
#define EXSIM_EVENT_HPP

// Outbound events emitted by the matching engine. Gateways turn them into
// execution reports for the owning sessions; the market-data publisher turns
// them into ITCH. Event order within one command is normative: see
// docs/SPEC.md section "Event ordering".

#include "types.hpp"
#include <cstddef>
#include <type_traits>
#include <vector>

namespace exsim {

enum class EventType : std::uint8_t {
    Accepted = 0,
    Rejected = 1,
    Rested = 2,
    Fill = 3,
    Canceled = 4,
    Replaced = 5,
    MassCancelDone = 6,
    PhaseChanged = 7,
    AuctionSummary = 8,
};

struct AcceptedEvent {
    OrderId order_id;
    ClOrdId cl_ord_id;
    SessionId session;
    OwnerId owner;
    Side side;
    OrdType ord_type;
    TimeInForce tif;
    StpMode stp;
    bool post_only;
    Price price;
    Quantity qty;
};

struct RejectedEvent {
    CommandType cmd;
    RejectReason reason;
    SessionId session;
    ClOrdId cl_ord_id;       // token of the rejected request
    ClOrdId orig_cl_ord_id;  // cancel/replace: target token; else 0
    OrderId order_id;        // target order if it exists; else 0
};

// The (remainder of an) order was placed on the book.
struct RestedEvent {
    OrderId order_id;
    ClOrdId cl_ord_id;
    SessionId session;
    Side side;
    Price price;
    Quantity leaves_qty;
};

struct FillLeg {
    OrderId order_id;
    ClOrdId cl_ord_id;
    SessionId session;
    OwnerId owner;
    Quantity leaves_qty;  // after this fill; 0 = order complete
    Quantity cum_qty;     // after this fill
};

struct FillEvent {
    MatchId match_id;
    Price price;
    Quantity qty;
    Aggressor aggressor;  // None for auction executions
    FillLeg buy;
    FillLeg sell;
};

struct CanceledEvent {
    OrderId order_id;
    ClOrdId cl_ord_id;          // the order's token
    ClOrdId request_cl_ord_id;  // token of the cancel/mass-cancel request; else 0
    SessionId session;
    Side side;
    Price price;
    Quantity canceled_qty;      // leaves removed by this cancel
    Quantity cum_qty;
    CancelReason reason;
    bool was_resting;           // true if the order was on the book
};

struct ReplacedEvent {
    OrderId old_order_id;
    OrderId new_order_id;       // == old_order_id iff priority_kept
    ClOrdId orig_cl_ord_id;
    ClOrdId cl_ord_id;          // token after the replace
    SessionId session;
    Side side;
    Price old_price;
    Price price;
    Quantity old_leaves_qty;
    Quantity qty;               // new total quantity
    Quantity leaves_qty;        // qty - cum_qty
    Quantity cum_qty;
    bool priority_kept;
};

struct MassCancelDoneEvent {
    SessionId session;
    ClOrdId request_cl_ord_id;
    std::uint32_t canceled_count;
    CancelReason reason;
};

struct PhaseChangedEvent {
    Phase old_phase;
    Phase new_phase;
};

struct AuctionSummaryEvent {
    Price price;
    std::uint64_t matched_qty;
};

struct Event {
    EventType type;
    InstrumentId instrument;  // 0 if not applicable / unknown
    Timestamp ts;             // copied from the command
    union {
        AcceptedEvent accepted;
        RejectedEvent rejected;
        RestedEvent rested;
        FillEvent fill;
        CanceledEvent canceled;
        ReplacedEvent replaced;
        MassCancelDoneEvent mass_cancel_done;
        PhaseChangedEvent phase_changed;
        AuctionSummaryEvent auction_summary;
    };
};

static_assert(std::is_trivially_copyable_v<Event>);

// Reusable output buffer. The engine appends; the caller drains and clears.
// Capacity is retained across clear() so steady-state use never allocates.
class EventBuffer {
public:
    explicit EventBuffer(std::size_t initial_capacity = 4096) { events_.reserve(initial_capacity); }

    void clear() noexcept { events_.clear(); }
    void push(const Event& e) { events_.push_back(e); }

    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
    [[nodiscard]] bool empty() const noexcept { return events_.empty(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return events_.capacity(); }
    [[nodiscard]] const Event& operator[](std::size_t i) const noexcept { return events_[i]; }
    [[nodiscard]] const Event* begin() const noexcept { return events_.data(); }
    [[nodiscard]] const Event* end() const noexcept { return events_.data() + events_.size(); }

private:
    std::vector<Event> events_;
};

}  // namespace exsim

#endif
