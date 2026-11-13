#ifndef EXSIM_SERVER_CORE_SESSION_DISPATCH_SET_HPP
#define EXSIM_SERVER_CORE_SESSION_DISPATCH_SET_HPP

// Groups the events of one batch by session, in O(events + sessions) with
// fixed storage, so the core hands each session only its own events and
// never allocates or overflows while doing so.

#include "event.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
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

// The events of one session in one batch, in batch order: the batch read
// through the positions of the events that concern the session. A Fill
// whose two legs both belong to the session appears once.
class SessionEvents {
public:
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Event;
        using difference_type = std::ptrdiff_t;
        using pointer = const Event*;
        using reference = const Event&;

        Iterator() = default;
        Iterator(const Event* batch, const std::uint32_t* position) noexcept : batch_(batch), position_(position) {}

        reference operator*() const noexcept { return batch_[*position_]; }
        pointer operator->() const noexcept { return &batch_[*position_]; }
        Iterator& operator++() noexcept {
            ++position_;
            return *this;
        }
        Iterator operator++(int) noexcept {
            Iterator before = *this;
            ++position_;
            return before;
        }
        bool operator==(const Iterator& other) const noexcept { return position_ == other.position_; }

    private:
        const Event* batch_ = nullptr;
        const std::uint32_t* position_ = nullptr;
    };

    SessionEvents(std::span<const Event> batch, std::span<const std::uint32_t> positions) noexcept
        : batch_(batch.data()), positions_(positions) {}

    [[nodiscard]] Iterator begin() const noexcept { return {batch_, positions_.data()}; }
    [[nodiscard]] Iterator end() const noexcept { return {batch_, positions_.data() + positions_.size()}; }
    [[nodiscard]] std::size_t size() const noexcept { return positions_.size(); }
    [[nodiscard]] bool empty() const noexcept { return positions_.empty(); }
    [[nodiscard]] const Event& operator[](std::size_t index) const noexcept { return batch_[positions_[index]]; }

private:
    const Event* batch_;
    std::span<const std::uint32_t> positions_;
};

// Counting sort of a batch's events by session. Per batch:
//   count_sessions(event) for every event, in batch order (lists sessions
//                         in first-appearance order and counts their events)
//   place_events(batch)   one pass that writes every event's position into
//                         its sessions' ranges
//   session(i), events_of(i, batch) for i < session_count()
//   clear()               cost proportional to the batch's sessions
class SessionDispatchSet {
public:
    static constexpr std::size_t kMaxSessions = std::numeric_limits<SessionId>::max();  // ids 1..65535

    // `max_batch_events`: the largest batch the core can produce. Each
    // event concerns at most two sessions.
    explicit SessionDispatchSet(std::size_t max_batch_events)
        : order_by_session_(std::make_unique<std::uint16_t[]>(kMaxSessions + 1)),
          sessions_(std::make_unique_for_overwrite<SessionId[]>(kMaxSessions)),
          range_ends_(std::make_unique_for_overwrite<std::uint32_t[]>(kMaxSessions)),
          positions_(std::make_unique_for_overwrite<std::uint32_t[]>(2 * max_batch_events)) {}

    void count_sessions(const Event& event) noexcept {
        SessionId previous = 0;
        visit_event_sessions(event, [&](SessionId session) {
            if (session == 0 || session == previous) return;
            previous = session;
            ++range_ends_[order_of(session)];
        });
    }

    void place_events(std::span<const Event> batch) noexcept {
        std::uint32_t range_begin = 0;
        for (std::size_t order = 0; order < session_count_; ++order) {
            const std::uint32_t count = range_ends_[order];
            range_ends_[order] = range_begin;  // next free slot of the range until placing ends
            range_begin += count;
        }
        for (std::size_t position = 0; position < batch.size(); ++position) {
            SessionId previous = 0;
            visit_event_sessions(batch[position], [&](SessionId session) {
                if (session == 0 || session == previous) return;
                previous = session;
                positions_[range_ends_[order_by_session_[session] - 1u]++] = static_cast<std::uint32_t>(position);
            });
        }
    }

    [[nodiscard]] std::size_t session_count() const noexcept { return session_count_; }
    [[nodiscard]] SessionId session(std::size_t order) const noexcept { return sessions_[order]; }
    [[nodiscard]] SessionEvents events_of(std::size_t order, std::span<const Event> batch) const noexcept {
        const std::uint32_t range_begin = order == 0 ? 0 : range_ends_[order - 1];
        return {batch, {positions_.get() + range_begin, range_ends_[order] - range_begin}};
    }

    void clear() noexcept {
        for (std::size_t order = 0; order < session_count_; ++order) order_by_session_[sessions_[order]] = 0;
        session_count_ = 0;
    }

private:
    std::size_t order_of(SessionId session) noexcept {
        std::uint16_t& order_plus_one = order_by_session_[session];
        if (order_plus_one == 0) {
            sessions_[session_count_] = session;
            range_ends_[session_count_] = 0;
            order_plus_one = static_cast<std::uint16_t>(++session_count_);
        }
        return order_plus_one - 1u;
    }

    std::unique_ptr<std::uint16_t[]> order_by_session_;  // first-appearance order + 1, 0 = not in the batch
    std::unique_ptr<SessionId[]> sessions_;              // by first-appearance order
    std::unique_ptr<std::uint32_t[]> range_ends_;        // by order: end of the session's range in positions_
    std::unique_ptr<std::uint32_t[]> positions_;         // batch positions grouped by session
    std::size_t session_count_ = 0;
};

}  // namespace exsim::server

#endif
