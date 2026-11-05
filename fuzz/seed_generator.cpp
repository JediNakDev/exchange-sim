// Writes the deterministic seed corpora for every fuzz target:
//
//   build/fuzz/seed_generator <output-directory>
//
// creates <output-directory>/<target>/seed-NNN files. Seeds are produced by
// the real encoders (so they start the campaign past the framing checks) plus
// hand-written text inputs; the committed fuzz/corpus/<target>/ directories
// are these seeds merged with what campaigns found (see docs/FUZZING.md).
// Same source, same bytes: no clock, no randomness.

#include <exsim/protocol/fix44.hpp>
#include <exsim/protocol/itch50.hpp>
#include <exsim/protocol/moldudp64.hpp>
#include <exsim/protocol/ouch42.hpp>
#include <exsim/protocol/sbe_order_entry.hpp>
#include <exsim/protocol/soupbintcp.hpp>
#include <exsim/sim/journal.hpp>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace fix = exsim::protocol::fix44;
namespace itch = exsim::protocol::itch50;
namespace mold = exsim::protocol::moldudp64;
namespace ouch = exsim::protocol::ouch42;
namespace sbe = exsim::protocol::sbe_order_entry;
namespace soup = exsim::protocol::soupbintcp;

using SeedBytes = std::string;
std::map<std::string, std::vector<SeedBytes>> seeds_by_target;

void add_seed(const std::string& target, SeedBytes bytes) { seeds_by_target[target].push_back(std::move(bytes)); }

std::string big_endian(std::uint64_t value, std::size_t width) {
    std::string bytes(width, '\0');
    for (std::size_t index = 0; index < width; ++index) {
        bytes[width - 1 - index] = static_cast<char>(value >> (8 * index));
    }
    return bytes;
}

template <typename Message>
std::string encoded_fixed_layout(const Message& message) {
    std::array<char, Message::wire_size> buffer{};
    if (encode(message, buffer.data(), buffer.size()) != Message::wire_size) {
        buffer.fill(' ');
        buffer[0] = Message::type;
    }
    return {buffer.data(), buffer.size()};
}

template <typename... Messages>
void add_fixed_layout_seeds(const std::string& target) {
    (add_seed(target, encoded_fixed_layout(Messages{})), ...);
}

std::string bytes_of(std::initializer_list<int> values) {
    std::string bytes;
    for (const int value : values) bytes += static_cast<char>(value);
    return bytes;
}

std::string soup_frame(char type, std::string_view payload) {
    return big_endian(payload.size() + 1, 2) + type + std::string(payload);
}

fix::Header fix_header(std::uint64_t sequence) {
    fix::Header header;
    header.sender = "CLIENT1";
    header.target = "EXSIM";
    header.sequence = sequence;
    header.sending_time = 1'700'000'000'123'000'000ull;
    return header;
}

template <typename Message>
std::string encoded_fix(const Message& message, std::uint64_t sequence) {
    std::array<char, 1024> buffer{};
    const std::size_t written = fix::encode(message, fix_header(sequence), buffer.data(), buffer.size());
    return {buffer.data(), written};
}

std::vector<std::string> fix_frames() {
    std::vector<std::string> frames;
    fix::Logon logon;
    logon.heartbeat_interval = 30;
    frames.push_back(encoded_fix(logon, 1));
    frames.push_back(encoded_fix(fix::Heartbeat{}, 2));
    fix::NewOrderSingle order;
    order.client_order_id.assign("ORD-1");
    order.symbol.assign("AAPL");
    order.side = exsim::Side::BUY;
    order.quantity = 100;
    order.order_type = exsim::OrdType::Limit;
    order.has_price = true;
    order.price = 1'502'500;
    order.transact_time = 1'700'000'000'000'000'000ull;
    frames.push_back(encoded_fix(order, 3));
    fix::OrderCancelRequest cancel;
    cancel.original_client_order_id.assign("ORD-1");
    cancel.client_order_id.assign("ORD-2");
    cancel.symbol.assign("AAPL");
    cancel.side = exsim::Side::BUY;
    frames.push_back(encoded_fix(cancel, 4));
    fix::ExecutionReport report;
    report.order_id.assign("1");
    report.client_order_id.assign("ORD-1");
    report.execution_id.assign("E1");
    report.execution_type = 'F';
    report.order_status = '2';
    report.symbol.assign("AAPL");
    report.side = exsim::Side::SELL;
    report.quantity = 100;
    report.price = 1'502'500;
    report.last_quantity = 100;
    report.last_price = 1'502'500;
    report.cumulative_quantity = 100;
    report.average_price = 1'502'500;
    frames.push_back(encoded_fix(report, 5));
    fix::Logout logout;
    logout.has_text = true;
    logout.text.assign("bye");
    frames.push_back(encoded_fix(logout, 6));
    std::erase_if(frames, [](const std::string& frame) { return frame.empty(); });
    return frames;
}

void add_protocol_seeds() {
    add_fixed_layout_seeds<ouch::EnterOrder, ouch::ReplaceOrder, ouch::CancelOrder, ouch::ModifyOrder>(
        "ouch42_inbound");
    add_fixed_layout_seeds<ouch::SystemEvent, ouch::OrderAccepted, ouch::OrderCanceled, ouch::OrderExecuted,
                           ouch::OrderRejected, ouch::CancelPending, ouch::CancelReject, ouch::OrderModified,
                           ouch::OrderReplaced>("ouch42_outbound");
    add_fixed_layout_seeds<itch::SystemEvent, itch::StockDirectory, itch::StockTradingAction, itch::AddOrder,
                           itch::AddOrderMPID, itch::OrderExecuted, itch::OrderExecutedWithPrice, itch::OrderCancel,
                           itch::OrderDelete, itch::OrderReplace, itch::Trade, itch::CrossTrade,
                           itch::NetOrderImbalance>("itch50");

    // SoupBinTCP: whole-input packets, then reader headers followed by a session's frame stream.
    soup::LoginRequest login;
    std::array<char, 47> login_bytes{};
    const std::size_t login_size = soup::encode(login, login_bytes.data(), login_bytes.size());
    const std::string login_payload(login_bytes.data() + 1, login_size > 0 ? login_size - 1 : 0);
    const std::string session_stream = soup_frame('L', login_payload) + soup_frame('R', "") +
                                       soup_frame('U', encoded_fixed_layout(ouch::EnterOrder{})) +
                                       soup_frame('+', "debug text") + soup_frame('O', "");
    add_seed("soupbintcp", soup_frame('R', ""));
    add_seed("soupbintcp", soup_frame('L', login_payload));
    add_seed("soupbintcp", std::string("\x10\x40") + big_endian(12345, 4) + session_stream);
    add_seed("soupbintcp", std::string("\xff\xff") + big_endian(7, 4) + session_stream);
    add_seed("soupbintcp", std::string("\x03\x02") + big_endian(1, 4) + soup_frame('S', "abc") + std::string("\0\0", 2));

    // MoldUDP64: a two-message packet, an end-of-session packet and a request.
    const std::string session_name = "EXSIM     ";
    add_seed("moldudp64", session_name + big_endian(1, 8) + big_endian(2, 2) + big_endian(3, 2) + "abc" +
                              big_endian(1, 2) + "z");
    add_seed("moldudp64", session_name + big_endian(42, 8) + big_endian(65535, 2));
    add_seed("moldudp64", session_name + big_endian(9, 8) + big_endian(4, 2));
    add_seed("moldudp64", std::string(64, 'm'));

    for (const std::string& frame : fix_frames()) add_seed("fix44_message", frame);
    std::string fix_stream;
    for (const std::string& frame : fix_frames()) fix_stream += frame;
    add_seed("fix44_stream", std::string("\x00\x40", 2) + big_endian(99, 4) + fix_stream);
    add_seed("fix44_stream", std::string("\x00\xff", 2) + big_endian(5, 4) + fix_stream);
    for (char mode = 1; mode <= 3; ++mode) {
        std::string structured(1, mode);
        for (int index = 0; index < 96; ++index) structured += static_cast<char>('A' + index % 26);
        add_seed("fix44_stream", structured);
    }

    add_seed("wire_text_fields", std::string("      12345"));
    add_seed("wire_text_fields", std::string("AAPL    \x01\x02\x03\x04\x05\x06\x07\x08", 16));
    add_seed("wire_text_fields", std::string(32, ' '));
}

template <typename Message>
std::string sbe_frame(const Message& message) {
    std::array<char, sbe::kMaxEncodedFrameSize> buffer{};
    const std::size_t size = sbe::encode(message, buffer.data(), buffer.size());
    return {buffer.data(), size};
}

// SBE over SOFH: one frame of every message type, a client session's frames
// back to back, a later schema version with an appended root block field,
// and frames broken in each header field.
void add_sbe_seeds() {
    const sbe::SessionUuid uuid{0x6f, 0x1c, 0x2a, 0x10, 0x55, 0x3b, 0x4e, 0x21,
                                0x9a, 0x77, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    const sbe::WireSymbol symbol{'A', 'A', 'P', 'L', 0, 0, 0, 0};
    sbe::Negotiate negotiate{uuid, 1'000, sbe::FlowType::Idempotent, "ALICE:alice"};
    sbe::Establish establish{uuid, 2'000, 1'000, 1, "ALICE:alice"};
    sbe::NewOrderSingle new_order{};
    new_order.cl_ord_id = 1;
    new_order.price_mantissa = 1'502'500;
    new_order.order_qty = 100;
    new_order.symbol = symbol;
    sbe::OrderCancelReplaceRequest replace{2, 1, 1'503'000, 150, sbe::BooleanCode::True};
    sbe::OrderMassCancelRequest mass_cancel{3, symbol, sbe::MassCancelRequestTypeCode::CancelOrdersForSecurity};
    sbe::ExecutionReportTrade trade{};
    trade.order_id = 7;
    trade.cl_ord_id = 1;
    trade.last_px_mantissa = 1'502'500;
    trade.last_qty = 40;
    trade.leaves_qty = 60;
    trade.cum_qty = 40;
    trade.symbol = symbol;
    sbe::OrderMassCancelReport mass_cancel_report{};
    mass_cancel_report.symbol = symbol;

    const std::string frames[] = {
        sbe_frame(negotiate),
        sbe_frame(sbe::NegotiationResponse{uuid, 1'000, sbe::FlowType::Recoverable, ""}),
        sbe_frame(sbe::NegotiationReject{uuid, 1'000, sbe::NegotiationRejectCode::Credentials, "bad credentials"}),
        sbe_frame(establish),
        sbe_frame(sbe::EstablishmentAck{uuid, 2'000, 1'000, 1}),
        sbe_frame(sbe::EstablishmentReject{uuid, 2'000, sbe::EstablishmentRejectCode{}, "rejected"}),
        sbe_frame(sbe::Sequence{5}),
        sbe_frame(sbe::UnsequencedHeartbeat{}),
        sbe_frame(sbe::RetransmitRequest{uuid, 3'000, 2, 3}),
        sbe_frame(sbe::Retransmission{uuid, 3'000, 2, 3}),
        sbe_frame(sbe::RetransmitReject{uuid, 3'000, sbe::RetransmitRejectCode::OutOfRange, "out of range"}),
        sbe_frame(sbe::Terminate{uuid, sbe::TerminationCode::Finished, "bye"}),
        sbe_frame(sbe::NotApplied{4, 2}),
        sbe_frame(new_order),
        sbe_frame(sbe::OrderCancelRequest{4, 1, 0}),
        sbe_frame(replace),
        sbe_frame(mass_cancel),
        sbe_frame(sbe::ExecutionReportNew{}),
        sbe_frame(trade),
        sbe_frame(sbe::ExecutionReportCancel{}),
        sbe_frame(sbe::ExecutionReportReplace{}),
        sbe_frame(sbe::ExecutionReportReject{}),
        sbe_frame(sbe::OrderCancelReject{}),
        sbe_frame(mass_cancel_report),
        sbe_frame(sbe::BusinessMessageReject{9, 100, sbe::BusinessRejectReasonCode::UnsupportedMessageType}),
    };
    for (const std::string& frame : frames) add_seed("sbe_order_entry", frame);
    add_seed("sbe_order_entry", frames[0] + frames[3] + frames[13] + frames[14] + frames[15] + frames[16] + frames[11]);

    // Version 2 NewOrderSingle: four appended root block bytes, then one extra variable length field.
    std::string later_version = frames[13];
    later_version.append("\x01\x02\x03\x04", 4);
    later_version[6] = static_cast<char>(sbe::NewOrderSingle::kBlockLength + 4);  // blockLength, little-endian
    later_version[12] = 2;                                                       // version
    later_version.append("\x03\x00xyz", 5);
    later_version[17] = 1;  // numVarDataFields
    const std::string frame_size = big_endian(later_version.size(), 4);
    later_version.replace(0, 4, frame_size);
    add_seed("sbe_order_entry", later_version);

    std::string broken = frames[13];
    broken[4] = 0x00;  // encoding type
    add_seed("sbe_order_entry", broken);
    broken = frames[13];
    broken[10] = 0x00;  // schema id
    add_seed("sbe_order_entry", broken);
    broken = frames[13];
    broken[14] = 0x01;  // numGroups
    add_seed("sbe_order_entry", broken);
    add_seed("sbe_order_entry", frames[3].substr(0, frames[3].size() - 3));  // truncated

    // Variable length field at the limit (accepted) and one byte over it (VariableLengthFieldTooLong).
    const std::string longest_reason(sbe::kMaxVariableLengthFieldBytes, 'r');
    const std::string longest = sbe_frame(sbe::Terminate{uuid, sbe::TerminationCode::Finished, longest_reason});
    add_seed("sbe_order_entry", longest);
    std::string too_long = longest + "r";
    const std::size_t length_offset = sbe::kFramePrefixSize + sbe::Terminate::kBlockLength;
    too_long[length_offset] = static_cast<char>(sbe::kMaxVariableLengthFieldBytes + 1);  // little-endian low byte
    too_long.replace(0, 4, big_endian(too_long.size(), 4));
    add_seed("sbe_order_entry", too_long);

    // Version 2 NewOrderSingle padded to the largest accepted frame, and one byte past it (FrameLengthTooLarge).
    for (const std::size_t frame_length : {sbe::kMaxAcceptedFrameSize, sbe::kMaxAcceptedFrameSize + 1}) {
        std::string padded = frames[13];
        const std::size_t block_length = frame_length - sbe::kFramePrefixSize;
        padded.resize(frame_length, '\0');
        padded[6] = static_cast<char>(block_length & 0xff);  // blockLength, little-endian
        padded[7] = static_cast<char>(block_length >> 8);
        padded[12] = 2;  // version
        padded.replace(0, 4, big_endian(frame_length, 4));
        add_seed("sbe_order_entry", padded);
    }
    // blockLength one byte longer than the frame holds (BlockExceedsFrame).
    broken = frames[13];
    broken[6] = static_cast<char>(sbe::NewOrderSingle::kBlockLength + 1);
    add_seed("sbe_order_entry", broken);
}

void add_text_seeds(const std::filesystem::path& repository_root) {
    // Stream buffers: capacities, then opcodes with arguments.
    add_seed("stream_buffers", bytes_of({16, 16, 0, 0, 0, 0, 8}) + "hello123" + bytes_of({1, 0, 0, 0, 3}));
    add_seed("stream_buffers", bytes_of({32, 8, 3, 0, 0, 0, 6, 4, 0, 0, 0, 5}) + "abcde" + bytes_of({8}));
    add_seed("stream_buffers",
             bytes_of({8, 8, 6, 0, 0, 0, 10}) + "ABCDEFGHIJ" + bytes_of({7, 7, 9, 0, 7, 8}));

    for (const char* line :
         {"help", "status", "shutdown", "phase AAPL continuous", "phase * halted", "book AAPL", "book MSFT 10",
          "fault md drop 5", "fault md dup 2", "fault md reorder 3 2", "fault md rate drop=1000 dup=10 seed=7",
          "fault md off", "fault user ALICE reject 1", "fault user BOB delay 2500", "fault user BOB drop_out 4",
          "fault user BOB disconnect", "fault user BOB off", "bots on", "BOTS OFF", "frobnicate"}) {
        add_seed("admin_command", line);
    }

    std::ifstream example_config(repository_root / "config" / "exsim.conf", std::ios::binary);
    add_seed("server_config", {std::istreambuf_iterator<char>(example_config), {}});
    add_seed("server_config",
             "[engine]\nmax_instruments = 2\nmax_orders = 1000\n[instrument AAPL]\ntick_size = 0.01\n"
             "min_price = 1.00\nmax_price = 1000\nreference_price = 150.25\n[user ALICE]\nsession_id = 1\n"
             "password = secret\n[user FIXCLI]\nprotocol = fix44\nsession_id = 2\nstp = cancel_both\n");

    for (const char* text : {"150.25", "-0.0001", "0", "922337203685477.5807", "-922337203685477.5808", ".5", "5.",
                             "+1.25", "12.34567", "1e5"}) {
        add_seed("price_text", text);
    }

    add_seed("bot_options", bytes_of({0}) + "name=mm1\nsession_id=7\ninstrument=1\nlevels=5\nsize=200\nspread_ticks=2\n"
                            "requote_ms=50\ncancel_on_halt=false\norder_capacity=256\nstp=cancel_both");
    add_seed("bot_options", bytes_of({1}) + std::string("name=nt1\nsession_id=8\ninstrument=1\nmean_interval_ms=20\n"
                                        "min_size=1\nmax_size=50\nlimit_ppm=250000\nfair_value_ticks=15000\nenabled=0"));
    add_seed("bot_options", bytes_of({0}) + "levels=0\nlevels=65\nsize=-1\nunknown=1");
}

void add_simulation_seeds(const std::filesystem::path& repository_root) {
    // Journal: a real file from JournalWriter, as-is (mode byte 0) and with its
    // integrity fields stripped (mode byte 1, rebuilt by the target).
    exsim::sim::JournalSetup setup{{4, 1024}, {}};
    exsim::InstrumentConfig apple;
    apple.symbol = exsim::Symbol("AAPL");
    exsim::InstrumentConfig microsoft;
    microsoft.symbol = exsim::Symbol("MSFT");
    microsoft.initial_phase = exsim::Phase::PreOpen;
    setup.instruments = {apple, microsoft};
    const auto journal_path =
        std::filesystem::temp_directory_path() / ("exsim-seed-journal-" + std::to_string(::getpid()) + ".bin");
    {
        exsim::sim::JournalWriter writer(journal_path.string(), setup);
        exsim::NewOrder order;
        order.ts = 1;
        order.instrument = 1;
        order.session = 1;
        order.cl_ord_id = 1;
        order.price = 1'000'000;
        order.qty = 10;
        exsim::CancelOrder cancel;
        cancel.ts = 2;
        cancel.session = 1;
        cancel.orig_cl_ord_id = 1;
        exsim::SetPhase phase;
        phase.ts = 3;
        phase.instrument = 2;
        phase.phase = exsim::Phase::Continuous;
        if (!writer.append(exsim::Command{order}) || !writer.append(exsim::Command{cancel}) ||
            !writer.append(exsim::Command{phase}) || !writer.flush()) {
            std::fprintf(stderr, "journal seed: write failed\n");
        }
    }
    std::ifstream journal_file(journal_path, std::ios::binary);
    const std::string journal{std::istreambuf_iterator<char>(journal_file), {}};
    std::filesystem::remove(journal_path);
    add_seed("journal_reader", std::string(1, '\0') + journal);
    const std::size_t header_end = 16 + 24 + setup.instruments.size() * 64;
    std::string stripped = journal.substr(16, header_end - 16);
    for (std::size_t record = header_end + 8; record + 280 <= journal.size() - 24; record += 280) {
        stripped += journal.substr(record + 8, 264);
    }
    add_seed("journal_reader", std::string(1, '\1') + stripped);
    add_seed("journal_reader", std::string(1, '\0') + journal.substr(0, journal.size() / 2));

    std::vector<std::filesystem::path> scenario_files;
    for (const auto& entry : std::filesystem::directory_iterator(repository_root / "tests" / "scenarios")) {
        if (entry.path().extension() == ".scn") scenario_files.push_back(entry.path());
    }
    std::sort(scenario_files.begin(), scenario_files.end());
    for (const auto& path : scenario_files) {
        std::ifstream scenario(path, std::ios::binary);
        add_seed("scenario_dsl", {std::istreambuf_iterator<char>(scenario), {}});
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <repository-root> <output-directory>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path repository_root = argv[1];
    const std::filesystem::path output_directory = argv[2];
    add_protocol_seeds();
    add_sbe_seeds();
    add_text_seeds(repository_root);
    add_simulation_seeds(repository_root);
    for (const auto& [target, seeds] : seeds_by_target) {
        std::filesystem::create_directories(output_directory / target);
        for (std::size_t index = 0; index < seeds.size(); ++index) {
            char name[32];
            std::snprintf(name, sizeof name, "seed-%03zu", index);
            std::ofstream out(output_directory / target / name, std::ios::binary | std::ios::trunc);
            out.write(seeds[index].data(), static_cast<std::streamsize>(seeds[index].size()));
        }
        std::printf("%-18s %zu seeds\n", target.c_str(), seeds.size());
    }
    return 0;
}
