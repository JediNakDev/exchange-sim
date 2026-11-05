// exsim_codec_bench: cost of the wire codecs on the order entry and market
// data paths, one message at a time, as the gateways call them.
//
//   inbound order    framed client bytes -> typed order
//                    OUCH 4.2 EnterOrder in a SoupBinTCP 'U' packet,
//                    FIX 4.4 NewOrderSingle (framing, checksum, fields),
//                    SBE NewOrderSingle in a SOFH frame
//   outbound fill    typed fill -> framed bytes
//                    OUCH OrderExecuted wrapped in a SoupBinTCP 'S' packet,
//                    FIX ExecutionReport (header, body length, checksum),
//                    SBE ExecutionReportTrade
//   market data      ITCH 5.0 AddOrder and OrderExecuted encode, AddOrder decode
//
// Each case cycles through 256 distinct prebuilt inputs, so the measured loop
// is the codec call and a fold of its result. A round times N calls with
// steady_clock; the report gives the median and minimum ns per message over
// the rounds. Heap allocations are counted during every round and must be zero.
//
// Usage: exsim_codec_bench [--iterations N] [--rounds R]

#include <exsim/protocol/fix44.hpp>
#include <exsim/protocol/itch50.hpp>
#include <exsim/protocol/ouch42.hpp>
#include <exsim/protocol/sbe_order_entry.hpp>
#include <exsim/protocol/soupbintcp.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {
std::atomic<bool> g_count_allocations{false};
std::atomic<std::uint64_t> g_allocations{0};
}  // namespace

// Every replacement allocates with malloc, so free() is the matching release.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t size) {
    if (g_count_allocations.load(std::memory_order_relaxed)) g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

using namespace exsim;
namespace fix = protocol::fix44;
namespace itch = protocol::itch50;
namespace ouch = protocol::ouch42;
namespace sbe = protocol::sbe_order_entry;
namespace soup = protocol::soupbintcp;

constexpr std::size_t kInputCount = 256;  // distinct inputs per case, a power of two
constexpr Price kBasePrice = 1'502'500;   // 150.2500

// Keeps a value alive without letting the compiler see how it is used.
inline void keep(std::uint64_t value) noexcept { __asm__ volatile("" : : "r"(value) : "memory"); }

// Tells the compiler that memory behind the pointer is read here, so a decode
// must fill every field and an encode must store every byte, each iteration.
inline void escape(const void* pointer) noexcept { __asm__ volatile("" : : "r"(pointer) : "memory"); }

struct Measurement {
    std::string name;
    std::size_t wire_bytes = 0;
    double median_ns = 0;
    double minimum_ns = 0;
    std::uint64_t allocations = 0;
};

void fail(const char* what) {
    std::fprintf(stderr, "exsim_codec_bench: %s\n", what);
    std::exit(2);
}

// Body(index) returns a non-zero value derived from the codec's output, or 0
// when the codec call fails. Every input must succeed before timing starts, so
// no case measures an error path.
template <typename Body>
Measurement measure(const char* name, std::size_t wire_bytes, std::size_t iterations, std::size_t rounds, Body body) {
    for (std::size_t index = 0; index < kInputCount; ++index) {
        if (body(index) == 0) {
            std::fprintf(stderr, "exsim_codec_bench: %s fails on input %zu\n", name, index);
            std::exit(2);
        }
    }
    for (std::size_t index = 0; index < iterations / 10; ++index) keep(body(index));  // warm up
    std::vector<double> per_message;
    per_message.reserve(rounds);
    g_allocations.store(0);
    g_count_allocations.store(true);
    for (std::size_t round = 0; round < rounds; ++round) {
        std::uint64_t fold = 0;
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < iterations; ++index) fold += body(index);
        const auto stop = std::chrono::steady_clock::now();
        keep(fold);
        per_message.push_back(std::chrono::duration<double, std::nano>(stop - start).count() /
                              static_cast<double>(iterations));
    }
    g_count_allocations.store(false);
    std::sort(per_message.begin(), per_message.end());
    return {name, wire_bytes, per_message[per_message.size() / 2], per_message.front(), g_allocations.load()};
}

template <std::size_t N>
void set_token(std::array<char, N>& token, std::uint64_t value) {
    token.fill(' ');
    char digits[24];
    const int length = std::snprintf(digits, sizeof digits, "ORD%llu", static_cast<unsigned long long>(value));
    std::memcpy(token.data(), digits, std::min<std::size_t>(static_cast<std::size_t>(length), N));
}

using Frame = std::vector<char>;

// ---------------------------------------------------------------- inbound

std::vector<Frame> ouch_enter_order_frames() {
    std::vector<Frame> frames;
    for (std::size_t index = 0; index < kInputCount; ++index) {
        ouch::EnterOrder order;
        set_token(order.token, 1'000 + index);
        order.side = index % 2 ? 'S' : 'B';
        order.shares = static_cast<Quantity>(100 + index);
        order.stock = Symbol("AAPL");
        order.price = kBasePrice + static_cast<Price>(index) * 100;
        order.time_in_force = 99'999;
        order.display = 'Y';
        order.capacity = 'A';
        order.ise = 'N';
        order.cross_type = 'N';
        order.customer_type = 'R';
        char payload[64];
        const std::size_t payload_size = ouch::encode(order, payload, sizeof payload);
        Frame frame(payload_size + 3);
        if (payload_size == 0 ||
            soup::encode(soup::UnsequencedData{{payload, payload_size}}, frame.data(), frame.size()) != frame.size()) {
            fail("cannot build the OUCH EnterOrder input");
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

std::vector<Frame> fix_new_order_frames() {
    std::vector<Frame> frames;
    for (std::size_t index = 0; index < kInputCount; ++index) {
        fix::NewOrderSingle order;
        order.client_order_id.assign("ORD" + std::to_string(1'000 + index));
        order.symbol.assign("AAPL");
        order.side = index % 2 ? Side::SELL : Side::BUY;
        order.quantity = static_cast<Quantity>(100 + index);
        order.order_type = OrdType::Limit;
        order.price = kBasePrice + static_cast<Price>(index) * 100;
        order.has_price = true;
        order.time_in_force = TimeInForce::Day;
        order.transact_time = 1'762'300'800'000'000'000ULL + index * 1'000'000;
        const fix::Header header{"CLIENT01", "EXSIM", 2 + index, 1'762'300'800'000'000'000ULL + index * 1'000'000};
        Frame frame(512);
        const std::size_t size = fix::encode(order, header, frame.data(), frame.size());
        if (size == 0) fail("cannot build the FIX NewOrderSingle input");
        frame.resize(size);
        frames.push_back(std::move(frame));
    }
    return frames;
}

std::vector<Frame> sbe_new_order_frames() {
    std::vector<Frame> frames;
    for (std::size_t index = 0; index < kInputCount; ++index) {
        sbe::NewOrderSingle order;
        order.cl_ord_id = 1'000 + index;
        order.price_mantissa = kBasePrice + static_cast<Price>(index) * 100;
        order.order_qty = static_cast<std::uint32_t>(100 + index);
        order.symbol = {'A', 'A', 'P', 'L', 0, 0, 0, 0};
        order.side = index % 2 ? sbe::SideCode::Sell : sbe::SideCode::Buy;
        Frame frame(sbe::encoded_frame_size(order));
        if (sbe::encode(order, frame.data(), frame.size()) != frame.size()) fail("cannot build the SBE NewOrderSingle input");
        frames.push_back(std::move(frame));
    }
    return frames;
}

// ---------------------------------------------------------------- outbound

std::vector<ouch::OrderExecuted> ouch_fills() {
    std::vector<ouch::OrderExecuted> fills(kInputCount);
    for (std::size_t index = 0; index < kInputCount; ++index) {
        set_token(fills[index].token, 1'000 + index);
        fills[index].timestamp = 34'200'000'000'000ULL + index;
        fills[index].shares = static_cast<Quantity>(10 + index);
        fills[index].price = kBasePrice + static_cast<Price>(index) * 100;
        fills[index].liquidity = 'A';
        fills[index].match_number = 50'000 + index;
    }
    return fills;
}

std::vector<fix::ExecutionReport> fix_fills() {
    std::vector<fix::ExecutionReport> fills(kInputCount);
    for (std::size_t index = 0; index < kInputCount; ++index) {
        fix::ExecutionReport& fill = fills[index];
        fill.order_id.assign(std::to_string(70'000 + index));
        fill.client_order_id.assign("ORD" + std::to_string(1'000 + index));
        fill.execution_id.assign(std::to_string(90'000 + index));
        fill.execution_type = 'F';
        fill.order_status = '1';
        fill.symbol.assign("AAPL");
        fill.side = Side::BUY;
        fill.quantity = 200;
        fill.price = kBasePrice;
        fill.last_quantity = static_cast<Quantity>(10 + index);
        fill.last_price = kBasePrice + static_cast<Price>(index) * 100;
        fill.leaves_quantity = 190 - static_cast<Quantity>(index % 100);
        fill.cumulative_quantity = 10 + static_cast<Quantity>(index % 100);
        fill.average_price = kBasePrice;
        fill.transact_time = 1'762'300'800'000'000'000ULL + index * 1'000;
    }
    return fills;
}

std::vector<sbe::ExecutionReportTrade> sbe_fills() {
    std::vector<sbe::ExecutionReportTrade> fills(kInputCount);
    for (std::size_t index = 0; index < kInputCount; ++index) {
        sbe::ExecutionReportTrade& fill = fills[index];
        fill.order_id = 70'000 + index;
        fill.cl_ord_id = 1'000 + index;
        fill.exec_id = 90'000 + index;
        fill.transact_time_ns = 1'762'300'800'000'000'000ULL + index * 1'000;
        fill.last_px_mantissa = kBasePrice + static_cast<Price>(index) * 100;
        fill.trd_match_id = 50'000 + index;
        fill.last_qty = static_cast<std::uint32_t>(10 + index);
        fill.leaves_qty = 190;
        fill.cum_qty = 10;
        fill.symbol = {'A', 'A', 'P', 'L', 0, 0, 0, 0};
    }
    return fills;
}

// ---------------------------------------------------------------- market data

std::vector<itch::AddOrder> itch_adds() {
    std::vector<itch::AddOrder> adds(kInputCount);
    for (std::size_t index = 0; index < kInputCount; ++index) {
        adds[index].stock_locate = 1;
        adds[index].timestamp = 34'200'000'000'000ULL + index;
        adds[index].order_reference = 70'000 + index;
        adds[index].side = index % 2 ? Side::SELL : Side::BUY;
        adds[index].shares = static_cast<Quantity>(100 + index);
        adds[index].stock = Symbol("AAPL");
        adds[index].price = kBasePrice + static_cast<Price>(index) * 100;
    }
    return adds;
}

std::vector<itch::OrderExecuted> itch_executions() {
    std::vector<itch::OrderExecuted> executions(kInputCount);
    for (std::size_t index = 0; index < kInputCount; ++index) {
        executions[index].stock_locate = 1;
        executions[index].timestamp = 34'200'000'000'000ULL + index;
        executions[index].order_reference = 70'000 + index;
        executions[index].shares = static_cast<Quantity>(10 + index);
        executions[index].match_number = 50'000 + index;
    }
    return executions;
}

std::size_t parse_count(const char* text, const char* option) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value == 0) {
        std::fprintf(stderr, "exsim_codec_bench: %s needs a positive integer\n", option);
        std::exit(2);
    }
    return static_cast<std::size_t>(value);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 2'000'000;
    std::size_t rounds = 7;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if ((option == "--iterations" || option == "--rounds") && index + 1 < argc) {
            (option == "--iterations" ? iterations : rounds) = parse_count(argv[++index], option.c_str());
        } else {
            std::fprintf(stderr, "usage: exsim_codec_bench [--iterations N] [--rounds R]\n");
            return 2;
        }
    }

    const std::vector<Frame> ouch_orders = ouch_enter_order_frames();
    const std::vector<Frame> fix_orders = fix_new_order_frames();
    const std::vector<Frame> sbe_orders = sbe_new_order_frames();
    const std::vector<ouch::OrderExecuted> ouch_fill_messages = ouch_fills();
    const std::vector<fix::ExecutionReport> fix_fill_messages = fix_fills();
    const std::vector<sbe::ExecutionReportTrade> sbe_fill_messages = sbe_fills();
    const std::vector<itch::AddOrder> itch_add_messages = itch_adds();
    const std::vector<itch::OrderExecuted> itch_execution_messages = itch_executions();
    std::vector<Frame> itch_add_bytes;
    for (const itch::AddOrder& add : itch_add_messages) {
        Frame frame(itch::AddOrder::wire_size);
        if (itch::encode(add, frame.data(), frame.size()) != frame.size()) fail("cannot build the ITCH AddOrder input");
        itch_add_bytes.push_back(std::move(frame));
    }
    std::array<char, 1024> output{};
    std::vector<Measurement> results;

    // ------------------------------------------------------------ inbound order
    results.push_back(measure("OUCH 4.2 EnterOrder in SoupBinTCP, decode", ouch_orders[0].size(), iterations, rounds,
                              [&](std::size_t index) -> std::uint64_t {
                                  const Frame& frame = ouch_orders[index & (kInputCount - 1)];
                                  soup::UnsequencedData packet;
                                  if (soup::decode(frame.data(), frame.size(), packet) != protocol::Status::ok) return 0;
                                  ouch::EnterOrder order;
                                  if (ouch::decode(packet.payload.data(), packet.payload.size(), order) !=
                                      protocol::Status::ok) {
                                      return 0;
                                  }
                                  escape(&order);
                                  return order.shares + static_cast<std::uint64_t>(order.price);
                              }));
    results.push_back(measure("FIX 4.4 NewOrderSingle, frame + decode", fix_orders[0].size(), iterations, rounds,
                              [&](std::size_t index) -> std::uint64_t {
                                  const Frame& frame = fix_orders[index & (kInputCount - 1)];
                                  std::size_t size = 0;
                                  if (fix::frame_size({frame.data(), frame.size()}, size) != fix::Status::ok) return 0;
                                  fix::NewOrderSingle order;
                                  if (fix::decode(frame.data(), size, order) != fix::Status::ok) return 0;
                                  escape(&order);
                                  return order.quantity + static_cast<std::uint64_t>(order.price);
                              }));
    results.push_back(measure("SBE NewOrderSingle in SOFH, decode", sbe_orders[0].size(), iterations, rounds,
                              [&](std::size_t index) -> std::uint64_t {
                                  const Frame& frame = sbe_orders[index & (kInputCount - 1)];
                                  sbe::FrameView view;
                                  if (sbe::decode_frame(frame.data(), frame.size(), view) != sbe::DecodeStatus::Ok) {
                                      return 0;
                                  }
                                  sbe::NewOrderSingle order;
                                  if (sbe::decode(view, order) != sbe::DecodeStatus::Ok) return 0;
                                  escape(&order);
                                  return order.order_qty + static_cast<std::uint64_t>(order.price_mantissa);
                              }));

    // ------------------------------------------------------------ outbound fill
    std::size_t ouch_fill_bytes = 0;
    results.push_back(measure("OUCH 4.2 OrderExecuted in SoupBinTCP, encode", 0, iterations, rounds,
                              [&](std::size_t index) -> std::uint64_t {
                                  char payload[64];
                                  const std::size_t payload_size = ouch::encode(
                                      ouch_fill_messages[index & (kInputCount - 1)], payload, sizeof payload);
                                  const std::size_t size = soup::encode(
                                      soup::SequencedData{{payload, payload_size}}, output.data(), output.size());
                                  ouch_fill_bytes = size;
                                  escape(output.data());
                                  return size == 0 ? 0 : size + static_cast<unsigned char>(output[size - 1]);
                              }));
    results.back().wire_bytes = ouch_fill_bytes;
    std::size_t fix_fill_bytes = 0;
    results.push_back(measure("FIX 4.4 ExecutionReport (fill), encode", 0, iterations, rounds,
                              [&](std::size_t index) -> std::uint64_t {
                                  const fix::Header header{"EXSIM", "CLIENT01", 2 + index,
                                                           1'762'300'800'000'000'000ULL + index};
                                  const std::size_t size = fix::encode(fix_fill_messages[index & (kInputCount - 1)],
                                                                       header, output.data(), output.size());
                                  fix_fill_bytes = size;
                                  escape(output.data());
                                  return size == 0 ? 0 : size + static_cast<unsigned char>(output[size - 2]);
                              }));
    results.back().wire_bytes = fix_fill_bytes;
    results.push_back(measure("SBE ExecutionReportTrade in SOFH, encode",
                              sbe::encoded_frame_size(sbe_fill_messages[0]), iterations, rounds,
                              [&](std::size_t index) -> std::uint64_t {
                                  const std::size_t size = sbe::encode(sbe_fill_messages[index & (kInputCount - 1)],
                                                                       output.data(), output.size());
                                  escape(output.data());
                                  return size == 0 ? 0 : size + static_cast<unsigned char>(output[size - 1]);
                              }));

    // ------------------------------------------------------------ market data
    results.push_back(measure("ITCH 5.0 AddOrder, encode", itch::AddOrder::wire_size, iterations, rounds,
                              [&](std::size_t index) -> std::uint64_t {
                                  const std::size_t size = itch::encode(itch_add_messages[index & (kInputCount - 1)],
                                                                        output.data(), output.size());
                                  escape(output.data());
                                  return size == 0 ? 0 : size + static_cast<unsigned char>(output[size - 1]);
                              }));
    results.push_back(measure("ITCH 5.0 OrderExecuted, encode", itch::OrderExecuted::wire_size, iterations, rounds,
                              [&](std::size_t index) -> std::uint64_t {
                                  const std::size_t size = itch::encode(
                                      itch_execution_messages[index & (kInputCount - 1)], output.data(), output.size());
                                  escape(output.data());
                                  return size == 0 ? 0 : size + static_cast<unsigned char>(output[size - 1]);
                              }));
    results.push_back(measure("ITCH 5.0 AddOrder, decode", itch::AddOrder::wire_size, iterations, rounds,
                              [&](std::size_t index) -> std::uint64_t {
                                  const Frame& frame = itch_add_bytes[index & (kInputCount - 1)];
                                  itch::AddOrder add;
                                  if (itch::decode(frame.data(), frame.size(), add) != protocol::Status::ok) return 0;
                                  escape(&add);
                                  return add.order_reference + static_cast<std::uint64_t>(add.price);
                              }));

    bool allocated = false;
    std::printf("%-46s %6s %10s %10s %9s %7s\n", "case", "bytes", "median ns", "min ns", "Mmsg/s", "allocs");
    for (const Measurement& result : results) {
        std::printf("%-46s %6zu %10.1f %10.1f %9.2f %7llu\n", result.name.c_str(), result.wire_bytes, result.median_ns,
                    result.minimum_ns, 1'000.0 / result.median_ns, static_cast<unsigned long long>(result.allocations));
        allocated = allocated || result.allocations != 0;
    }
    std::printf("%zu rounds of %zu messages per case\n", rounds, iterations);
    if (allocated) {
        std::fprintf(stderr, "exsim_codec_bench: a codec path allocated on the heap\n");
        return 1;
    }
    return 0;
}
