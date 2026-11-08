#ifndef EXSIM_RESEARCH_TIMED_EVENT_QUEUE_HPP
#define EXSIM_RESEARCH_TIMED_EVENT_QUEUE_HPP

// Fixed-capacity priority queue of timed payloads for the discrete-event runner.
//
// Ordering contract (normative, tested by research_scheduler):
//   1. ascending simulated time in nanoseconds;
//   2. for equal times, ascending enqueue sequence, i.e. first scheduled is
//      first processed (FIFO tie-break).
// The enqueue sequence is a per-queue counter that starts at 0 and grows by
// one on every successful push, so the order is total and deterministic.
//
// Storage: a binary heap of 24-byte keys {time, sequence, slot} over a slab of
// payload slots with a free-slot stack. Everything is allocated in the
// constructor; push/pop never allocate. A full queue rejects the push and the
// caller must fail the run explicitly (never drop an event silently).

#include "exsim/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace exsim::research {

template <typename Payload>
class TimedEventQueue {
    static_assert(std::is_trivially_copyable_v<Payload>, "payloads are copied in and out of slots");

public:
    struct Entry {
        Timestamp time = 0;
        std::uint64_t sequence = 0;
        Payload payload{};
    };

    explicit TimedEventQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0 || capacity > (std::size_t{1} << 31)) throw std::invalid_argument("timed event queue capacity");
        heap_ = std::make_unique<Key[]>(capacity);
        payloads_ = std::make_unique<Payload[]>(capacity);
        free_slots_ = std::make_unique<std::uint32_t[]>(capacity);
        for (std::size_t i = 0; i < capacity; ++i) free_slots_[i] = static_cast<std::uint32_t>(capacity - 1 - i);
        free_count_ = capacity;
    }

    TimedEventQueue(const TimedEventQueue&) = delete;
    TimedEventQueue& operator=(const TimedEventQueue&) = delete;

    [[nodiscard]] bool push(Timestamp time, const Payload& payload) noexcept {
        if (size_ == capacity_) return false;
        const std::uint32_t slot = free_slots_[--free_count_];
        payloads_[slot] = payload;
        std::size_t index = size_++;
        if (size_ > high_water_mark_) high_water_mark_ = size_;
        const Key inserted{time, next_sequence_++, slot};
        while (index > 0) {
            const std::size_t parent = (index - 1) / 2;
            if (!earlier(inserted, heap_[parent])) break;
            heap_[index] = heap_[parent];
            index = parent;
        }
        heap_[index] = inserted;
        return true;
    }

    // Preconditions for top_time/top_sequence/pop: !empty().
    [[nodiscard]] Timestamp top_time() const noexcept { return heap_[0].time; }
    [[nodiscard]] std::uint64_t top_sequence() const noexcept { return heap_[0].sequence; }
    [[nodiscard]] const Payload& top_payload() const noexcept { return payloads_[heap_[0].slot]; }

    Entry pop() noexcept {
        const Key head = heap_[0];
        Entry result{head.time, head.sequence, payloads_[head.slot]};
        free_slots_[free_count_++] = head.slot;
        const Key last = heap_[--size_];
        std::size_t index = 0;
        while (true) {
            const std::size_t left = 2 * index + 1;
            if (left >= size_) break;
            const std::size_t right = left + 1;
            const std::size_t child = right < size_ && earlier(heap_[right], heap_[left]) ? right : left;
            if (!earlier(heap_[child], last)) break;
            heap_[index] = heap_[child];
            index = child;
        }
        if (size_ > 0) heap_[index] = last;
        return result;
    }

    // Visits every queued entry in unspecified (heap) order. Used at run end,
    // e.g. to censor observations scheduled past the horizon.
    template <typename Visitor>
    void for_each(Visitor&& visitor) const {
        for (std::size_t i = 0; i < size_; ++i) {
            const Key& key = heap_[i];
            visitor(key.time, key.sequence, payloads_[key.slot]);
        }
    }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t high_water_mark() const noexcept { return high_water_mark_; }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }

private:
    struct Key {
        Timestamp time = 0;
        std::uint64_t sequence = 0;
        std::uint32_t slot = 0;
    };

    [[nodiscard]] static bool earlier(const Key& a, const Key& b) noexcept {
        return a.time != b.time ? a.time < b.time : a.sequence < b.sequence;
    }

    std::size_t capacity_;
    std::unique_ptr<Key[]> heap_;
    std::unique_ptr<Payload[]> payloads_;
    std::unique_ptr<std::uint32_t[]> free_slots_;
    std::size_t free_count_ = 0;
    std::size_t size_ = 0;
    std::size_t high_water_mark_ = 0;
    std::uint64_t next_sequence_ = 0;
};

}  // namespace exsim::research

#endif
