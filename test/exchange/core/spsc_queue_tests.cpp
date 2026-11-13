// exsim::util::SPSCQueue: capacity, wraparound and cross-thread FIFO order.

#include "test/framework/test.hpp"
#include <util/spsc_queue.hpp>

#include <cstdint>
#include <thread>

using exsim::util::SPSCQueue;

TEST_CASE(spsc_queue, holds_capacity_minus_one_and_keeps_fifo_across_wraparound) {
    SPSCQueue<std::uint64_t, 4> queue;
    std::uint64_t next_to_push = 0;
    std::uint64_t next_expected = 0;
    bool fifo_ok = true;
    bool capacity_ok = true;
    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 3; ++i) {
            capacity_ok = capacity_ok && queue.try_push(next_to_push++);
        }
        capacity_ok = capacity_ok && !queue.try_push(12345);  // one slot always stays empty
        std::uint64_t value = 0;
        const int pops = 1 + round % 3;
        for (int i = 0; i < pops; ++i) {
            fifo_ok = fifo_ok && queue.try_pop(value) && value == next_expected++;
        }
        while (queue.try_pop(value)) {
            fifo_ok = fifo_ok && value == next_expected++;
        }
        capacity_ok = capacity_ok && queue.empty();
    }
    CHECK(fifo_ok);
    CHECK(capacity_ok);
    CHECK_EQ(next_expected, next_to_push);
}

TEST_CASE(spsc_queue, two_threads_transfer_every_value_in_order) {
    SPSCQueue<std::uint64_t, 64> queue;  // small ring: producer and consumer both hit full/empty
    constexpr std::uint64_t kValues = 2'000'000;
    std::thread producer([&queue] {
        for (std::uint64_t value = 1; value <= kValues; ++value) {
            while (!queue.try_push(value)) {
                std::this_thread::yield();
            }
        }
    });
    std::uint64_t expected = 1;
    bool in_order = true;
    while (expected <= kValues) {
        std::uint64_t value = 0;
        if (!queue.try_pop(value)) {
            std::this_thread::yield();
            continue;
        }
        in_order = in_order && value == expected;
        ++expected;
    }
    producer.join();
    CHECK(in_order);
    CHECK(queue.empty());
}
