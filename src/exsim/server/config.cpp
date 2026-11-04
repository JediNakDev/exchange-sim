#include "exsim/server/config.hpp"

#include "exsim/md/feed_common.hpp"

#include <algorithm>
#include <charconv>
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
    else if (s == "fixp_sbe" || s == "sbe") out = Protocol::FixpSbe;
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

enum class Section { None, Exchange, MarketData, Engine, Journal, Instrument, User, Bot };

// A [bot] key waiting for the end of its section: bot options depend on
// `type`, which may appear after them.
struct PendingBotKey {
    std::string key;
    std::string value;
    int line_no = 0;
};

struct Parser {
    ServerConfig& cfg;
    std::string& error;
    int line_no = 0;
    Section section = Section::None;
    std::vector<PendingBotKey> pending_bot_keys{};
    std::vector<int> bot_header_lines{};   // per cfg.bots entry
    std::vector<bool> bot_stp_explicit{};  // per cfg.bots entry: stp set in the section

    bool fail(const std::string& msg) {
        error = std::to_string(line_no) + ": " + msg;
        return false;
    }

    bool fail_at(int line, const std::string& msg) {
        line_no = line;
        return fail(msg);
    }

    bool bad_value(std::string_view key, std::string_view value) {
        return fail("invalid value '" + std::string(value) + "' for '" + std::string(key) + "'");
    }

    bool unknown_key(std::string_view key) { return fail("unknown key '" + std::string(key) + "'"); }

    bool header(std::string_view h) {
        if (!finish_bot_section()) {
            return false;
        }
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
        if (kind == "bot") {
            if (arg.empty() || arg.size() > sim::bots::BotName::kCapacity || !printable_ascii(arg)) {
                return fail("bot name must be 1-32 printable characters");
            }
            BotSectionConfig b{};
            b.name = std::string(arg);
            cfg.bots.push_back(std::move(b));
            bot_header_lines.push_back(line_no);
            bot_stp_explicit.push_back(false);
            pending_bot_keys.clear();
            section = Section::Bot;
            return true;
        }
        return fail("unknown section [" + std::string(h) + "]");
    }

    // Applies the collected keys of the current [bot] section (no-op for
    // any other section). Called at every header and at end of input.
    bool finish_bot_section() {
        if (section != Section::Bot) {
            return true;
        }
        section = Section::None;
        BotSectionConfig& bot = cfg.bots.back();
        const int header_line = bot_header_lines.back();
        bool have_type = false;
        for (const PendingBotKey& k : pending_bot_keys) {
            if (k.key != "type") {
                continue;
            }
            const std::string type = lower(k.value);
            if (type == "market_maker") bot.type = BotType::MarketMaker;
            else if (type == "noise_taker") bot.type = BotType::NoiseTaker;
            else return fail_at(k.line_no, "invalid value '" + k.value + "' for 'type' (market_maker|noise_taker)");
            have_type = true;
        }
        if (!have_type) {
            return fail_at(header_line, "bot " + bot.name + ": 'type' is required");
        }
        bot.market_maker.name.assign(bot.name);
        bot.noise_taker.name.assign(bot.name);
        for (const PendingBotKey& k : pending_bot_keys) {
            if (k.key == "type") {
                continue;
            }
            if (k.key == "user") {
                bot.user = k.value;
                continue;
            }
            if (k.key == "instrument") {
                bot.instrument_symbol = k.value;
                continue;
            }
            if (k.key == "session_id" || k.key == "owner") {
                return fail_at(k.line_no, "bot key '" + k.key + "' is taken from the bot's user");
            }
            if (k.key == "name") {
                return fail_at(k.line_no, "bot key 'name' is taken from the section header");
            }
            const bool applied = bot.type == BotType::MarketMaker
                                     ? sim::bots::apply_bot_option(bot.market_maker, k.key, k.value)
                                     : sim::bots::apply_bot_option(bot.noise_taker, k.key, k.value);
            if (!applied) {
                return fail_at(k.line_no, "invalid " + std::string(to_string(bot.type)) + " option '" + k.key +
                                              " = " + k.value + "'");
            }
            if (k.key == "stp") {
                bot_stp_explicit.back() = true;
            }
        }
        if (bot.user.empty()) {
            return fail_at(header_line, "bot " + bot.name + ": 'user' is required");
        }
        if (bot.instrument_symbol.empty()) {
            return fail_at(header_line, "bot " + bot.name + ": 'instrument' is required");
        }
        pending_bot_keys.clear();
        return true;
    }

    bool exchange_key(const std::string& k, std::string_view v) {
        ExchangeSection& x = cfg.exchange;
        if (k == "name") { x.name = std::string(v); return !v.empty() || bad_value(k, v); }
        if (k == "bind_address") { x.bind_address = std::string(v); return true; }
        if (k == "ouch_port") return parse_uint(v, x.ouch_port) || bad_value(k, v);
        if (k == "fix_port") return parse_uint(v, x.fix_port) || bad_value(k, v);
        if (k == "fixp_sbe_port") return parse_uint(v, x.fixp_sbe_port) || bad_value(k, v);
        if (k == "admin_port") return parse_uint(v, x.admin_port) || bad_value(k, v);
        if (k == "max_connections") return (parse_uint(v, x.max_connections) && x.max_connections > 0) || bad_value(k, v);
        if (k == "heartbeat_interval_ms") return (parse_uint(v, x.heartbeat_interval_ms) && x.heartbeat_interval_ms > 0) || bad_value(k, v);
        if (k == "soup_session") { x.soup_session = std::string(v); return true; }
        if (k == "trading_day_start") return parse_day_time(v, x.trading_day_start_ns) || bad_value(k, v);
        if (k == "trading_day_end") return parse_day_time(v, x.trading_day_end_ns) || bad_value(k, v);
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
            case Section::Bot:
                pending_bot_keys.push_back(PendingBotKey{key, std::string(value), line_no});
                return true;
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
        if (cfg.exchange.trading_day_start_ns >= cfg.exchange.trading_day_end_ns) {
            return fail("exchange.trading_day_start must be before exchange.trading_day_end");
        }
        return validate_bots();
    }

    // Resolves user/symbol references and rejects bot configurations that
    // could never submit a valid order (bots/README.md, "Cross-field").
    bool validate_bots() {
        if (cfg.bots.size() > kMaxBots) {
            return fail("more than " + std::to_string(kMaxBots) + " [bot] sections");
        }
        std::set<std::string> bot_names;
        std::set<std::string> bot_users;
        for (std::size_t i = 0; i < cfg.bots.size(); ++i) {
            BotSectionConfig& bot = cfg.bots[i];
            line_no = bot_header_lines[i];
            if (!bot_names.insert(bot.name).second) {
                return fail("duplicate bot " + bot.name);
            }
            const UserConfig* user = cfg.find_user(bot.user);
            if (user == nullptr) {
                return fail("bot " + bot.name + ": unknown user " + bot.user);
            }
            if (!bot_users.insert(bot.user).second) {
                return fail("bot " + bot.name + ": user " + bot.user + " is already driven by another bot");
            }
            std::size_t instrument_index = cfg.instruments.size();
            for (std::size_t j = 0; j < cfg.instruments.size(); ++j) {
                if (cfg.instruments[j].symbol.view() == bot.instrument_symbol) {
                    instrument_index = j;
                }
            }
            if (instrument_index == cfg.instruments.size()) {
                return fail("bot " + bot.name + ": unknown instrument " + bot.instrument_symbol);
            }
            const InstrumentConfig& instrument = cfg.instruments[instrument_index];
            for (sim::bots::BotConfig* identity :
                 {static_cast<sim::bots::BotConfig*>(&bot.market_maker), static_cast<sim::bots::BotConfig*>(&bot.noise_taker)}) {
                identity->session_id = user->session_id;
                identity->owner = user->owner;
                identity->instrument = static_cast<InstrumentId>(instrument_index + 1);
                if (!bot_stp_explicit[i]) {
                    identity->stp = user->stp;
                }
            }
            if (bot.type == BotType::MarketMaker) {
                const sim::bots::MarketMakerConfig& m = bot.market_maker;
                const Quantity quoted = std::min(m.size, instrument.max_order_qty) / instrument.lot_size * instrument.lot_size;
                if (quoted == 0) {
                    return fail("bot " + bot.name + ": size is below the lot_size of " + bot.instrument_symbol);
                }
                if (m.order_capacity < std::size_t{2} * m.levels) {
                    return fail("bot " + bot.name + ": order_capacity must hold 2 * levels orders");
                }
            } else {
                const sim::bots::NoiseTakerConfig& t = bot.noise_taker;
                const std::uint64_t lowest_lots = (std::uint64_t{t.min_size} + instrument.lot_size - 1) / instrument.lot_size;
                const std::uint64_t highest_lots = std::min(t.max_size, instrument.max_order_qty) / instrument.lot_size;
                if (lowest_lots == 0 || lowest_lots > highest_lots) {
                    return fail("bot " + bot.name + ": min_size..max_size holds no lot multiple of " + bot.instrument_symbol);
                }
            }
        }
        line_no = 0;
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
    if (!p.finish_bot_section()) {
        return false;
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
