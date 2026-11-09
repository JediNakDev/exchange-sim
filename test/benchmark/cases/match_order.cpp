#include "../utils/constants.hpp"
#include "../utils/csv_writer.hpp"
#include "../utils/stats.hpp"
#include "../utils/warmup.hpp"
#include "../utils/workload.hpp"
#include <lob/order_book.hpp>

using namespace bench;

static void BM_MatchOrder(benchmark::State& state) {
    warmup();
    const auto& w = workload();
    std::vector<double> latencies;
    latencies.reserve(BENCHMARK_SAMPLES);

    for (auto _ : state) {
        state.PauseTiming();
        PrePopulatedBook prepop(50, 5);
        auto& book = prepop.book();
        latencies.clear();
        size_t idx = 0;
        size_t total_fills = 0;
        state.ResumeTiming();

        for (size_t i = 0; i < BENCHMARK_SAMPLES; ++i) {
            const auto& order = w.get(idx++);
            auto best_bid = book.get_best_bid();
            auto best_ask = book.get_best_ask();

            lob::Price price;
            lob::Side side;
            if (i % 2 == 0 && best_ask) {
                price = *best_ask;
                side = lob::Side::BUY;
            } else if (best_bid) {
                price = *best_bid;
                side = lob::Side::SELL;
            } else {
                price = order.price;
                side = order.side;
            }

            auto start = std::chrono::high_resolution_clock::now();
            auto result = book.add_order(price, order.quantity, side);
            auto end = std::chrono::high_resolution_clock::now();

            benchmark::DoNotOptimize(result);
            total_fills += result.fills.size();
            latencies.push_back(std::chrono::duration<double, std::nano>(end - start).count());

            if (book.get_total_orders() < 100) {
                for (int j = 0; j < 10; ++j) {
                    const auto& refill = w.get(idx++);
                    lob::Price refill_price = refill.side == lob::Side::BUY
                                              ? BASE_PRICE - (j + 1) * TICK_SIZE
                                              : BASE_PRICE + (j + 1) * TICK_SIZE;
                    (void)book.add_order(refill_price, refill.quantity, refill.side);
                }
            }
        }

        state.counters["TotalFills"] = static_cast<double>(total_fills);
        state.counters["FillRate"] = static_cast<double>(total_fills) / BENCHMARK_SAMPLES;
    }

    auto stats = Stats::compute(latencies);
    stats.report(state);
    if (csv()) csv()->write("MatchOrder", stats);
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * BENCHMARK_SAMPLES));
    state.SetLabel("Aggressive orders crossing spread");
}

BENCHMARK(BM_MatchOrder)->Unit(benchmark::kNanosecond)->MinTime(3.0);
