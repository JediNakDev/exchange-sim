#ifndef EXSIM_SIM_PACKET_FAULTS_HPP
#define EXSIM_SIM_PACKET_FAULTS_HPP

// Deterministic fault injection for datagram streams (market-data packets).
//
// Sits between a packet producer (MoldUDP64 publisher) and the socket:
//
//   publisher ──packet──► PacketFaultInjector::submit(pkt, sink) ──► sink(pkt)
//
// and can drop, duplicate or reorder packets, either randomly (seeded rates
// in parts-per-million, reproducible from the seed) or as one-shot admin
// commands ("drop the next 5 packets"). A reordered packet is copied into a
// pre-allocated hold slot and released after `reorder_delay` further
// submissions, i.e. later packets overtake it. Nothing allocates after
// construction; a packet that cannot be held (no free slot or larger than
// the slot size) is delivered normally.
//
// This is what lets client feed handlers be tested against gaps,
// duplicates and out-of-order delivery with the real wire format.

#include "exsim/sim/random.hpp"
#include "lob/compiler.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

namespace exsim::sim {

struct PacketFaultConfig {
    std::uint32_t drop_ppm = 0;       // probability a packet is lost
    std::uint32_t duplicate_ppm = 0;  // probability a delivered packet is sent twice
    std::uint32_t reorder_ppm = 0;    // probability a packet is held back
    std::uint32_t reorder_delay = 1;  // held packet is released after this many later packets (>= 1)
    std::uint64_t seed = 1;
};

struct PacketFaultStats {
    std::uint64_t submitted = 0;
    std::uint64_t delivered = 0;  // sink calls, including duplicates and releases
    std::uint64_t dropped = 0;
    std::uint64_t duplicated = 0;
    std::uint64_t reordered = 0;
};

class PacketFaultInjector {
public:
    PacketFaultInjector(std::size_t max_packet_size, std::size_t max_held)
        : slot_size_(max_packet_size),
          max_held_(max_held),
          storage_(std::make_unique<char[]>(max_packet_size * (max_held == 0 ? 1 : max_held))),
          slots_(std::make_unique<Slot[]>(max_held == 0 ? 1 : max_held)) {}

    void configure(const PacketFaultConfig& cfg) noexcept {
        cfg_ = cfg;
        if (cfg_.reorder_delay == 0) {
            cfg_.reorder_delay = 1;
        }
        rng_.reseed(cfg.seed);
    }
    [[nodiscard]] const PacketFaultConfig& config() const noexcept { return cfg_; }

    // One-shot faults applied to the next n submitted packets (before random
    // faults). Calls accumulate.
    void drop_next(std::uint32_t n) noexcept { drop_next_ += n; }
    void duplicate_next(std::uint32_t n) noexcept { duplicate_next_ += n; }
    void reorder_next(std::uint32_t n) noexcept { reorder_next_ += n; }

    // True when no fault can fire and nothing is held: callers may bypass
    // the injector entirely.
    [[nodiscard]] bool idle() const noexcept {
        return held_ == 0 && drop_next_ == 0 && duplicate_next_ == 0 && reorder_next_ == 0 &&
               cfg_.drop_ppm == 0 && cfg_.duplicate_ppm == 0 && cfg_.reorder_ppm == 0;
    }

    // Sink: callable as sink(std::span<const char>).
    template <typename Sink>
    void submit(std::span<const char> pkt, Sink&& sink) {
        ++stats_.submitted;
        const bool drop = take(drop_next_) || rng_.chance_ppm(cfg_.drop_ppm);
        if (drop) {
            ++stats_.dropped;
        } else if ((take(reorder_next_) || rng_.chance_ppm(cfg_.reorder_ppm)) && hold(pkt)) {
            ++stats_.reordered;
        } else {
            emit(pkt, sink);
            if (take(duplicate_next_) || rng_.chance_ppm(cfg_.duplicate_ppm)) {
                ++stats_.duplicated;
                emit(pkt, sink);
            }
        }
        tick(sink);
    }

    // Releases every held packet, oldest first (e.g. at end of session).
    template <typename Sink>
    void flush(Sink&& sink) {
        while (held_ != 0) {
            release_oldest(sink, /*only_due=*/false);
        }
    }

    [[nodiscard]] const PacketFaultStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t held() const noexcept { return held_; }

private:
    struct Slot {
        std::uint64_t order = 0;      // insertion order, 0 = free
        std::uint32_t remaining = 0;  // submissions until release
        std::uint32_t len = 0;
    };

    static bool take(std::uint32_t& counter) noexcept {
        if (LOB_LIKELY(counter == 0)) {
            return false;
        }
        --counter;
        return true;
    }

    template <typename Sink>
    void emit(std::span<const char> pkt, Sink& sink) {
        ++stats_.delivered;
        sink(pkt);
    }

    bool hold(std::span<const char> pkt) noexcept {
        if (held_ == max_held_ || pkt.size() > slot_size_) {
            return false;
        }
        for (std::size_t i = 0; i < max_held_; ++i) {
            Slot& s = slots_[i];
            if (s.order == 0) {
                s.order = ++next_order_;
                // +1: the tick at the end of this same submit() must not count.
                s.remaining = cfg_.reorder_delay + 1;
                s.len = static_cast<std::uint32_t>(pkt.size());
                if (!pkt.empty()) {
                    std::memcpy(storage_.get() + i * slot_size_, pkt.data(), pkt.size());
                }
                ++held_;
                return true;
            }
        }
        return false;
    }

    // One submission elapsed: release every held packet that is due, oldest first.
    template <typename Sink>
    void tick(Sink& sink) {
        if (LOB_LIKELY(held_ == 0)) {
            return;
        }
        for (std::size_t i = 0; i < max_held_; ++i) {
            if (slots_[i].order != 0) {
                --slots_[i].remaining;
            }
        }
        while (held_ != 0 && release_oldest(sink, /*only_due=*/true)) {
        }
    }

    template <typename Sink>
    bool release_oldest(Sink& sink, bool only_due) {
        std::size_t best = max_held_;
        for (std::size_t i = 0; i < max_held_; ++i) {
            const Slot& s = slots_[i];
            if (s.order != 0 && (!only_due || s.remaining == 0) &&
                (best == max_held_ || s.order < slots_[best].order)) {
                best = i;
            }
        }
        if (best == max_held_) {
            return false;
        }
        Slot& s = slots_[best];
        emit(std::span<const char>(storage_.get() + best * slot_size_, s.len), sink);
        s.order = 0;
        --held_;
        return true;
    }

    std::size_t slot_size_;
    std::size_t max_held_;
    std::unique_ptr<char[]> storage_;
    std::unique_ptr<Slot[]> slots_;
    std::size_t held_ = 0;
    std::uint64_t next_order_ = 0;
    std::uint32_t drop_next_ = 0;
    std::uint32_t duplicate_next_ = 0;
    std::uint32_t reorder_next_ = 0;
    PacketFaultConfig cfg_{};
    PacketFaultStats stats_{};
    Rng rng_{1};
};

}  // namespace exsim::sim

#endif
