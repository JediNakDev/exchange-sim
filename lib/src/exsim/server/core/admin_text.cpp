#include "exsim/server/core/admin_text.hpp"

#include "exsim/server/config.hpp"

#include <algorithm>
#include <vector>

namespace exsim::server {

void append_line(std::string& reply, std::string_view line) {
    reply.append(line);
    reply.push_back('\n');
}

void append_ok(std::string& reply) { append_line(reply, "OK"); }

void append_error(std::string& reply, std::string_view message) {
    reply.append("ERR ");
    append_line(reply, message);
}

namespace {

void append_level(std::string& reply, std::string_view side, const LevelInfo& level) {
    reply.append(side);
    reply.push_back(' ');
    reply.append(format_price(level.price));
    reply.append(" qty=");
    reply.append(std::to_string(level.qty));
    reply.append(" orders=");
    reply.append(std::to_string(level.order_count));
    reply.push_back('\n');
}

}  // namespace

void append_book(std::string& reply, const MatchingEngine& engine, InstrumentId instrument, std::size_t levels) {
    const InstrumentConfig* config = engine.instrument(instrument);
    if (config == nullptr) {
        append_error(reply, "unknown instrument");
        return;
    }
    levels = std::clamp<std::size_t>(levels, 1, kMaxAdminBookLevels);  // 0 would print a misleading "(empty)"
    std::vector<LevelInfo> asks(levels);
    std::vector<LevelInfo> bids(levels);
    const std::size_t ask_count = engine.depth(instrument, Side::SELL, asks.data(), levels);
    const std::size_t bid_count = engine.depth(instrument, Side::BUY, bids.data(), levels);
    reply.append("book ");
    reply.append(config->symbol.view());
    reply.append(" phase=");
    reply.append(to_string(engine.phase(instrument)));
    reply.push_back('\n');
    if (ask_count == 0 && bid_count == 0) {
        append_line(reply, "(empty)");
    }
    for (std::size_t i = ask_count; i > 0; --i) {
        append_level(reply, "ask", asks[i - 1]);
    }
    for (std::size_t i = 0; i < bid_count; ++i) {
        append_level(reply, "bid", bids[i]);
    }
    append_ok(reply);
}

std::string describe_user_faults(const UserFaultState& faults) {
    return "reject=" + std::to_string(faults.reject_next_new_orders) +
           " delay_us=" + std::to_string(faults.inbound_delay_ns / 1000) +
           " drop_out=" + std::to_string(faults.drop_next_outbound) +
           " disconnect=" + (faults.disconnect_requested ? "requested" : "no");
}

std::string describe_packet_faults(const sim::PacketFaultInjector& injector) {
    const sim::PacketFaultConfig& c = injector.config();
    const sim::PacketFaultStats& s = injector.stats();
    return "drop_ppm=" + std::to_string(c.drop_ppm) + " dup_ppm=" + std::to_string(c.duplicate_ppm) +
           " reorder_ppm=" + std::to_string(c.reorder_ppm) + " delay=" + std::to_string(c.reorder_delay) +
           " seed=" + std::to_string(c.seed) + " held=" + std::to_string(injector.held()) +
           " idle=" + (injector.idle() ? "yes" : "no") + " submitted=" + std::to_string(s.submitted) +
           " delivered=" + std::to_string(s.delivered) + " dropped=" + std::to_string(s.dropped) +
           " duplicated=" + std::to_string(s.duplicated) + " reordered=" + std::to_string(s.reordered);
}

}  // namespace exsim::server
