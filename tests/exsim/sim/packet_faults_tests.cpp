#include "exsim/sim/packet_faults.hpp"
#include "exsim/sim/random.hpp"
#include "../../framework/test.hpp"

#include <cstring>
#include <string>
#include <vector>

using exsim::sim::PacketFaultConfig;
using exsim::sim::PacketFaultInjector;
using exsim::sim::Rng;

namespace {

struct Recorder {
    std::vector<int> seen;
    void operator()(std::span<const char> p) {
        int v = 0;
        std::memcpy(&v, p.data(), sizeof v);
        seen.push_back(v);
    }
};

void send(PacketFaultInjector& inj, Recorder& rec, int v) {
    char buf[sizeof(int)];
    std::memcpy(buf, &v, sizeof v);
    inj.submit(std::span<const char>(buf, sizeof buf), rec);
}

}  // namespace

TEST_CASE(sim_rng, deterministic_and_bounded) {
    Rng a(123), b(123), c(124);
    bool differs = false;
    for (int i = 0; i < 1000; ++i) {
        const auto x = a.next();
        CHECK_EQ(x, b.next());
        differs |= x != c.next();
    }
    CHECK(differs);
    for (int i = 0; i < 10'000; ++i) {
        CHECK_LT(a.below(7), 7u);
        const auto v = a.between(-3, 3);
        CHECK_GE(v, -3);
        CHECK_LE(v, 3);
    }
    CHECK(!a.chance_ppm(0));
    CHECK(a.chance_ppm(1'000'000));
}

TEST_CASE(sim_packet_faults, passthrough_when_idle) {
    PacketFaultInjector inj(64, 4);
    Recorder rec;
    CHECK(inj.idle());
    for (int i = 0; i < 10; ++i) send(inj, rec, i);
    CHECK_EQ(rec.seen.size(), 10u);
    for (int i = 0; i < 10; ++i) CHECK_EQ(rec.seen[static_cast<std::size_t>(i)], i);
    CHECK_EQ(inj.stats().delivered, 10u);
}

TEST_CASE(sim_packet_faults, one_shot_drop_and_duplicate) {
    PacketFaultInjector inj(64, 4);
    Recorder rec;
    send(inj, rec, 1);
    inj.drop_next(2);
    send(inj, rec, 2);
    send(inj, rec, 3);
    inj.duplicate_next(1);
    send(inj, rec, 4);
    send(inj, rec, 5);
    CHECK(rec.seen == (std::vector<int>{1, 4, 4, 5}));
    CHECK_EQ(inj.stats().dropped, 2u);
    CHECK_EQ(inj.stats().duplicated, 1u);
    CHECK(inj.idle());
}

TEST_CASE(sim_packet_faults, reorder_releases_after_delay) {
    PacketFaultInjector inj(64, 4);
    PacketFaultConfig cfg;
    cfg.reorder_delay = 2;
    inj.configure(cfg);
    Recorder rec;
    inj.reorder_next(1);
    send(inj, rec, 1);  // held
    CHECK_EQ(inj.held(), 1u);
    send(inj, rec, 2);
    send(inj, rec, 3);  // 1 released after 3
    send(inj, rec, 4);
    CHECK(rec.seen == (std::vector<int>{2, 3, 1, 4}));
    CHECK_EQ(inj.held(), 0u);
}

TEST_CASE(sim_packet_faults, flush_releases_in_arrival_order) {
    PacketFaultInjector inj(64, 4);
    PacketFaultConfig cfg;
    cfg.reorder_delay = 100;
    inj.configure(cfg);
    Recorder rec;
    inj.reorder_next(3);
    send(inj, rec, 1);
    send(inj, rec, 2);
    send(inj, rec, 3);
    send(inj, rec, 4);
    CHECK(rec.seen == (std::vector<int>{4}));
    inj.flush(rec);
    CHECK(rec.seen == (std::vector<int>{4, 1, 2, 3}));
}

TEST_CASE(sim_packet_faults, clear_faults_stops_new_faults_and_flush_releases_held) {
    PacketFaultInjector inj(64, 4);
    PacketFaultConfig cfg;
    cfg.drop_ppm = 1'000'000;
    cfg.reorder_delay = 100;
    cfg.seed = 9;
    inj.configure(cfg);
    Recorder rec;
    inj.reorder_next(1);
    inj.duplicate_next(5);
    send(inj, rec, 1);  // dropped by the rate
    inj.drop_next(0);
    inj.configure([] {
        PacketFaultConfig held_cfg;
        held_cfg.reorder_delay = 100;
        return held_cfg;
    }());
    send(inj, rec, 2);  // held by the pending one-shot reorder
    inj.drop_next(3);
    CHECK(!inj.idle());
    inj.clear_faults();
    CHECK_EQ(inj.config().reorder_delay, 100u);
    CHECK_EQ(inj.held(), 1u);
    CHECK(!inj.idle());  // something is still held
    send(inj, rec, 3);   // no drop, no duplicate
    inj.flush(rec);
    CHECK(inj.idle());
    CHECK(rec.seen == (std::vector<int>{3, 2}));
}

TEST_CASE(sim_packet_faults, cannot_hold_falls_back_to_delivery) {
    PacketFaultInjector inj(2, 1);  // slot too small for an int, one slot
    Recorder rec;
    inj.reorder_next(1);
    send(inj, rec, 7);
    CHECK(rec.seen == (std::vector<int>{7}));
    CHECK_EQ(inj.stats().reordered, 0u);
}

TEST_CASE(sim_packet_faults, random_faults_are_reproducible_and_conserve_packets) {
    auto run = [](std::uint64_t seed, exsim::sim::PacketFaultStats& stats) {
        PacketFaultInjector inj(64, 8);
        PacketFaultConfig cfg;
        cfg.drop_ppm = 50'000;
        cfg.duplicate_ppm = 50'000;
        cfg.reorder_ppm = 50'000;
        cfg.reorder_delay = 3;
        cfg.seed = seed;
        inj.configure(cfg);
        Recorder rec;
        for (int i = 0; i < 20'000; ++i) send(inj, rec, i);
        inj.flush(rec);
        stats = inj.stats();
        return rec.seen;
    };
    exsim::sim::PacketFaultStats s1{}, s2{};
    const auto a = run(99, s1);
    const auto b = run(99, s2);
    CHECK(a == b);
    CHECK_GT(s1.dropped, 0u);
    CHECK_GT(s1.duplicated, 0u);
    CHECK_GT(s1.reordered, 0u);
    // Every packet is either dropped or delivered at least once.
    CHECK_EQ(s1.delivered, s1.submitted - s1.dropped + s1.duplicated);
    CHECK_EQ(a.size(), s1.delivered);
}
