// Test the real benchmark recorder without starting sockets or measuring speed.
#define main exsim_w2w_bench_main_for_schedule_test
#include "test/tools/exsim_w2w_bench/main.cpp"
#undef main
#include "test/framework/test.hpp"

TEST_CASE(server_w2w_schedule, intended_deadline_delay_is_retained_for_both_responses) {
    LatencyRecorder recorder(1, 0);
    const auto now = exsim::net::monotonic_ns();
    recorder.intended_time_ns[0] = now - 110'000'000;
    recorder.send_time_ns[0] = now - 10'000'000;
    recorder.accepted(0);
    const std::array<char, 1> add{'A'};
    recorder.on_message(1, add);
    REQUIRE_EQ(recorder.scheduled_accept_latency_ns.size(), 1u);
    REQUIRE_EQ(recorder.scheduled_itch_add_latency_ns.size(), 1u);
    CHECK_EQ(recorder.scheduled_accept_latency_ns[0] - recorder.accept_latency_ns[0], 100'000'000u);
    CHECK_EQ(recorder.scheduled_itch_add_latency_ns[0] - recorder.itch_add_latency_ns[0], 100'000'000u);
}

TEST_CASE(server_w2w_schedule, warmup_excludes_both_latency_distributions) {
    LatencyRecorder recorder(1, 1);
    recorder.send_time_ns[0] = exsim::net::monotonic_ns();
    recorder.intended_time_ns[0] = recorder.send_time_ns[0];
    recorder.accepted(0);
    const std::array<char, 1> add{'A'};
    recorder.on_message(1, add);
    CHECK(recorder.accept_latency_ns.empty());
    CHECK(recorder.scheduled_accept_latency_ns.empty());
    CHECK(recorder.itch_add_latency_ns.empty());
    CHECK(recorder.scheduled_itch_add_latency_ns.empty());
}
