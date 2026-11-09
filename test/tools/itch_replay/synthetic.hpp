#ifndef LOB_TOOLS_SYNTHETIC_HPP
#define LOB_TOOLS_SYNTHETIC_HPP

// Deterministic ITCH 5.0 stream generator. Emits wire-format bytes for
// A / E / X / D / U messages so the replayer's parse path is exercised
// end-to-end. The mix mirrors a realistic exchange tape: ~70% adds at
// session start, then 60% cancels / deletes during steady state, 10%
// executes, 5% replaces. All adds are non-marketable by construction
// (buys below current best ask, sells above current best bid).

#include <lob/protocol/itch.hpp>
#include <lob/types.hpp>
#include <cstdint>
#include <cstring>
#include <random>
#include <unordered_map>
#include <vector>

namespace lob::tools {

class SyntheticItchStream {
public:
    SyntheticItchStream(std::uint64_t seed, std::size_t n_messages,
                        Price mid_price, Price half_spread = 50)
        : rng_(seed), n_(n_messages), mid_(mid_price), half_(half_spread) {}

    std::vector<char> build() {
        std::vector<char> out;
        out.reserve(n_ * 32);
        warmup_book(out);
        steady_state(out);
        return out;
    }

private:
    std::mt19937_64 rng_;
    std::size_t n_;
    Price mid_;
    Price half_;
    std::uint64_t next_ref_ = 1;
    std::uint64_t ts_ = 0;
    std::vector<std::uint64_t> live_refs_;
    std::unordered_map<std::uint64_t, Side> side_of_;

    void warmup_book(std::vector<char>& out) {
        // Seed ~5% of total budget with adds on both sides.
        std::size_t seed_n = std::max<std::size_t>(64, n_ / 20);
        for (std::size_t i = 0; i < seed_n && out.size() / 32 < n_; ++i) {
            emit_add(out);
        }
    }

    void steady_state(std::vector<char>& out) {
        std::uniform_int_distribution<int> pick(0, 99);
        while (emitted_ < n_) {
            int r = pick(rng_);
            if (r < 50 || live_refs_.empty()) emit_add(out);
            else if (r < 75)                  emit_delete(out);
            else if (r < 90)                  emit_cancel(out);
            else if (r < 97)                  emit_execute(out);
            else                              emit_replace(out);
        }
    }

    std::size_t emitted_ = 0;

    Price random_passive_price(Side s) {
        std::uniform_int_distribution<Price> jitter(0, half_ * 4);
        Price j = jitter(rng_);
        return (s == Side::BUY) ? (mid_ - half_ - j)
                                : (mid_ + half_ + j);
    }

    void emit_add(std::vector<char>& out) {
        char buf[36] = {0};
        buf[0] = 'A';
        // stock_locate / tracking (offsets 1,3) — left zero
        std::uniform_int_distribution<int> side_d(0, 1);
        Side s = side_d(rng_) ? Side::BUY : Side::SELL;
        Price p = random_passive_price(s);
        std::uniform_int_distribution<std::uint32_t> qty_d(100, 1000);
        std::uint32_t qty = qty_d(rng_);

        std::uint64_t ref = next_ref_++;
        ts_ += 100;
        write_be48(buf + 5, ts_);
        write_be64(buf + 11, ref);
        buf[19] = (s == Side::BUY) ? 'B' : 'S';
        write_be32(buf + 20, qty);
        // stock symbol (offsets 24..31) left as spaces — leave zero, fine.
        write_be32(buf + 32, static_cast<std::uint32_t>(p));

        out.insert(out.end(), buf, buf + 36);
        live_refs_.push_back(ref);
        side_of_[ref] = s;
        ++emitted_;
    }

    std::uint64_t pop_random_ref() {
        std::uniform_int_distribution<std::size_t> idx_d(0, live_refs_.size() - 1);
        std::size_t i = idx_d(rng_);
        std::uint64_t ref = live_refs_[i];
        live_refs_[i] = live_refs_.back();
        live_refs_.pop_back();
        side_of_.erase(ref);
        return ref;
    }

    void emit_delete(std::vector<char>& out) {
        char buf[19] = {0};
        buf[0] = 'D';
        ts_ += 100;
        write_be48(buf + 5, ts_);
        write_be64(buf + 11, pop_random_ref());
        out.insert(out.end(), buf, buf + 19);
        ++emitted_;
    }

    void emit_cancel(std::vector<char>& out) {
        // Partial cancel: don't pop, just subtract some shares.
        char buf[23] = {0};
        buf[0] = 'X';
        ts_ += 100;
        std::uniform_int_distribution<std::size_t> idx_d(0, live_refs_.size() - 1);
        std::uint64_t ref = live_refs_[idx_d(rng_)];
        write_be48(buf + 5, ts_);
        write_be64(buf + 11, ref);
        std::uniform_int_distribution<std::uint32_t> q(10, 100);
        write_be32(buf + 19, q(rng_));
        out.insert(out.end(), buf, buf + 23);
        ++emitted_;
    }

    void emit_execute(std::vector<char>& out) {
        char buf[31] = {0};
        buf[0] = 'E';
        ts_ += 100;
        std::uniform_int_distribution<std::size_t> idx_d(0, live_refs_.size() - 1);
        std::uint64_t ref = live_refs_[idx_d(rng_)];
        write_be48(buf + 5, ts_);
        write_be64(buf + 11, ref);
        std::uniform_int_distribution<std::uint32_t> q(10, 50);
        write_be32(buf + 19, q(rng_));
        write_be64(buf + 23, ts_); // match number
        out.insert(out.end(), buf, buf + 31);
        ++emitted_;
    }

    void emit_replace(std::vector<char>& out) {
        char buf[35] = {0};
        buf[0] = 'U';
        ts_ += 100;
        std::uniform_int_distribution<std::size_t> idx_d(0, live_refs_.size() - 1);
        std::size_t i = idx_d(rng_);
        std::uint64_t old_ref = live_refs_[i];
        Side s = side_of_[old_ref];     // ITCH 'U' preserves the original side
        // Remove old ref from live set.
        live_refs_[i] = live_refs_.back();
        live_refs_.pop_back();
        side_of_.erase(old_ref);
        std::uint64_t new_ref = next_ref_++;
        Price p = random_passive_price(s);
        std::uniform_int_distribution<std::uint32_t> qty_d(100, 1000);
        write_be48(buf + 5, ts_);
        write_be64(buf + 11, old_ref);
        write_be64(buf + 19, new_ref);
        write_be32(buf + 27, qty_d(rng_));
        write_be32(buf + 31, static_cast<std::uint32_t>(p));
        out.insert(out.end(), buf, buf + 35);
        live_refs_.push_back(new_ref);
        side_of_[new_ref] = s;
        ++emitted_;
    }

    static void write_be32(char* p, std::uint32_t v) {
        p[0] = char((v >> 24) & 0xFF);
        p[1] = char((v >> 16) & 0xFF);
        p[2] = char((v >>  8) & 0xFF);
        p[3] = char(v & 0xFF);
    }
    static void write_be48(char* p, std::uint64_t v) {
        p[0] = char((v >> 40) & 0xFF);
        p[1] = char((v >> 32) & 0xFF);
        p[2] = char((v >> 24) & 0xFF);
        p[3] = char((v >> 16) & 0xFF);
        p[4] = char((v >>  8) & 0xFF);
        p[5] = char(v & 0xFF);
    }
    static void write_be64(char* p, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) p[i] = char((v >> (56 - i * 8)) & 0xFF);
    }
};

}  // namespace lob::tools

#endif
