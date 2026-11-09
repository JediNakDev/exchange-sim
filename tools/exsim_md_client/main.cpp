// exsim_md_client: listens to the exsim ITCH 5.0 / MoldUDP64 market-data
// feed, recovers gaps from the retransmission server and prints either every
// message or the rebuilt top of book.
//
//   exsim_md_client --port 31001 [--group 239.1.1.1] [--interface 127.0.0.1]
//                   [--bind 0.0.0.0] [--session EXSIM]
//                   [--retransmit 127.0.0.1:31002] [--mode messages|book]
//                   [--levels 5] [--late-join] [--count N] [--timeout-ms N]
//
// Retransmission-socket datagrams are accepted only from the exact
// --retransmit endpoint (the server must answer from that address); others
// are ignored and counted. Without --retransmit, gap requests have nowhere to go: missing messages are
// reported as lost after the retry limit and delivery continues (a book is
// then stale). Exit status 0 after End of Session, --count messages or
// --timeout-ms of silence; 1 on bad arguments or socket errors.

#include <md/book_builder.hpp>
#include <md/itch_dispatch.hpp>
#include <md/receiver.hpp>
#include <net/clocks.hpp>
#include <net/poller.hpp>
#include <net/udp.hpp>

#include "retransmission_source.hpp"

#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using namespace exsim;
namespace itch = exsim::protocol::itch50;

enum class OutputMode { Messages, Book };

struct ClientOptions {
    std::uint16_t feed_port = 0;
    std::uint32_t multicast_group = 0;  // 0: plain unicast listener
    std::uint32_t multicast_interface = net::kIpv4AnyAddress;
    std::uint32_t bind_address = net::kIpv4AnyAddress;
    std::string_view session = {};
    std::optional<net::Ipv4Endpoint> retransmission_server;
    OutputMode mode = OutputMode::Messages;
    std::size_t book_levels = 5;
    bool late_join = false;
    std::uint64_t message_limit = 0;   // 0: unlimited
    std::uint64_t silence_timeout_ms = 0;  // 0: never
};

void print_usage() {
    std::fprintf(stderr,
                 "usage: exsim_md_client --port PORT [--group ADDR] [--interface ADDR] [--bind ADDR]\n"
                 "                       [--session NAME] [--retransmit HOST:PORT] [--mode messages|book]\n"
                 "                       [--levels N] [--late-join] [--count N] [--timeout-ms N]\n");
}

bool parse_unsigned(std::string_view text, std::uint64_t maximum, std::uint64_t& value_out) {
    if (text.empty() || text.size() > 20) return false;
    std::uint64_t value = 0;
    for (char digit : text) {
        if (digit < '0' || digit > '9') return false;
        const std::uint64_t next = value * 10 + static_cast<std::uint64_t>(digit - '0');
        if (next / 10 != value || next > maximum) return false;
        value = next;
    }
    value_out = value;
    return true;
}

bool parse_endpoint(std::string_view text, net::Ipv4Endpoint& endpoint_out) {
    const std::size_t colon = text.rfind(':');
    std::uint64_t port = 0;
    if (colon == std::string_view::npos || !parse_unsigned(text.substr(colon + 1), 65535, port) || port == 0)
        return false;
    if (!net::parse_ipv4_address(text.substr(0, colon), endpoint_out.address)) return false;
    endpoint_out.port = static_cast<std::uint16_t>(port);
    return true;
}

bool parse_arguments(int argc, char** argv, ClientOptions& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view flag = argv[index];
        if (flag == "--late-join") {
            options.late_join = true;
            continue;
        }
        if (index + 1 >= argc) return false;
        const std::string_view value = argv[++index];
        std::uint64_t number = 0;
        if (flag == "--port") {
            if (!parse_unsigned(value, 65535, number) || number == 0) return false;
            options.feed_port = static_cast<std::uint16_t>(number);
        } else if (flag == "--group") {
            if (!net::parse_ipv4_address(value, options.multicast_group) ||
                !net::is_ipv4_multicast_address(options.multicast_group))
                return false;
        } else if (flag == "--interface") {
            if (!net::parse_ipv4_address(value, options.multicast_interface)) return false;
        } else if (flag == "--bind") {
            if (!net::parse_ipv4_address(value, options.bind_address)) return false;
        } else if (flag == "--session") {
            if (value.size() > 10) return false;
            options.session = value;
        } else if (flag == "--retransmit") {
            net::Ipv4Endpoint endpoint{};
            if (!parse_endpoint(value, endpoint)) return false;
            options.retransmission_server = endpoint;
        } else if (flag == "--mode") {
            if (value == "messages") {
                options.mode = OutputMode::Messages;
            } else if (value == "book") {
                options.mode = OutputMode::Book;
            } else {
                return false;
            }
        } else if (flag == "--levels") {
            if (!parse_unsigned(value, 1000, number) || number == 0) return false;
            options.book_levels = static_cast<std::size_t>(number);
        } else if (flag == "--count") {
            if (!parse_unsigned(value, UINT64_MAX, number)) return false;
            options.message_limit = number;
        } else if (flag == "--timeout-ms") {
            if (!parse_unsigned(value, UINT64_MAX / 1'000'000, number)) return false;
            options.silence_timeout_ms = number;
        } else {
            return false;
        }
    }
    return options.feed_port != 0;
}

// ITCH Price(4): four implied decimals.
void print_price(Price price) {
    std::printf("%" PRId64 ".%04" PRId64, price / 10'000, price % 10'000);
}

void print_symbol(const Symbol& symbol) {
    const std::string_view text = symbol.view();
    std::printf("%.*s", static_cast<int>(text.size()), text.data());
}

void print_header(std::uint64_t sequence, char type, std::uint16_t locate, std::uint64_t timestamp) {
    std::printf("%" PRIu64 " %c locate=%u ts=%" PRIu64, sequence, type, static_cast<unsigned>(locate), timestamp);
}

char side_code(Side side) { return side == Side::BUY ? 'B' : 'S'; }

// One line per ITCH message.
struct MessagePrinter {
    void on_itch(std::uint64_t sequence, const itch::SystemEvent& message) {
        print_header(sequence, 'S', message.stock_locate, message.timestamp);
        std::printf(" event=%c\n", message.event);
    }
    void on_itch(std::uint64_t sequence, const itch::StockDirectory& message) {
        print_header(sequence, 'R', message.stock_locate, message.timestamp);
        std::printf(" stock=");
        print_symbol(message.stock);
        std::printf(" round_lot=%u\n", message.round_lot_size);
    }
    void on_itch(std::uint64_t sequence, const itch::StockTradingAction& message) {
        print_header(sequence, 'H', message.stock_locate, message.timestamp);
        std::printf(" stock=");
        print_symbol(message.stock);
        std::printf(" state=%c\n", message.trading_state);
    }
    void on_itch(std::uint64_t sequence, const itch::AddOrder& message) {
        print_header(sequence, 'A', message.stock_locate, message.timestamp);
        std::printf(" ref=%" PRIu64 " side=%c shares=%" PRIu64 " price=", message.order_reference,
                    side_code(message.side), static_cast<std::uint64_t>(message.shares));
        print_price(message.price);
        std::printf("\n");
    }
    void on_itch(std::uint64_t sequence, const itch::AddOrderMPID& message) {
        print_header(sequence, 'F', message.stock_locate, message.timestamp);
        std::printf(" ref=%" PRIu64 " side=%c shares=%" PRIu64 " price=", message.order_reference,
                    side_code(message.side), static_cast<std::uint64_t>(message.shares));
        print_price(message.price);
        std::printf(" mpid=%.4s\n", message.attribution.data());
    }
    void on_itch(std::uint64_t sequence, const itch::OrderExecuted& message) {
        print_header(sequence, 'E', message.stock_locate, message.timestamp);
        std::printf(" ref=%" PRIu64 " shares=%" PRIu64 " match=%" PRIu64 "\n", message.order_reference,
                    static_cast<std::uint64_t>(message.shares), message.match_number);
    }
    void on_itch(std::uint64_t sequence, const itch::OrderExecutedWithPrice& message) {
        print_header(sequence, 'C', message.stock_locate, message.timestamp);
        std::printf(" ref=%" PRIu64 " shares=%" PRIu64 " match=%" PRIu64 " printable=%c price=",
                    message.order_reference, static_cast<std::uint64_t>(message.shares), message.match_number,
                    message.printable);
        print_price(message.price);
        std::printf("\n");
    }
    void on_itch(std::uint64_t sequence, const itch::OrderCancel& message) {
        print_header(sequence, 'X', message.stock_locate, message.timestamp);
        std::printf(" ref=%" PRIu64 " shares=%" PRIu64 "\n", message.order_reference,
                    static_cast<std::uint64_t>(message.shares));
    }
    void on_itch(std::uint64_t sequence, const itch::OrderDelete& message) {
        print_header(sequence, 'D', message.stock_locate, message.timestamp);
        std::printf(" ref=%" PRIu64 "\n", message.order_reference);
    }
    void on_itch(std::uint64_t sequence, const itch::OrderReplace& message) {
        print_header(sequence, 'U', message.stock_locate, message.timestamp);
        std::printf(" ref=%" PRIu64 " new_ref=%" PRIu64 " shares=%" PRIu64 " price=", message.original_reference,
                    message.new_reference, static_cast<std::uint64_t>(message.shares));
        print_price(message.price);
        std::printf("\n");
    }
    void on_itch(std::uint64_t sequence, const itch::Trade& message) {
        print_header(sequence, 'P', message.stock_locate, message.timestamp);
        std::printf(" shares=%" PRIu64 " match=%" PRIu64 " price=", static_cast<std::uint64_t>(message.shares),
                    message.match_number);
        print_price(message.price);
        std::printf("\n");
    }
    void on_itch(std::uint64_t sequence, const itch::CrossTrade& message) {
        print_header(sequence, 'Q', message.stock_locate, message.timestamp);
        std::printf(" stock=");
        print_symbol(message.stock);
        std::printf(" shares=%" PRIu64 " match=%" PRIu64 " cross=%c price=", message.shares, message.match_number,
                    message.cross_type);
        print_price(message.price);
        std::printf("\n");
    }
    void on_itch(std::uint64_t sequence, const itch::NetOrderImbalance& message) {
        print_header(sequence, 'I', message.stock_locate, message.timestamp);
        std::printf(" stock=");
        print_symbol(message.stock);
        std::printf(" paired=%" PRIu64 " imbalance=%" PRIu64 " direction=%c cross=%c near=", message.paired_shares,
                    message.imbalance_shares, message.imbalance_direction, message.cross_type);
        print_price(message.near_price);
        std::printf(" reference=");
        print_price(message.current_reference_price);
        std::printf("\n");
    }
    void on_unknown_itch(std::uint64_t sequence, std::span<const char> message) {
        std::printf("%" PRIu64 " unknown type 0x%02x length=%zu\n", sequence,
                    static_cast<unsigned>(static_cast<unsigned char>(message[0])), message.size());
    }
};

// Receiver handler for both output modes.
class FeedClientHandler {
public:
    FeedClientHandler(OutputMode mode, std::size_t book_levels)
        : mode_(mode),
          builder_(md::BookBuilderConfig{}),
          level_buffer_(book_levels),
          last_top_(builder_.max_instruments() + 1) {}

    void on_message(std::uint64_t sequence, std::span<const char> message) {
        ++delivered_messages_;
        if (mode_ == OutputMode::Messages) {
            if (md::dispatch_itch_message(sequence, message, printer_) != protocol::Status::ok)
                std::printf("%" PRIu64 " undecodable message (%zu bytes)\n", sequence, message.size());
            return;
        }
        const md::BookUpdateStatus status = builder_.apply(message);
        if (status != md::BookUpdateStatus::Applied && status != md::BookUpdateStatus::UnsupportedType)
            std::printf("%" PRIu64 " book update rejected (status %u)\n", sequence, static_cast<unsigned>(status));
        if (message.size() >= 3) print_top_if_changed(static_cast<InstrumentId>(
                                     (static_cast<unsigned char>(message[1]) << 8) | static_cast<unsigned char>(message[2])));
    }

    void on_messages_lost(std::uint64_t first_sequence, std::uint64_t count) {
        std::printf("LOST %" PRIu64 " messages from %" PRIu64 "%s\n", count, first_sequence,
                    mode_ == OutputMode::Book ? " (book is stale)" : "");
    }

    void on_end_of_session(std::uint64_t next_sequence) {
        std::printf("END OF SESSION at sequence %" PRIu64 "\n", next_sequence);
        if (mode_ == OutputMode::Book) print_all_depth();
    }

    void print_all_depth() {
        for (std::size_t locate = 1; locate <= builder_.max_instruments(); ++locate) {
            const auto id = static_cast<InstrumentId>(locate);
            const Symbol* symbol = builder_.symbol(id);
            if (symbol == nullptr && builder_.level_count(id, Side::BUY) == 0 &&
                builder_.level_count(id, Side::SELL) == 0)
                continue;
            std::printf("depth locate=%u ", static_cast<unsigned>(locate));
            if (symbol != nullptr) print_symbol(*symbol);
            std::printf(" state=%c\n", builder_.trading_state(id));
            for (Side side : {Side::BUY, Side::SELL}) {
                const std::size_t count = builder_.depth(id, side, level_buffer_.data(), level_buffer_.size());
                for (std::size_t level = 0; level < count; ++level) {
                    std::printf("  %s %" PRIu64 " @ ", side == Side::BUY ? "bid" : "ask", level_buffer_[level].qty);
                    print_price(level_buffer_[level].price);
                    std::printf(" (%u orders)\n", level_buffer_[level].order_count);
                }
            }
        }
    }

    [[nodiscard]] std::uint64_t delivered_messages() const noexcept { return delivered_messages_; }

private:
    struct TopOfBook {
        Price bid_price = 0;
        std::uint64_t bid_qty = 0;
        Price ask_price = 0;
        std::uint64_t ask_qty = 0;
        bool operator==(const TopOfBook&) const = default;
    };

    void print_top_if_changed(InstrumentId id) {
        if (id == 0 || id > builder_.max_instruments()) return;
        TopOfBook top;
        LevelInfo best{};
        if (builder_.depth(id, Side::BUY, &best, 1) == 1) {
            top.bid_price = best.price;
            top.bid_qty = best.qty;
        }
        if (builder_.depth(id, Side::SELL, &best, 1) == 1) {
            top.ask_price = best.price;
            top.ask_qty = best.qty;
        }
        if (top == last_top_[id]) return;
        last_top_[id] = top;
        const Symbol* symbol = builder_.symbol(id);
        if (symbol != nullptr) {
            print_symbol(*symbol);
        } else {
            std::printf("locate=%u", static_cast<unsigned>(id));
        }
        std::printf(" [%c] bid ", builder_.trading_state(id));
        if (top.bid_qty != 0) {
            std::printf("%" PRIu64 " @ ", top.bid_qty);
            print_price(top.bid_price);
        } else {
            std::printf("-");
        }
        std::printf("  ask ");
        if (top.ask_qty != 0) {
            std::printf("%" PRIu64 " @ ", top.ask_qty);
            print_price(top.ask_price);
        } else {
            std::printf("-");
        }
        std::printf("\n");
    }

    OutputMode mode_;
    MessagePrinter printer_;
    md::ItchBookBuilder builder_;
    std::vector<LevelInfo> level_buffer_;
    std::vector<TopOfBook> last_top_;
    std::uint64_t delivered_messages_ = 0;
};

struct RetransmissionRequestSink {
    net::UdpReceiver* socket;
    std::optional<net::Ipv4Endpoint> server;
    void operator()(std::span<const char> request) const {
        if (!server) return;
        if (socket->send_to(request, *server) < 0) std::fprintf(stderr, "retransmission request failed: errno %d\n", errno);
    }
};

}  // namespace

int main(int argc, char** argv) {
    ClientOptions options;
    if (!parse_arguments(argc, argv, options)) {
        print_usage();
        return 1;
    }

    net::UdpReceiver feed_socket;
    const bool multicast = options.multicast_group != 0;
    int error = feed_socket.open({options.bind_address, options.feed_port}, multicast);
    if (error == 0 && multicast) error = feed_socket.join_multicast(options.multicast_group, options.multicast_interface);
    if (error != 0) {
        std::fprintf(stderr, "feed socket: %s\n", std::strerror(error));
        return 1;
    }
    net::UdpReceiver retransmission_socket;
    error = retransmission_socket.open({net::kIpv4AnyAddress, 0});
    if (error != 0) {
        std::fprintf(stderr, "retransmission socket: %s\n", std::strerror(error));
        return 1;
    }
    net::Poller poller;
    error = poller.open();
    if (error == 0) error = poller.add(feed_socket.fd(), 1, net::PollInterest::Read);
    if (error == 0) error = poller.add(retransmission_socket.fd(), 2, net::PollInterest::Read);
    if (error != 0) {
        std::fprintf(stderr, "poller: %s\n", std::strerror(error));
        return 1;
    }

    FeedClientHandler handler(options.mode, options.book_levels);
    md::ReceiverConfig receiver_config;
    receiver_config.session = options.session;
    receiver_config.first_sequence = options.late_join ? 0 : 1;
    md::MoldUdp64Receiver<FeedClientHandler, RetransmissionRequestSink> receiver(
        receiver_config, handler, RetransmissionRequestSink{&retransmission_socket, options.retransmission_server});

    std::array<char, 65536> datagram{};
    std::uint64_t last_packet_ns = net::monotonic_ns();
    std::uint64_t foreign_retransmission_datagrams_ignored = 0;
    for (;;) {
        if (poller.wait(10) < 0 && errno != EINTR) {
            std::fprintf(stderr, "poll: %s\n", std::strerror(errno));
            return 1;
        }
        for (net::UdpReceiver* socket : {&feed_socket, &retransmission_socket}) {
            for (;;) {
                net::Ipv4Endpoint peer{};
                const ssize_t length = socket->receive_from(datagram, peer);
                if (length < 0) {
                    if (!net::is_would_block(errno) && errno != EMSGSIZE)
                        std::fprintf(stderr, "receive: %s\n", std::strerror(errno));
                    if (errno != EMSGSIZE) break;
                    continue;
                }
                if (socket == &retransmission_socket &&
                    !tools::md_client::is_configured_retransmission_source(options.retransmission_server, peer)) {
                    if (foreign_retransmission_datagrams_ignored++ == 0)
                        std::fprintf(stderr, "ignoring retransmission-socket datagrams not from --retransmit\n");
                    continue;
                }
                last_packet_ns = net::monotonic_ns();
                const md::PacketStatus status = receiver.on_packet(
                    std::span<const char>(datagram.data(), static_cast<std::size_t>(length)), last_packet_ns);
                if (status == md::PacketStatus::Malformed) std::fprintf(stderr, "malformed packet ignored\n");
            }
        }
        const std::uint64_t now = net::monotonic_ns();
        receiver.poll(now);
        std::fflush(stdout);
        if (receiver.end_of_session_reached()) break;
        if (options.message_limit != 0 && handler.delivered_messages() >= options.message_limit) break;
        if (options.silence_timeout_ms != 0 && now - last_packet_ns >= options.silence_timeout_ms * 1'000'000) {
            std::fprintf(stderr, "no packets for %" PRIu64 " ms, exiting\n", options.silence_timeout_ms);
            break;
        }
    }
    if (options.mode == OutputMode::Book && !receiver.end_of_session_reached()) handler.print_all_depth();
    const md::ReceiverStats& stats = receiver.stats();
    std::fprintf(stderr,
                 "packets=%" PRIu64 " delivered=%" PRIu64 " duplicates=%" PRIu64 " requests=%" PRIu64
                 " lost=%" PRIu64 " foreign_retransmissions_ignored=%" PRIu64 "\n",
                 stats.packets, stats.messages_delivered, stats.duplicate_messages, stats.requests_sent,
                 stats.messages_lost, foreign_retransmission_datagrams_ignored);
    return 0;
}
