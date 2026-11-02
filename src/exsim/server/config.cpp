#include "exsim/server/config.hpp"

#include <charconv>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <system_error>

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
        if (k == "admin_port") return parse_uint(v, x.admin_port) || bad_value(k, v);
        if (k == "max_connections") return (parse_uint(v, x.max_connections) && x.max_connections > 0) || bad_value(k, v);
        if (k == "heartbeat_interval_ms") return (parse_uint(v, x.heartbeat_interval_ms) && x.heartbeat_interval_ms > 0) || bad_value(k, v);
        if (k == "soup_session") { x.soup_session = std::string(v); return true; }
        return unknown_key(k);
    }

    bool market_data_key(const std::string& k, std::string_view v) {
        MarketDataSection& m = cfg.market_data;
        if (k == "address") { m.address = std::string(v); return !v.empty() || bad_value(k, v); }
        if (k == "port") return parse_uint(v, m.port) || bad_value(k, v);
        if (k == "interface") { m.interface_address = std::string(v); return true; }
        if (k == "ttl") return parse_uint(v, m.ttl) || bad_value(k, v);
        if (k == "loopback") return parse_bool(v, m.loopback) || bad_value(k, v);
        if (k == "retransmit_port") return parse_uint(v, m.retransmit_port) || bad_value(k, v);
        if (k == "max_payload") return (parse_uint(v, m.max_payload) && m.max_payload >= 64 && m.max_payload <= 65507) || bad_value(k, v);
        if (k == "session") { m.session = std::string(v); return true; }
        if (k == "retransmit_buffer") return (parse_uint(v, m.retransmit_buffer) && m.retransmit_buffer > 0) || bad_value(k, v);
        if (k == "noii_interval_ms") return parse_uint(v, m.noii_interval_ms) || bad_value(k, v);
        return unknown_key(k);
    }

    bool engine_key(const std::string& k, std::string_view v) {
        EngineSection& e = cfg.engine;
        if (k == "max_instruments") return (parse_uint(v, e.engine.max_instruments) && e.engine.max_instruments > 0) || bad_value(k, v);
        if (k == "max_orders") return (parse_uint(v, e.engine.max_orders) && e.engine.max_orders > 0) || bad_value(k, v);
        if (k == "max_tokens_per_session") return (parse_uint(v, e.max_tokens_per_session) && e.max_tokens_per_session > 0) || bad_value(k, v);
        if (k == "store_messages_per_session") return (parse_uint(v, e.store_messages_per_session) && e.store_messages_per_session > 0) || bad_value(k, v);
        if (k == "store_bytes_per_session") return (parse_uint(v, e.store_bytes_per_session) && e.store_bytes_per_session > 0) || bad_value(k, v);
        return unknown_key(k);
    }

    bool journal_key(const std::string& k, std::string_view v) {
        if (k == "path") { cfg.journal_path = std::string(v); return true; }
        return unknown_key(k);
    }

    bool instrument_key(const std::string& k, std::string_view v) {
        InstrumentConfig& ic = cfg.instruments.back();
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
        }
        std::set<std::string> names;
        std::set<SessionId> sessions;
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
    constexpr Price kMax = std::numeric_limits<Price>::max();
    Price value = 0;
    for (char c : whole) {
        if (c < '0' || c > '9') {
            return false;
        }
        if (value > (kMax - (c - '0')) / 10) {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    if (value > kMax / kPriceScale) {
        return false;
    }
    value *= kPriceScale;
    Price scale = kPriceScale / 10;
    for (char c : frac) {
        if (c < '0' || c > '9') {
            return false;
        }
        value += (c - '0') * scale;
        scale /= 10;
    }
    out = negative ? -value : value;
    return true;
}

std::string format_price(Price p) {
    std::string out;
    std::uint64_t mag = p < 0 ? static_cast<std::uint64_t>(-(p + 1)) + 1 : static_cast<std::uint64_t>(p);
    if (p < 0) {
        out.push_back('-');
    }
    out += std::to_string(mag / kPriceScale);
    std::uint64_t frac = mag % kPriceScale;
    if (frac != 0) {
        char digits[5] = {'0', '0', '0', '0', '\0'};
        for (int i = 3; i >= 0; --i) {
            digits[i] = static_cast<char>('0' + frac % 10);
            frac /= 10;
        }
        int n = 4;
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
