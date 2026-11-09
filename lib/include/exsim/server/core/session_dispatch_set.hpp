#ifndef EXSIM_SERVER_CORE_SESSION_DISPATCH_SET_HPP
#define EXSIM_SERVER_CORE_SESSION_DISPATCH_SET_HPP

// Distinct sessions of one event batch in first-appearance order, with
// fixed storage (a 65536-bit seen set plus an order list sized for every
// possible non-zero SessionId), so building the dispatch list never
// allocates and never overflows.

#include "exsim/event.hpp"
#include "exsim/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace exsim::server {

// Calls visit(SessionId) for each session an event concerns, in field
// order: one for order events, buy leg then sell leg for a Fill, none for
// PhaseChanged / AuctionSummary. Session 0 (SetPhase rejections) is passed
// through; callers skip it.
template <typename Visitor>
constexpr void visit_event_sessions(const Event& event, Visitor&& visit) noexcept {
    switch (event.type) {
        case EventType::Accepted: visit(event.accepted.session); return;
        case EventType::Rejected: visit(event.rejected.session); return;
        case EventType::Rested: visit(event.rested.session); return;
        case EventType::Fill:
            visit(event.fill.buy.session);
            visit(event.fill.sell.session);
            return;
        case EventType::Canceled: visit(event.canceled.session); return;
        case EventType::Replaced: visit(event.replaced.session); return;
        case EventType::MassCancelDone: visit(event.mass_cancel_done.session); return;
        case EventType::PhaseChanged:
        case EventType::AuctionSummary: return;
    }
}

class SessionDispatchSet {
public:
    static constexpr std::size_t kMaxSessions = std::numeric_limits<SessionId>::max();  // ids 1..65535

    SessionDispatchSet() : order_(std::make_unique<SessionId[]>(kMaxSessions)) {}

    // Appends `session` unless it is 0 or already listed.
    void add(SessionId session) noexcept {
        if (session == 0) {
            return;
        }
        std::uint64_t& word = seen_[session >> 6];
        const std::uint64_t bit = std::uint64_t{1} << (session & 63);
        if ((word & bit) != 0) {
            return;
        }
        word |= bit;
        order_[size_++] = session;
    }

    [[nodiscard]] std::span<const SessionId> sessions() const noexcept { return {order_.get(), size_}; }

    // Forgets the listed sessions only: cost proportional to the batch.
    void clear() noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            seen_[order_[i] >> 6] &= ~(std::uint64_t{1} << (order_[i] & 63));
        }
        size_ = 0;
    }

private:
    std::array<std::uint64_t, (kMaxSessions + 1) / 64> seen_{};
    std::unique_ptr<SessionId[]> order_;
    std::size_t size_ = 0;
};

}  // namespace exsim::server

#endif
