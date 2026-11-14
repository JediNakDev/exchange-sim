#include "server/config.hpp"

#include "md/feed_common.hpp"
#include "protocol/cme_sbe.hpp"
#include "protocol/ilink3.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace exsim::server {

namespace {

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

template <typename T>
bool parse_uint(std::string_view v, T& out) noexcept {
    std::uint64_t x = 0;
    const auto* end = v.data() + v.size();
    const auto r = std::from_chars(v.data(), end, x);
    if (r.ec != std::errc{} || r.ptr != end || x > std::numeric_limits<T>::max()) {
        return false;
    }
    out = static_cast<T>(x);
    return true;
}

bool parse_bool(std::string_view v, bool& out) {
    const std::string s = lower(v);
    if (s == "true" || s == "yes" || s == "1" || s == "on") {
        out = true;
        return true;
    }
    if (s == "false" || s == "no" || s == "0" || s == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parse_stp(std::string_view v, StpMode& out) {
    const std::string s = lower(v);
    if (s == "none") out = StpMode::None;
    else if (s == "cancel_resting") out = StpMode::CancelResting;
    else if (s == "cancel_aggressor") out = StpMode::CancelAggressor;
    else if (s == "cancel_both") out = StpMode::CancelBoth;
    else return false;
    return true;
}

bool parse_phase(std::string_view v, Phase& out) {
    const std::string s = lower(v);
    if (s == "closed") out = Phase::Closed;
    else if (s == "pre_open") out = Phase::PreOpen;
    else if (s == "continuous") out = Phase::Continuous;
    else if (s == "halted") out = Phase::Halted;
    else if (s == "pre_close") out = Phase::PreClose;
    else return false;
    return true;
}

bool parse_protocol(std::string_view v, Protocol& out) {
    const std::string s = lower(v);
    if (s == "ouch42" || s == "ouch") out = Protocol::Ouch42;
    else if (s == "fix44" || s == "fix") out = Protocol::Fix44;
    else if (s == "ilink3") out = Protocol::Ilink3;
    else return false;
    return true;
}

bool printable_ascii(std::string_view s) noexcept {
    for (char c : s) {
        if (c < 0x21 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

enum class Section { None, Exchange, MarketData, Engine, Journal, Instrument, User };

struct Parser {
    ServerConfig& cfg;
    std::string& error;
    int line_no = 0;
    Section section = Section::None;
    std::set<std::string> market_data_keys{};  // seen in [market_data], checked against the protocol
    bool fail(const std::string& msg) {
        error = std::to_string(line_no) + ": " + msg;
        return false;
    }


    bool bad_value(std::string_view key, std::string_view value) {
        return fail("invalid value '" + std::string(value) + "' for '" + std::string(key) + "'");
    }

    bool unknown_key(std::string_view key) { return fail("unknown key '" + std::string(key) + "'"); }

    bool header(std::string_view h) {
        const std::size_t sp = h.find_first_of(" \t");
        const std::string kind = lower(trim(h.substr(0, sp)));
        const std::string_view arg = sp == std::string_view::npos ? std::string_view{} : trim(h.substr(sp));
        if (kind == "exchange" || kind == "market_data" || kind == "engine" || kind == "journal") {
            if (!arg.empty()) {
                return fail("section [" + kind + "] takes no name");
            }
            section = kind == "exchange"      ? Section::Exchange
                      : kind == "market_data" ? Section::MarketData
                      : kind == "engine"      ? Section::Engine
                                              : Section::Journal;
            return true;
        }
        if (kind == "instrument") {
            if (arg.empty() || arg.size() > 8 || !printable_ascii(arg)) {
                return fail("instrument symbol must be 1-8 printable characters");
            }
            InstrumentConfig ic{};
            ic.symbol = Symbol(arg);
            cfg.instruments.push_back(ic);
            section = Section::Instrument;
            return true;
        }
        if (kind == "user") {
            if (arg.empty() || !printable_ascii(arg)) {
                return fail("user name must be non-empty printable characters");
            }
            UserConfig u{};
            u.username = std::string(arg);
            cfg.users.push_back(u);
            section = Section::User;
            return true;
        }
        return fail("unknown section [" + std::string(h) + "]");
    }

    bool exchange_key(const std::string& k, std::string_view v) {
        ExchangeSection& x = cfg.exchange;
        if (k == "name") { x.name = std::string(v); return !v.empty() || bad_value(k, v); }
        if (k == "bind_address") { x.bind_address = std::string(v); return true; }
        if (k == "ouch_port") return parse_uint(v, x.ouch_port) || bad_value(k, v);
        if (k == "fix_port") return parse_uint(v, x.fix_port) || bad_value(k, v);
        if (k == "ilink_port") return parse_uint(v, x.ilink_port) || bad_value(k, v);
        if (k == "fixp_sbe_port") return fail("exchange.fixp_sbe_port is not supported: the legacy FIXP/SBE gateway was removed");
        if (k == "admin_port") return fail("exchange.admin_port is not supported: there is no public admin listener");
        if (k == "max_connections") return (parse_uint(v, x.max_connections) && x.max_connections > 0) || bad_value(k, v);
        if (k == "heartbeat_interval_ms") return (parse_uint(v, x.heartbeat_interval_ms) && x.heartbeat_interval_ms > 0) || bad_value(k, v);
        if (k == "soup_session") { x.soup_session = std::string(v); return true; }
        if (k == "trading_day_start") return parse_day_time(v, x.trading_day_start_ns) || bad_value(k, v);
        if (k == "trading_day_end") return parse_day_time(v, x.trading_day_end_ns) || bad_value(k, v);
        return unknown_key(k);
    }

    bool market_data_key(const std::string& k, std::string_view v) {
        market_data_keys.insert(k);
        MarketDataSection& m = cfg.market_data;
        if (k == "protocol") {
            if (v == "itch50") m.protocol = MarketDataProtocol::Itch50;
            else if (v == "mdp3") m.protocol = MarketDataProtocol::Mdp3;
            else return bad_value(k, v);
            return true;
        }
        if (k == "channel_id") return (parse_uint(v, m.channel_id) && m.channel_id > 0 && m.channel_id <= INT16_MAX) || bad_value(k, v);
        if (k == "market_segment_id") return (parse_uint(v, m.market_segment_id) && m.market_segment_id > 0 && m.market_segment_id < UINT8_MAX) || bad_value(k, v);
        if (k == "book_depth") return (parse_uint(v, m.book_depth) && m.book_depth > 0 && m.book_depth <= 10) || bad_value(k, v);
        if (k == "snapshot_port") return parse_uint(v, m.snapshot_port) || bad_value(k, v);
        if (k == "instrument_port") return parse_uint(v, m.instrument_port) || bad_value(k, v);
        if (k == "backup_port") return parse_uint(v, m.backup_port) || bad_value(k, v);
        if (k == "snapshot_interval_ms") return (parse_uint(v, m.snapshot_interval_ms) && m.snapshot_interval_ms > 0) || bad_value(k, v);
        if (k == "instrument_interval_ms") return (parse_uint(v, m.instrument_interval_ms) && m.instrument_interval_ms > 0) || bad_value(k, v);
        if (k == "replay_buffer_packets") return (parse_uint(v, m.replay_buffer_packets) && m.replay_buffer_packets > 0) || bad_value(k, v);
        if (k == "replay_username") { m.replay_username = std::string(v); return true; }
        if (k == "replay_password") { m.replay_password = std::string(v); return true; }
        if (k == "address") { m.address = std::string(v); return !v.empty() || bad_value(k, v); }
        if (k == "port") return parse_uint(v, m.port) || bad_value(k, v);
        if (k == "interface") { m.interface_address = std::string(v); return true; }
        if (k == "ttl") return parse_uint(v, m.ttl) || bad_value(k, v);
        if (k == "loopback") return parse_bool(v, m.loopback) || bad_value(k, v);
        if (k == "retransmit_port") return parse_uint(v, m.retransmit_port) || bad_value(k, v);
        if (k == "max_payload") return (parse_uint(v, m.max_payload) && m.max_payload >= md::kMinimumMaxPayload && m.max_payload <= md::kMaximumMaxPayload) || bad_value(k, v);
        if (k == "session") { m.session = std::string(v); return true; }
        if (k == "retransmit_buffer") return (parse_uint(v, m.retransmit_buffer) && m.retransmit_buffer > 0) || bad_value(k, v);
        if (k == "noii_interval_ms") return parse_uint(v, m.noii_interval_ms) || bad_value(k, v);
        return unknown_key(k);
    }

    bool engine_key(const std::string& k, std::string_view v) {
        EngineSection& e = cfg.engine;
        if (k == "max_instruments") return (parse_uint(v, e.engine.max_instruments) && e.engine.max_instruments > 0 &&
            e.engine.max_instruments <= std::numeric_limits<InstrumentId>::max()) || bad_value(k, v);
        if (k == "max_orders") return (parse_uint(v, e.engine.max_orders) && e.engine.max_orders > 0 &&
            e.engine.max_orders <= std::numeric_limits<std::uint32_t>::max()) || bad_value(k, v);
        if (k == "max_tokens_per_session") return (parse_uint(v, e.max_tokens_per_session) && e.max_tokens_per_session > 0) || bad_value(k, v);
        if (k == "store_messages_per_session") return (parse_uint(v, e.store_messages_per_session) && e.store_messages_per_session > 0) || bad_value(k, v);
        if (k == "store_bytes_per_session") return (parse_uint(v, e.store_bytes_per_session) && e.store_bytes_per_session > 0) || bad_value(k, v);
        return unknown_key(k);
    }

    bool journal_key(const std::string& k, std::string_view v) {
        if (k == "path") { cfg.journal_path = std::string(v); return true; }
        if (k == "on_failure") {
            const std::string policy = lower(v);
            if (policy == "shutdown") cfg.journal_failure_policy = JournalFailurePolicy::Shutdown;
            else if (policy == "continue") cfg.journal_failure_policy = JournalFailurePolicy::Continue;
            else return bad_value(k, v);
            return true;
        }
        if (k == "flush_interval_ms") return parse_uint(v, cfg.journal_flush_interval_ms) || bad_value(k, v);
        return unknown_key(k);
    }

    bool instrument_key(const std::string& k, std::string_view v) {
        InstrumentConfig& ic = cfg.instruments.back();
        if (k == "cme_security_id") return (parse_uint(v, ic.cme_security_id) && ic.cme_security_id > 0 && ic.cme_security_id < INT32_MAX) || bad_value(k, v);
        if (k == "cme_contract_size") return (parse_uint(v, ic.cme_contract_size) && ic.cme_contract_size > 0) || bad_value(k, v);
        if (k == "cme_maturity_date") {
            std::uint32_t value = 0;
            if (!parse_uint(v, value) || value < 19700101 || value > 99991231) return bad_value(k, v);
            const std::chrono::year_month_day date{std::chrono::year(static_cast<int>(value / 10000)),
                std::chrono::month(value / 100 % 100), std::chrono::day(value % 100)};
            if (!date.ok()) return bad_value(k, v);
            ic.cme_maturity_date = value; return true;
        }
        if (k == "cme_symbol") {
            if (v.empty() || v.size() > ic.cme_symbol.size() || !printable_ascii(v)) return bad_value(k, v);
            ic.cme_symbol.fill(0);
            std::copy(v.begin(), v.end(), ic.cme_symbol.begin());
            return true;
        }
        if (k == "tick_size") return (parse_price(v, ic.tick_size) && ic.tick_size > 0) || bad_value(k, v);
        if (k == "lot_size") return (parse_uint(v, ic.lot_size) && ic.lot_size > 0) || bad_value(k, v);
        if (k == "max_order_qty") return (parse_uint(v, ic.max_order_qty) && ic.max_order_qty > 0) || bad_value(k, v);
        if (k == "min_price") return parse_price(v, ic.min_price) || bad_value(k, v);
        if (k == "max_price") return parse_price(v, ic.max_price) || bad_value(k, v);
        if (k == "reference_price") return parse_price(v, ic.reference_price) || bad_value(k, v);
        if (k == "initial_phase") return parse_phase(v, ic.initial_phase) || bad_value(k, v);
        return unknown_key(k);
    }

    bool user_key(const std::string& k, std::string_view v) {
        UserConfig& u = cfg.users.back();
        if (k == "password") { u.password = std::string(v); return true; }
        if (k == "protocol") return parse_protocol(v, u.protocol) || bad_value(k, v);
        if (k == "session_id") return (parse_uint(v, u.session_id) && u.session_id != 0) || bad_value(k, v);
        if (k == "owner") return parse_uint(v, u.owner) || bad_value(k, v);
        if (k == "stp") return parse_stp(v, u.stp) || bad_value(k, v);
        if (k == "cancel_on_disconnect") return parse_bool(v, u.cancel_on_disconnect) || bad_value(k, v);
        if (k == "ilink_access_key") { u.ilink_access_key = std::string(v); return true; }
        if (k == "ilink_secret_key") { u.ilink_secret_key = std::string(v); return true; }
        if (k == "ilink_session") { u.ilink_session = std::string(v); return true; }
        if (k == "ilink_firm") { u.ilink_firm = std::string(v); return true; }
        if (k == "ilink_account") { u.ilink_account = std::string(v); return true; }
        if (k == "ilink_party_id") return (parse_uint(v, u.ilink_party_id) && u.ilink_party_id > 0 && u.ilink_party_id < UINT64_MAX) || bad_value(k, v);
        return unknown_key(k);
    }

    bool key_value(std::string_view kv) {
        const std::size_t eq = kv.find('=');
        if (eq == std::string_view::npos) {
            return fail("expected 'key = value'");
        }
        const std::string key = lower(trim(kv.substr(0, eq)));
        const std::string_view value = trim(kv.substr(eq + 1));
        if (key.empty()) {
            return fail("empty key");
        }
        switch (section) {
            case Section::None: return fail("key '" + key + "' outside of any section");
            case Section::Exchange: return exchange_key(key, value);
            case Section::MarketData: return market_data_key(key, value);
            case Section::Engine: return engine_key(key, value);
            case Section::Journal: return journal_key(key, value);
            case Section::Instrument: return instrument_key(key, value);
            case Section::User: return user_key(key, value);
        }
        return fail("internal error");
    }

    bool validate() {
        line_no = 0;
        const bool mdp = cfg.market_data.protocol == MarketDataProtocol::Mdp3;
        for (const auto& key : market_data_keys) {
            const bool itch_only = key == "retransmit_buffer" || key == "noii_interval_ms" || key == "session";
            const bool mdp_only = key == "channel_id" || key == "book_depth" ||
                key == "snapshot_port" || key == "instrument_port" || key == "backup_port" ||
                key == "snapshot_interval_ms" || key == "instrument_interval_ms" ||
                key == "replay_buffer_packets" || key == "replay_username" || key == "replay_password";
            if ((mdp && itch_only) || (!mdp && mdp_only)) return fail("market_data." + key + " is ignored by the selected protocol");
        }
        if (cfg.exchange.soup_session.size() > 10 || !printable_ascii(cfg.exchange.soup_session)) {
            return fail("exchange.soup_session must be at most 10 printable characters");
        }
        if (cfg.market_data.session.size() > 10 || !printable_ascii(cfg.market_data.session)) {
            return fail("market_data.session must be at most 10 printable characters");
        }
        if (cfg.instruments.empty()) {
            return fail("no [instrument] sections");
        }
        if (cfg.instruments.size() > cfg.engine.engine.max_instruments) {
            return fail("more instruments than engine.max_instruments");
        }
        std::set<std::string> symbols;
        const bool cme_profile = cfg.market_data.protocol == MarketDataProtocol::Mdp3 ||
            std::any_of(cfg.users.begin(), cfg.users.end(), [](const auto& user) { return user.protocol == Protocol::Ilink3; });
        std::set<std::int32_t> security_ids;
        std::set<std::string> cme_symbols;
        for (const InstrumentConfig& ic : cfg.instruments) {
            const std::string sym(ic.symbol.view());
            if (!symbols.insert(sym).second) {
                return fail("duplicate instrument " + sym);
            }
            if (ic.min_price <= 0 || ic.min_price > ic.max_price) {
                return fail("instrument " + sym + ": need 0 < min_price <= max_price");
            }
            if (ic.min_price % ic.tick_size != 0 || ic.max_price % ic.tick_size != 0) {
                return fail("instrument " + sym + ": price band must be a multiple of tick_size");
            }
            if (ic.reference_price < ic.min_price || ic.reference_price > ic.max_price ||
                ic.reference_price % ic.tick_size != 0) {
                return fail("instrument " + sym + ": reference_price must be an on-tick price inside the band");
            }
            if (ic.max_order_qty < ic.lot_size) {
                return fail("instrument " + sym + ": max_order_qty < lot_size");
            }
            if (cme_profile) {
                if (ic.cme_security_id <= 0 || !security_ids.insert(ic.cme_security_id).second)
                    return fail("MDP requires a unique positive cme_security_id per instrument");
                const auto end = std::find(ic.cme_symbol.begin(), ic.cme_symbol.end(), char{0});
                const std::string external = end == ic.cme_symbol.begin() ? sym : std::string(ic.cme_symbol.begin(), end);
                if (!cme_symbols.insert(external).second) return fail("duplicate cme_symbol " + external);
                std::int64_t encoded = 0;
                if (!protocol::cme::price_to_price9(ic.max_price, encoded) || ic.max_order_qty >= INT32_MAX || ic.lot_size > INT32_MAX / 10000u)
                    return fail("MDP instrument price or quantity exceeds wire range");
            }
        }
        if (cfg.market_data.protocol == MarketDataProtocol::Mdp3) {
            const auto& m = cfg.market_data;
            if (!m.snapshot_port || !m.instrument_port || !m.port || m.max_payload < 1400 ||
                m.port == m.snapshot_port || m.port == m.instrument_port || m.snapshot_port == m.instrument_port ||
                (m.backup_port && (m.backup_port == m.port || m.backup_port == m.snapshot_port || m.backup_port == m.instrument_port)))
                return fail("MDP requires distinct feed/snapshot/instrument destinations and max_payload >= 1400");
            if (m.replay_username.empty() || m.replay_password.empty() || m.replay_username.size() > 128 || m.replay_password.size() > 128 ||
                !printable_ascii(m.replay_username) || !printable_ascii(m.replay_password))
                return fail("MDP requires printable replay_username and replay_password");
        }
        std::set<std::string> names;
        std::set<SessionId> sessions;
        std::set<std::string> ilink_sessions;
        for (const UserConfig& u : cfg.users) {
            if (!names.insert(u.username).second) {
                return fail("duplicate user " + u.username);
            }
            if (u.session_id == 0) {
                return fail("user " + u.username + ": session_id is required and must be non-zero");
            }
            if (!sessions.insert(u.session_id).second) {
                return fail("user " + u.username + ": duplicate session_id " + std::to_string(u.session_id));
            }
            if (u.protocol == Protocol::Ouch42 && (u.username.size() > 6 || u.password.size() > 10)) {
                return fail("user " + u.username + ": SoupBinTCP limits username to 6 and password to 10 characters");
            }
            if (u.protocol == Protocol::Ilink3) {
                std::array<char, 32> key{};
                if (u.ilink_access_key.empty() || u.ilink_access_key.size() > 20 || !printable_ascii(u.ilink_access_key) ||
                    !protocol::ilink3::decode_secret(u.ilink_secret_key, key) || u.ilink_session.size() != 3 ||
                    !printable_ascii(u.ilink_session) || u.ilink_firm.empty() || u.ilink_firm.size() > 5 || !printable_ascii(u.ilink_firm) ||
                    !u.ilink_party_id || u.ilink_party_id == UINT64_MAX || u.ilink_account.empty() ||
                    u.ilink_account.size() > 20 || !printable_ascii(u.ilink_account))
                    return fail("iLink user requires access key, base64url secret, three-character session, firm, party ID and account");
                if (!ilink_sessions.insert(u.ilink_session + "/" + u.ilink_firm).second)
                    return fail("duplicate iLink session/firm identity");
            }
        }
        if (cfg.exchange.trading_day_start_ns >= cfg.exchange.trading_day_end_ns) {
            return fail("exchange.trading_day_start (" + format_day_time(cfg.exchange.trading_day_start_ns) +
                        ") must be before exchange.trading_day_end (" +
                        format_day_time(cfg.exchange.trading_day_end_ns) +
                        "): trading days that cross midnight are not supported");
        }
        return true;
    }

};

}  // namespace

const UserConfig* ServerConfig::find_user(std::string_view username) const noexcept {
    for (const UserConfig& u : users) {
        if (u.username == username) {
            return &u;
        }
    }
    return nullptr;
}

bool parse_price(std::string_view text, Price& out) noexcept {
    if (text.empty()) {
        return false;
    }
    bool negative = false;
    if (text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    const std::size_t dot = text.find('.');
    const std::string_view whole = text.substr(0, dot);
    const std::string_view frac = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if ((whole.empty() && frac.empty()) || frac.size() > 4 || (dot != std::string_view::npos && frac.empty())) {
        return false;
    }
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<Price>::max());
    const std::uint64_t limit = negative ? maximum + 1 : maximum;
    std::uint64_t value = 0;
    for (char c : whole) {
        if (c < '0' || c > '9') {
            return false;
        }
        if (value > (limit - static_cast<unsigned>(c - '0')) / 10) {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value > limit / kPriceScale) {
        return false;
    }
    value *= kPriceScale;
    std::uint64_t scale = kPriceScale / 10;
    for (char c : frac) {
        if (c < '0' || c > '9') {
            return false;
        }
        const auto fraction_units = static_cast<unsigned>(c - '0') * scale;
        if (value > limit - fraction_units) return false;
        value += fraction_units;
        scale /= 10;
    }
    out = negative && value == maximum + 1 ? std::numeric_limits<Price>::min()
        : negative ? -static_cast<Price>(value) : static_cast<Price>(value);
    return true;
}

bool parse_day_time(std::string_view text, Timestamp& out) noexcept {
    // HH:MM[:SS[.fraction]]
    auto two_digits = [](std::string_view s, std::size_t at, unsigned& value) noexcept {
        if (s.size() < at + 2 || s[at] < '0' || s[at] > '9' || s[at + 1] < '0' || s[at + 1] > '9') {
            return false;
        }
        value = static_cast<unsigned>((s[at] - '0') * 10 + (s[at + 1] - '0'));
        return true;
    };
    unsigned hours = 0, minutes = 0, seconds = 0;
    if (!two_digits(text, 0, hours) || text.size() < 5 || text[2] != ':' || !two_digits(text, 3, minutes)) {
        return false;
    }
    std::size_t at = 5;
    std::uint64_t fraction_ns = 0;
    if (text.size() > at) {
        if (text[at] != ':' || !two_digits(text, at + 1, seconds)) {
            return false;
        }
        at += 3;
        if (text.size() > at) {
            if (text[at] != '.' || text.size() == at + 1 || text.size() > at + 10) {
                return false;
            }
            std::uint64_t scale = 100'000'000;
            for (std::size_t i = at + 1; i < text.size(); ++i) {
                if (text[i] < '0' || text[i] > '9') {
                    return false;
                }
                fraction_ns += static_cast<std::uint64_t>(text[i] - '0') * scale;
                scale /= 10;
            }
        }
    }
    if (minutes > 59 || seconds > 59) {
        return false;
    }
    if (hours > 24 || (hours == 24 && (minutes != 0 || seconds != 0 || fraction_ns != 0))) {
        return false;
    }
    out = ((Timestamp{hours} * 60 + minutes) * 60 + seconds) * 1'000'000'000 + fraction_ns;
    return true;
}

std::string format_day_time(Timestamp ns_since_midnight) {
    const std::uint64_t total_seconds = ns_since_midnight / 1'000'000'000;
    std::uint64_t fraction = ns_since_midnight % 1'000'000'000;
    char text[64];  // room for three 20-digit fields, so no truncation is possible
    std::snprintf(text, sizeof text, "%02llu:%02llu:%02llu", static_cast<unsigned long long>(total_seconds / 3600),
                  static_cast<unsigned long long>(total_seconds / 60 % 60),
                  static_cast<unsigned long long>(total_seconds % 60));
    std::string out(text);
    if (fraction != 0) {
        char digits[10] = {'0', '0', '0', '0', '0', '0', '0', '0', '0', '\0'};
        for (int i = 8; i >= 0; --i) {
            digits[i] = static_cast<char>('0' + fraction % 10);
            fraction /= 10;
        }
        int n = 9;
        while (n > 1 && digits[n - 1] == '0') {
            --n;
        }
        out.push_back('.');
        out.append(digits, static_cast<std::size_t>(n));
    }
    return out;
}

bool parse_config(std::string_view text, ServerConfig& out, std::string& error) {
    ServerConfig cfg{};
    Parser p{cfg, error};
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++p.line_no;
        const std::size_t comment = line.find_first_of("#;");
        if (comment != std::string_view::npos) {
            line = line.substr(0, comment);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[') {
            if (line.back() != ']') {
                return p.fail("unterminated section header");
            }
            if (!p.header(trim(line.substr(1, line.size() - 2)))) {
                return false;
            }
            continue;
        }
        if (!p.key_value(line)) {
            return false;
        }
    }
    if (!p.validate()) {
        return false;
    }
    out = std::move(cfg);
    return true;
}

bool load_config(const char* path, ServerConfig& out, std::string& error) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        error = std::string("cannot open ") + path;
        return false;
    }
    std::string text;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        text.append(buf, n);
    }
    std::fclose(f);
    return parse_config(text, out, error);
}

}  // namespace exsim::server
