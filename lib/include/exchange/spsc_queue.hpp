#ifndef EXSIM_EXCHANGE_ENGINE_SPSC_QUEUE_HPP
#define EXSIM_EXCHANGE_ENGINE_SPSC_QUEUE_HPP

#include <array>
#include <atomic>
#include <cstddef>

namespace exsim::exchange::engine {

// Bounded single-producer single-consumer ring. Each side keeps a private
// copy of the other side's index and reloads the shared one only when the
// ring looks full (producer) or empty (consumer), so the steady state does
// not pull the other thread's cache line on every operation.
template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert(Capacity > 1, "Capacity must be > 1");

public:
    bool try_push(const T& value) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = increment(tail);
        if (next == producer_cached_head_) {
            producer_cached_head_ = head_.load(std::memory_order_acquire);
            if (next == producer_cached_head_) {
                return false;
            }
        }

        buffer_[tail] = value;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == consumer_cached_tail_) {
            consumer_cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == consumer_cached_tail_) {
                return false;
            }
        }

        out = buffer_[head];
        head_.store(increment(head), std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t increment(std::size_t idx) noexcept {
        return (idx + 1) % Capacity;
    }

    // Consumer-owned line.
    alignas(128) std::atomic<std::size_t> head_{0};
    std::size_t consumer_cached_tail_ = 0;
    // Producer-owned line.
    alignas(128) std::atomic<std::size_t> tail_{0};
    std::size_t producer_cached_head_ = 0;
    alignas(128) std::array<T, Capacity> buffer_{};
};

}  // namespace exsim::exchange::engine

#endif
