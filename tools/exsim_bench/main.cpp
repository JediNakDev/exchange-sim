// exsim_bench — deterministic matching-engine benchmark.
//
// Every workload is generated up front by driving a scratch engine and
// reacting to its events (so cancels/replaces always target live orders),
// then replayed on a fresh engine. Because the engine is a pure function of
// its command stream, the replay sees exactly the same book states. The
// measured loop is therefore nothing but `engine.handle(cmd, out)`.
//
// Two passes per workload:
//   throughput  one timestamp around the whole replay -> Mops/s, mean ns/op
//   latency     a counter read around every command   -> percentiles
// Heap allocations during both passes are counted via a global operator new
// override and must be zero.
//
// Note: Apple Silicon's counter ticks at 24 MHz (41.7 ns), so per-op
// percentiles there are quantised; x86 rdtsc gives cycle resolution.
//
// Usage: exsim_bench [--ops N] [--seed S] [--workload NAME] [--csv FILE]

#include <exsim/matching_engine.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Allocation counter
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_count_allocs{false};
std::atomic<std::uint64_t> g_allocs{0};
}  // namespace

void* operator new(std::size_t n) {
    if (g_count_allocs.load(std::memory_order_relaxed)) {
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(n == 0 ? 1 : n)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace exsim;

inline std::uint64_t read_counter() noexcept {
#if defined(__x86_64__)
    unsigned lo, hi;
    __asm__ volatile("lfence\nrdtsc" : "=a"(lo), "=d"(hi)::"memory");
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__)
    std::uint64_t v;
    __asm__ volatile("isb\nmrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

double counter_ticks_per_ns() {
    const auto c0 = std::chrono::steady_clock::now();
    const std::uint64_t t0 = read_counter();
    while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(50)) {
    }
    const std::uint64_t t1 = read_counter();
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - c0).count();
    return static_cast<double>(t1 - t0) / ns;
}

struct Rng {
    std::uint64_t s;
    std::uint64_t next() noexcept {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::uint64_t below(std::uint64_t n) noexcept { return next() % n; }
};

constexpr Price kTick = 100;
constexpr Price kMid = 1'000'000;
constexpr SessionId kSessions = 8;

InstrumentConfig bench_instrument() {
    InstrumentConfig cfg;
    cfg.symbol = Symbol("BENCH");
    cfg.tick_size = kTick;
    cfg.lot_size = 1;
    cfg.max_order_qty = 1'000'000;
    cfg.min_price = 500'000;
    cfg.max_price = 1'500'000;
    cfg.reference_price = kMid;
    cfg.initial_phase = Phase::Continuous;
    return cfg;
}

// Tracks live (session, token) pairs from the event stream so the generator
// can pick random live orders in O(1).
class LiveSet {
public:
    void add(SessionId s, ClOrdId c) {
        const std::uint64_t k = key(s, c);
        index_[k] = keys_.size();
        keys_.push_back(k);
    }
    void remove(SessionId s, ClOrdId c) {
        const auto it = index_.find(key(s, c));
        if (it == index_.end()) return;
        const std::size_t i = it->second;
        index_[keys_.back()] = i;
        keys_[i] = keys_.back();
        keys_.pop_back();
        index_.erase(it);
    }
    [[nodiscard]] bool empty() const { return keys_.empty(); }
    [[nodiscard]] std::size_t size() const { return keys_.size(); }
    std::pair<SessionId, ClOrdId> pick(Rng& rng) const {
        const std::uint64_t k = keys_[rng.below(keys_.size())];
        return {static_cast<SessionId>(k >> 48), k & kMaxClOrdId};
    }

private:
    static std::uint64_t key(SessionId s, ClOrdId c) { return (std::uint64_t{s} << 48) | c; }
    std::vector<std::uint64_t> keys_;
    std::unordered_map<std::uint64_t, std::size_t> index_;
};

void track(const EventBuffer& out, LiveSet& live) {
    for (const Event& e : out) {
        switch (e.type) {
            case EventType::Rested: live.add(e.rested.session, e.rested.cl_ord_id); break;
            case EventType::Fill:
                if (e.fill.buy.leaves_qty == 0) live.remove(e.fill.buy.session, e.fill.buy.cl_ord_id);
                if (e.fill.sell.leaves_qty == 0) live.remove(e.fill.sell.session, e.fill.sell.cl_ord_id);
                break;
            case EventType::Canceled:
                if (e.canceled.was_resting) live.remove(e.canceled.session, e.canceled.cl_ord_id);
                break;
            case EventType::Replaced:
                if (e.replaced.cl_ord_id != e.replaced.orig_cl_ord_id) {
                    live.remove(e.replaced.session, e.replaced.orig_cl_ord_id);
                    live.add(e.replaced.session, e.replaced.cl_ord_id);
                }
                break;
            default: break;
        }
    }
}

struct Workload {
    std::string name;
    std::string description;
    std::vector<Command> setup;   // replayed untimed
    std::vector<Command> timed;   // measured
};

class Generator {
public:
    explicit Generator(std::uint64_t seed) : rng_{seed | 1}, engine_(config()) {
        instrument_ = engine_.add_instrument(bench_instrument());
    }

    static EngineConfig config() {
        EngineConfig cfg;
        cfg.max_instruments = 4;
        cfg.max_orders = 1u << 21;
        return cfg;
    }

    void emit(std::vector<Command>& dst, const Command& cmd) {
        out_.clear();
        engine_.handle(cmd, out_);
        track(out_, live_);
        dst.push_back(cmd);
    }

    NewOrder passive(Side side, int max_ticks_away) {
        NewOrder o;
        o.ts = ++ts_;
        o.instrument = instrument_;
        o.session = static_cast<SessionId>(1 + rng_.below(kSessions));
        o.owner = o.session;
        o.cl_ord_id = ++next_token_;
        o.side = side;
        const Price away = kTick * static_cast<Price>(1 + rng_.below(static_cast<std::uint64_t>(max_ticks_away)));
        o.price = side == Side::BUY ? kMid - away : kMid + away;
        o.qty = static_cast<Quantity>(100 * (1 + rng_.below(10)));
        return o;
    }

    NewOrder aggressive(Side side, TimeInForce tif, int ticks_through) {
        NewOrder o = passive(side, 1);
        o.tif = tif;
        const Price through = kTick * static_cast<Price>(rng_.below(static_cast<std::uint64_t>(ticks_through) + 1));
        o.price = side == Side::BUY ? kMid + kTick + through : kMid - kTick - through;
        o.qty = static_cast<Quantity>(100 * (1 + rng_.below(5)));
        return o;
    }

    CancelOrder cancel_random() {
        const auto [session, token] = live_.pick(rng_);
        CancelOrder c;
        c.ts = ++ts_;
        c.session = session;
        c.orig_cl_ord_id = token;
        return c;
    }

    ReplaceOrder replace_random() {
        const auto [session, token] = live_.pick(rng_);
        const auto info = engine_.find_order(session, token);
        ReplaceOrder r;
        r.ts = ++ts_;
        r.session = session;
        r.orig_cl_ord_id = token;
        if (rng_.below(2) == 0) {
            // Quantity down at the same price: keeps queue priority.
            r.price = info->price;
            r.qty = info->cum_qty + std::max<Quantity>(1, info->leaves_qty / 2);
        } else {
            // Price move one tick away from the touch: loses priority.
            r.price = info->side == Side::BUY ? info->price - kTick : info->price + kTick;
            r.price = std::clamp<Price>(r.price, 500'000, 1'500'000);
            r.qty = info->qty;
            r.cl_ord_id = ++next_token_;
        }
        return r;
    }

    Rng& rng() { return rng_; }
    LiveSet& live() { return live_; }

private:
    Rng rng_;
    MatchingEngine engine_;
    EventBuffer out_;
    LiveSet live_;
    InstrumentId instrument_ = 0;
    Timestamp ts_ = 0;
    ClOrdId next_token_ = 0;
};

void seed_book(Generator& g, std::vector<Command>& dst, std::size_t per_side, int ticks) {
    for (std::size_t i = 0; i < per_side; ++i) {
        g.emit(dst, g.passive(Side::BUY, ticks));
        g.emit(dst, g.passive(Side::SELL, ticks));
    }
}

Workload make_passive_add(std::uint64_t seed, std::size_t ops) {
    Generator g(seed);
    Workload w{"passive_add", "new non-crossing limit orders, book grows", {}, {}};
    seed_book(g, w.setup, 1000, 100);
    for (std::size_t i = 0; i < ops; ++i) {
        g.emit(w.timed, g.passive(g.rng().below(2) ? Side::BUY : Side::SELL, 100));
    }
    return w;
}

Workload make_cancel(std::uint64_t seed, std::size_t ops) {
    Generator g(seed);
    Workload w{"cancel", "cancel random live orders", {}, {}};
    seed_book(g, w.setup, ops / 2 + 1000, 100);
    for (std::size_t i = 0; i < ops && !g.live().empty(); ++i) {
        g.emit(w.timed, g.cancel_random());
    }
    return w;
}

Workload make_aggressive(std::uint64_t seed, std::size_t ops) {
    Generator g(seed);
    Workload w{"aggressive", "IOC orders sweeping 1-3 levels; book replenished", {}, {}};
    seed_book(g, w.setup, 20'000, 50);
    for (std::size_t i = 0; i < ops; ++i) {
        if (i % 2 == 0) {
            g.emit(w.timed, g.aggressive(g.rng().below(2) ? Side::BUY : Side::SELL, TimeInForce::IOC, 2));
        } else {
            g.emit(w.timed, g.passive(g.rng().below(2) ? Side::BUY : Side::SELL, 3));
        }
    }
    return w;
}

// Order-entry mix resembling a lit equity venue: most orders are cancelled
// or amended, a few percent trade.
Workload make_mixed(std::uint64_t seed, std::size_t ops) {
    Generator g(seed);
    Workload w{"mixed", "45% new / 35% cancel / 12% replace / 8% aggressive", {}, {}};
    seed_book(g, w.setup, 10'000, 50);
    for (std::size_t i = 0; i < ops; ++i) {
        const std::uint64_t r = g.rng().below(100);
        const Side side = g.rng().below(2) ? Side::BUY : Side::SELL;
        if (r < 45 || g.live().size() < 1000) {
            g.emit(w.timed, g.passive(side, 50));
        } else if (r < 80) {
            g.emit(w.timed, g.cancel_random());
        } else if (r < 92) {
            g.emit(w.timed, g.replace_random());
        } else if (r < 97) {
            g.emit(w.timed, g.aggressive(side, TimeInForce::IOC, 1));
        } else {
            g.emit(w.timed, g.aggressive(side, TimeInForce::Day, 1));
        }
    }
    return w;
}

struct Result {
    std::string name;
    std::size_t ops = 0;
    double mops = 0;
    double mean_ns = 0;
    double p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
    std::uint64_t allocs = 0;
    std::uint64_t events = 0;
};

Result run(const Workload& w, double ticks_per_ns) {
    Result r;
    r.name = w.name;
    r.ops = w.timed.size();
    EventBuffer out(1 << 16);

    // Throughput pass.
    {
        MatchingEngine engine(Generator::config());
        (void)engine.add_instrument(bench_instrument());
        for (const Command& c : w.setup) {
            out.clear();
            engine.handle(c, out);
        }
        g_allocs = 0;
        g_count_allocs = true;
        const auto t0 = std::chrono::steady_clock::now();
        for (const Command& c : w.timed) {
            out.clear();
            engine.handle(c, out);
            r.events += out.size();
        }
        const auto t1 = std::chrono::steady_clock::now();
        g_count_allocs = false;
        r.allocs = g_allocs.load();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        r.mean_ns = ns / static_cast<double>(r.ops);
        r.mops = 1e3 / r.mean_ns;
    }

    // Latency pass.
    {
        MatchingEngine engine(Generator::config());
        (void)engine.add_instrument(bench_instrument());
        for (const Command& c : w.setup) {
            out.clear();
            engine.handle(c, out);
        }
        std::vector<std::uint64_t> ticks(w.timed.size());
        g_allocs = 0;
        g_count_allocs = true;
        for (std::size_t i = 0; i < w.timed.size(); ++i) {
            out.clear();
            const std::uint64_t t0 = read_counter();
            engine.handle(w.timed[i], out);
            ticks[i] = read_counter() - t0;
        }
        g_count_allocs = false;
        r.allocs += g_allocs.load();
        std::sort(ticks.begin(), ticks.end());
        auto pct = [&](double p) {
            const std::size_t i = std::min(ticks.size() - 1, static_cast<std::size_t>(p * static_cast<double>(ticks.size())));
            return static_cast<double>(ticks[i]) / ticks_per_ns;
        };
        r.p50 = pct(0.50);
        r.p90 = pct(0.90);
        r.p99 = pct(0.99);
        r.p999 = pct(0.999);
        r.p9999 = pct(0.9999);
        r.max = static_cast<double>(ticks.back()) / ticks_per_ns;
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t ops = 1'000'000;
    std::uint64_t seed = 0x5EED;
    std::string only;
    std::string csv;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ops" && i + 1 < argc) ops = std::stoull(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
        else if (a == "--workload" && i + 1 < argc) only = argv[++i];
        else if (a == "--csv" && i + 1 < argc) csv = argv[++i];
        else {
            std::fprintf(stderr, "usage: exsim_bench [--ops N] [--seed S] [--workload NAME] [--csv FILE]\n");
            return 1;
        }
    }

    const double tpns = counter_ticks_per_ns();
    std::printf("exsim_bench: ops=%zu seed=%llu counter=%.3f ticks/ns (resolution %.1f ns)\n\n", ops,
                static_cast<unsigned long long>(seed), tpns, 1.0 / tpns);

    using Maker = Workload (*)(std::uint64_t, std::size_t);
    const std::pair<const char*, Maker> makers[] = {
        {"passive_add", make_passive_add},
        {"cancel", make_cancel},
        {"aggressive", make_aggressive},
        {"mixed", make_mixed},
    };

    std::printf("%-12s %9s %8s %8s %8s %8s %8s %8s %9s %10s %7s\n", "workload", "ops", "Mops/s", "mean_ns",
                "p50", "p90", "p99", "p99.9", "p99.99", "max", "allocs");
    std::vector<Result> results;
    bool alloc_violation = false;
    for (const auto& [name, make] : makers) {
        if (!only.empty() && only != name) continue;
        const Workload w = make(seed, ops);
        const Result r = run(w, tpns);
        std::printf("%-12s %9zu %8.2f %8.1f %8.0f %8.0f %8.0f %8.0f %9.0f %10.0f %7llu\n", r.name.c_str(), r.ops,
                    r.mops, r.mean_ns, r.p50, r.p90, r.p99, r.p999, r.p9999, r.max,
                    static_cast<unsigned long long>(r.allocs));
        alloc_violation |= r.allocs != 0;
        results.push_back(r);
    }

    if (!csv.empty()) {
        if (FILE* f = std::fopen(csv.c_str(), "w")) {
            std::fprintf(f, "workload,ops,mops,mean_ns,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,allocs\n");
            for (const Result& r : results) {
                std::fprintf(f, "%s,%zu,%.3f,%.2f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%llu\n", r.name.c_str(), r.ops,
                             r.mops, r.mean_ns, r.p50, r.p90, r.p99, r.p999, r.p9999, r.max,
                             static_cast<unsigned long long>(r.allocs));
            }
            std::fclose(f);
        }
    }

    if (alloc_violation) {
        std::printf("\nFAIL: heap allocation on the hot path\n");
        return 2;
    }
    return 0;
}
