#include <benchmark/benchmark.h>
#include <exsim/protocol/fix44.hpp>
#include <exsim/protocol/itch50.hpp>
#include <exsim/protocol/moldudp64.hpp>
#include <exsim/protocol/ouch42.hpp>
namespace {
namespace ouch = exsim::protocol::ouch42;
namespace itch = exsim::protocol::itch50;
namespace fix = exsim::protocol::fix44;
namespace mold = exsim::protocol::moldudp64;
void BM_OuchEnterDecode(benchmark::State &state) {
  ouch::EnterOrder m, d;
  m.side = 'B';
  m.shares = 100;
  m.price = 1502500;
  char b[49];
  ouch::encode(m, b, sizeof b);
  for (auto _ : state) {
    benchmark::DoNotOptimize(b);
    benchmark::DoNotOptimize(ouch::decode(b, sizeof b, d));
    benchmark::DoNotOptimize(d);
  }
}
void BM_OuchExecutedEncode(benchmark::State &state) {
  ouch::OrderExecuted m;
  m.shares = 100;
  m.price = 1502500;
  char b[40];
  for (auto _ : state) {
    benchmark::DoNotOptimize(m);
    benchmark::DoNotOptimize(ouch::encode(m, b, sizeof b));
    benchmark::DoNotOptimize(b);
    benchmark::ClobberMemory();
  }
}
void BM_FixNewOrderParse(benchmark::State &state) {
  fix::NewOrderSingle m, d;
  m.client_order_id.assign("123");
  m.symbol.assign("AAPL");
  m.quantity = 100;
  m.has_price = true;
  m.price = 1502500;
  char b[512];
  fix::Header h{"CLIENT", "EXSIM", 1, 0};
  auto n = fix::encode(m, h, b, sizeof b);
  for (auto _ : state) {
    benchmark::DoNotOptimize(b);
    benchmark::DoNotOptimize(fix::decode(b, n, d));
    benchmark::DoNotOptimize(d);
  }
}
void BM_FixExecutionBuild(benchmark::State &state) {
  fix::ExecutionReport m;
  m.order_id.assign("1");
  m.client_order_id.assign("123");
  m.execution_id.assign("1");
  m.symbol.assign("AAPL");
  m.execution_type = 'F';
  m.order_status = '2';
  m.quantity = 100;
  m.last_quantity = 100;
  m.price = 1502500;
  m.last_price = 1502500;
  m.average_price = 1502500;
  m.cumulative_quantity = 100;
  fix::Header h{"EXSIM", "CLIENT", 1, 0};
  char b[1024];
  for (auto _ : state) {
    benchmark::DoNotOptimize(m);
    benchmark::DoNotOptimize(fix::encode(m, h, b, sizeof b));
    benchmark::DoNotOptimize(b);
    benchmark::ClobberMemory();
  }
}
void BM_ItchAddEncode(benchmark::State &state) {
  itch::AddOrder m;
  m.side = exsim::Side::BUY;
  m.shares = 100;
  m.price = 1502500;
  char b[36];
  for (auto _ : state) {
    benchmark::DoNotOptimize(m);
    benchmark::DoNotOptimize(itch::encode(m, b, sizeof b));
    benchmark::DoNotOptimize(b);
    benchmark::ClobberMemory();
  }
}
void BM_MoldPacketBuild(benchmark::State &state) {
  char b[1400], message[36];
  itch::AddOrder m;
  itch::encode(m, message, sizeof message);
  mold::PacketBuilder builder(b);
  for (auto _ : state) {
    builder.reset("SESSION", 1);
    for (unsigned i = 0; i < 30; ++i)
      benchmark::DoNotOptimize(builder.append(message));
    benchmark::DoNotOptimize(builder.packet());
    benchmark::DoNotOptimize(b);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * 30);
}
} // namespace
BENCHMARK(BM_OuchEnterDecode)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OuchExecutedEncode)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_FixNewOrderParse)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_FixExecutionBuild)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_ItchAddEncode)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_MoldPacketBuild)->Unit(benchmark::kNanosecond);
