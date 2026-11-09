#include "exsim/research/experiment_config.hpp"

#include "exsim/research/seed_derivation.hpp"
#include "exsim/sim/price_text.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>

namespace exsim::research {

namespace {

constexpr Timestamp kMaxLatencyNs = 10 * kNanosPerSecond;
constexpr Timestamp kMaxDurationNs = 7ull * 24 * 3600 * kNanosPerSecond;

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

template <typename Integer>
bool parse_integer(std::string_view text, Integer& out) noexcept {
    if (text.empty()) return false;
    if constexpr (std::is_unsigned_v<Integer>) {
        if (text.front() == '-' || text.front() == '+') return false;
    }
    Integer parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
    out = parsed;
    return true;
}

bool parse_boolean(std::string_view text, bool& out) noexcept {
    if (text == "true" || text == "1") { out = true; return true; }
    if (text == "false" || text == "0") { out = false; return true; }
    return false;
}

bool parse_money(std::string_view text, std::int64_t& out) {
    const auto parsed = sim::parse_price(text);
    if (!parsed) return false;
    out = *parsed;
    return true;
}

bool parse_stp(std::string_view text, StpMode& out) noexcept {
    if (text == "none") out = StpMode::None;
    else if (text == "cancel_resting") out = StpMode::CancelResting;
    else if (text == "cancel_aggressor") out = StpMode::CancelAggressor;
    else if (text == "cancel_both") out = StpMode::CancelBoth;
    else return false;
    return true;
}

std::string_view stp_text(StpMode mode) noexcept {
    switch (mode) {
        case StpMode::None: return "none";
        case StpMode::CancelResting: return "cancel_resting";
        case StpMode::CancelAggressor: return "cancel_aggressor";
        case StpMode::CancelBoth: return "cancel_both";
    }
    return "?";
}

bool parse_strategy_kind(std::string_view text, StrategyKind& out) noexcept {
    for (const auto kind : {StrategyKind::FixedSpreadMarketMaker, StrategyKind::InventoryAwareMarketMaker,
                            StrategyKind::TwapExecutor, StrategyKind::ImmediateSweepExecutor}) {
        if (text == to_string(kind)) { out = kind; return true; }
    }
    return false;
}

std::string boolean_text(bool value) { return value ? "true" : "false"; }
std::string price_text(Price value) { return sim::format_price(value); }

bool is_market_maker_kind(StrategyKind kind) noexcept {
    return kind == StrategyKind::FixedSpreadMarketMaker || kind == StrategyKind::InventoryAwareMarketMaker;
}

// Strategy keys and the kinds that accept them.
bool strategy_key_applies(std::string_view key, StrategyKind kind) noexcept {
    if (key == "kind" || key == "stp") return true;
    if (key == "half_spread_ticks" || key == "quote_size" || key == "requote_interval" ||
        key == "requote_threshold_ticks" || key == "post_only")
        return is_market_maker_kind(kind);
    if (key == "skew_ticks_per_unit" || key == "skew_unit" || key == "scale_size_with_inventory")
        return kind == StrategyKind::InventoryAwareMarketMaker;
    if (key == "side" || key == "parent_qty" || key == "start" || key == "limit_offset_ticks" || key == "execution_duration")
        return kind == StrategyKind::TwapExecutor || kind == StrategyKind::ImmediateSweepExecutor;
    if (key == "slice_interval") return kind == StrategyKind::TwapExecutor;
    return false;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    ExperimentConfig parse() {
        config_ = default_experiment_config();
        std::size_t position = 0;
        while (position <= text_.size()) {
            const auto end = text_.find('\n', position);
            const auto raw = text_.substr(position, end == std::string_view::npos ? std::string_view::npos : end - position);
            ++line_;
            handle_line(raw);
            if (end == std::string_view::npos) break;
            position = end + 1;
        }
        for (const auto& key : strategy_keys_) {
            if (!strategy_key_applies(key.first, config_.strategy.kind))
                throw ConfigError(key.second, "strategy key '" + key.first + "' does not apply to kind " +
                                                  std::string(to_string(config_.strategy.kind)));
        }
        if (!auction_fee_set_) config_.fees.auction_fee_per_share = config_.fees.taker_fee_per_share;
        validate_experiment_config(config_);
        return config_;
    }

private:
    enum class Section { None, Experiment, Instrument, Engine, Latency, Fees, Risk, Accounting, Metrics, Runner, Strategy, Agent };

    [[noreturn]] void fail(const std::string& message) const { throw ConfigError(line_, message); }

    void handle_line(std::string_view raw) {
        // Comments are whole lines only: '#' or ';' as the first non-blank
        // character. Elsewhere both are ordinary text, so free-text values
        // such as a description are never cut short.
        const auto line = trim(raw);
        if (line.empty() || line.front() == '#' || line.front() == ';') return;
        if (line.front() == '[') {
            if (line.back() != ']') fail("unterminated section header");
            open_section(trim(line.substr(1, line.size() - 2)));
            return;
        }
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) fail("expected key = value");
        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));
        if (key.empty()) fail("empty key");
        if (section_ == Section::None) fail("key outside a section");
        const std::string seen_key = section_name_ + "." + std::string(key);
        if (!seen_keys_.insert(seen_key).second) fail("duplicate key '" + std::string(key) + "'");
        apply(key, value);
    }

    void open_section(std::string_view header) {
        const auto space = header.find(' ');
        const auto word = header.substr(0, space);
        section_name_ = std::string(header);
        if (word == "agent") {
            if (space == std::string_view::npos) fail("agent section needs a kind and a name");
            auto rest = trim(header.substr(space + 1));
            const auto second_space = rest.find(' ');
            if (second_space == std::string_view::npos) fail("agent section needs a kind and a name");
            const auto kind = rest.substr(0, second_space);
            const auto name = trim(rest.substr(second_space + 1));
            if (name.empty() || name.find(' ') != std::string_view::npos) fail("agent name must be one word");
            BackgroundAgentConfig agent;
            if (kind == "market_maker") agent.kind = BackgroundAgentKind::MarketMaker;
            else if (kind == "noise_taker") agent.kind = BackgroundAgentKind::NoiseTaker;
            else fail("unknown agent kind '" + std::string(kind) + "'");
            agent.name = std::string(name);
            for (const auto& existing : config_.agents)
                if (existing.name == agent.name) fail("duplicate agent name '" + agent.name + "'");
            if (config_.agents.size() == kMaxBackgroundAgents) fail("too many background agents");
            config_.agents.push_back(agent);
            section_ = Section::Agent;
            return;
        }
        if (space != std::string_view::npos) fail("unexpected words in section header");
        if (!opened_sections_.insert(std::string(word)).second) fail("duplicate section [" + std::string(word) + "]");
        if (word == "experiment") section_ = Section::Experiment;
        else if (word == "instrument") section_ = Section::Instrument;
        else if (word == "engine") section_ = Section::Engine;
        else if (word == "latency") section_ = Section::Latency;
        else if (word == "fees") section_ = Section::Fees;
        else if (word == "risk") section_ = Section::Risk;
        else if (word == "accounting") section_ = Section::Accounting;
        else if (word == "metrics") section_ = Section::Metrics;
        else if (word == "runner") section_ = Section::Runner;
        else if (word == "strategy") section_ = Section::Strategy;
        else fail("unknown section [" + std::string(word) + "]");
    }

    void require(bool ok, std::string_view key) const {
        if (!ok) fail("invalid value for '" + std::string(key) + "'");
    }

    void duration(std::string_view key, std::string_view value, Timestamp& out) const {
        require(parse_duration(value, out), key);
    }

    void apply(std::string_view key, std::string_view value) {
        switch (section_) {
            case Section::Experiment: return apply_experiment(key, value);
            case Section::Instrument: return apply_instrument(key, value);
            case Section::Engine:
                if (key == "max_orders") return require(parse_integer(value, config_.engine.max_orders), key);
                break;
            case Section::Latency: return apply_latency(key, value);
            case Section::Fees:
                if (key == "maker_fee_per_share") return require(parse_money(value, config_.fees.maker_fee_per_share), key);
                if (key == "taker_fee_per_share") return require(parse_money(value, config_.fees.taker_fee_per_share), key);
                if (key == "auction_fee_per_share") {
                    auction_fee_set_ = true;
                    return require(parse_money(value, config_.fees.auction_fee_per_share), key);
                }
                break;
            case Section::Risk:
                if (key == "max_long_position") return require(parse_integer(value, config_.risk.max_long_position), key);
                if (key == "max_short_position") return require(parse_integer(value, config_.risk.max_short_position), key);
                if (key == "max_order_qty") return require(parse_integer(value, config_.risk.max_order_qty), key);
                if (key == "max_open_orders") return require(parse_integer(value, config_.risk.max_open_orders), key);
                break;
            case Section::Accounting:
                if (key == "initial_cash") return require(parse_money(value, config_.accounting.initial_cash), key);
                if (key == "mark") {
                    if (value == "exchange_mid") config_.accounting.mark = MarkConvention::ExchangeMid;
                    else if (value == "last_trade_price") config_.accounting.mark = MarkConvention::LastTrade;
                    else fail("mark must be exchange_mid or last_trade_price");
                    return;
                }
                break;
            case Section::Metrics: return apply_metrics(key, value);
            case Section::Runner: return apply_runner(key, value);
            case Section::Strategy: return apply_strategy(key, value);
            case Section::Agent: return apply_agent(key, value);
            case Section::None: break;
        }
        fail("unknown key '" + std::string(key) + "' in [" + section_name_ + "]");
    }

    void apply_experiment(std::string_view key, std::string_view value) {
        if (key == "name") { config_.name = std::string(value); return; }
        if (key == "description") { config_.description = std::string(value); return; }
        if (key == "seed") return require(parse_integer(value, config_.default_seed), key);
        if (key == "duration") return duration(key, value, config_.limits.duration_ns);
        if (key == "max_simulation_events") return require(parse_integer(value, config_.limits.max_simulation_events), key);
        fail("unknown key '" + std::string(key) + "' in [experiment]");
    }

    void apply_instrument(std::string_view key, std::string_view value) {
        auto& instrument = config_.instrument;
        if (key == "symbol") {
            if (value.empty() || value.size() > 8) fail("symbol must be 1..8 characters");
            instrument.symbol = Symbol(value);
            return;
        }
        if (key == "tick_size") return require(parse_money(value, instrument.tick_size), key);
        if (key == "lot_size") return require(parse_integer(value, instrument.lot_size), key);
        if (key == "max_order_qty") return require(parse_integer(value, instrument.max_order_qty), key);
        if (key == "min_price") return require(parse_money(value, instrument.min_price), key);
        if (key == "max_price") return require(parse_money(value, instrument.max_price), key);
        if (key == "reference_price") return require(parse_money(value, instrument.reference_price), key);
        fail("unknown key '" + std::string(key) + "' in [instrument]");
    }

    void apply_latency(std::string_view key, std::string_view value) {
        auto& latency = config_.latency;
        if (key == "feed") return duration(key, value, latency.feed_ns);
        if (key == "order") return duration(key, value, latency.order_ns);
        if (key == "cancel") return duration(key, value, latency.cancel_ns);
        if (key == "ack") return duration(key, value, latency.ack_ns);
        if (key == "strategy_compute") return duration(key, value, latency.strategy_compute_ns);
        if (key == "feed_jitter") return duration(key, value, latency.feed_jitter_ns);
        if (key == "order_jitter") return duration(key, value, latency.order_jitter_ns);
        if (key == "cancel_jitter") return duration(key, value, latency.cancel_jitter_ns);
        if (key == "ack_jitter") return duration(key, value, latency.ack_jitter_ns);
        if (key == "order_channel_fifo") return require(parse_boolean(value, latency.order_channel_fifo), key);
        fail("unknown key '" + std::string(key) + "' in [latency]");
    }

    void apply_metrics(std::string_view key, std::string_view value) {
        if (key == "sample_interval") return duration(key, value, config_.metrics.sample_interval_ns);
        if (key == "markout_horizons") {
            config_.metrics.markout_horizon_count = 0;
            std::size_t position = 0;
            while (position <= value.size()) {
                const auto comma = value.find(',', position);
                const auto item = trim(value.substr(position, comma == std::string_view::npos ? std::string_view::npos : comma - position));
                if (config_.metrics.markout_horizon_count == kMaxMarkoutHorizons) fail("at most 8 markout horizons");
                Timestamp horizon = 0;
                duration(key, item, horizon);
                config_.metrics.markout_horizons_ns[config_.metrics.markout_horizon_count++] = horizon;
                if (comma == std::string_view::npos) break;
                position = comma + 1;
            }
            return;
        }
        fail("unknown key '" + std::string(key) + "' in [metrics]");
    }

    void apply_runner(std::string_view key, std::string_view value) {
        auto& limits = config_.limits;
        if (key == "simulation_queue_capacity") return require(parse_integer(value, limits.simulation_queue_capacity), key);
        if (key == "observation_queue_capacity") return require(parse_integer(value, limits.observation_queue_capacity), key);
        if (key == "recorder_command_capacity") return require(parse_integer(value, limits.recorder_command_capacity), key);
        if (key == "recorder_record_capacity") return require(parse_integer(value, limits.recorder_record_capacity), key);
        if (key == "recorder_flush_margin") return require(parse_integer(value, limits.recorder_flush_margin), key);
        fail("unknown key '" + std::string(key) + "' in [runner]");
    }

    void apply_strategy(std::string_view key, std::string_view value) {
        auto& strategy = config_.strategy;
        strategy_keys_.emplace_back(std::string(key), line_);
        if (key == "kind") {
            if (!parse_strategy_kind(value, strategy.kind)) fail("unknown strategy kind '" + std::string(value) + "'");
            return;
        }
        if (key == "stp") return require(parse_stp(value, strategy.stp), key);
        if (key == "half_spread_ticks") return require(parse_integer(value, strategy.quoting.half_spread_ticks), key);
        if (key == "quote_size") return require(parse_integer(value, strategy.quoting.quote_size), key);
        if (key == "requote_interval") return duration(key, value, strategy.quoting.requote_interval_ns);
        if (key == "requote_threshold_ticks") return require(parse_integer(value, strategy.quoting.requote_threshold_ticks), key);
        if (key == "post_only") return require(parse_boolean(value, strategy.quoting.post_only), key);
        if (key == "skew_ticks_per_unit") return require(parse_integer(value, strategy.inventory.skew_ticks_per_unit), key);
        if (key == "skew_unit") return require(parse_integer(value, strategy.inventory.skew_unit), key);
        if (key == "scale_size_with_inventory") return require(parse_boolean(value, strategy.inventory.scale_size_with_inventory), key);
        if (key == "side") {
            if (value == "buy") strategy.parent.side = Side::BUY;
            else if (value == "sell") strategy.parent.side = Side::SELL;
            else fail("side must be buy or sell");
            return;
        }
        if (key == "parent_qty") return require(parse_integer(value, strategy.parent.parent_qty), key);
        if (key == "start") return duration(key, value, strategy.parent.start_ns);
        if (key == "execution_duration") return duration(key, value, strategy.parent.execution_duration_ns);
        if (key == "slice_interval") return duration(key, value, strategy.parent.slice_interval_ns);
        if (key == "limit_offset_ticks") return require(parse_integer(value, strategy.parent.limit_offset_ticks), key);
        fail("unknown key '" + std::string(key) + "' in [strategy]");
    }

    void apply_agent(std::string_view key, std::string_view value) {
        auto& agent = config_.agents.back();
        if (key == "seed" || key == "session_id" || key == "instrument" || key == "name")
            fail("'" + std::string(key) + "' is assigned by the runner (seeds are derived from the master seed)");
        if (key == "timer_interval") return duration(key, value, agent.timer_interval_ns);
        const bool applied = agent.kind == BackgroundAgentKind::MarketMaker
                                 ? sim::bots::apply_bot_option(agent.market_maker, key, value)
                                 : sim::bots::apply_bot_option(agent.noise_taker, key, value);
        if (!applied) fail("unknown key or invalid value '" + std::string(key) + "' for agent " + agent.name);
        if (key == "owner") {
            const OwnerId owner = agent.kind == BackgroundAgentKind::MarketMaker ? agent.market_maker.owner : agent.noise_taker.owner;
            if (owner == kStrategyOwnerId) fail("owner 1 is reserved for the research strategy");
        }
    }

    std::string_view text_;
    std::size_t line_ = 0;
    Section section_ = Section::None;
    std::string section_name_;
    std::set<std::string> opened_sections_;
    std::set<std::string> seen_keys_;
    std::vector<std::pair<std::string, std::size_t>> strategy_keys_;
    bool auction_fee_set_ = false;
    ExperimentConfig config_;
};

void check(bool ok, const std::string& message) {
    if (!ok) throw ConfigError(0, message);
}

bool on_grid(std::uint64_t quantity, Quantity lot) noexcept { return lot != 0 && quantity % lot == 0; }

}  // namespace

std::string_view to_string(StrategyKind kind) noexcept {
    switch (kind) {
        case StrategyKind::FixedSpreadMarketMaker: return "fixed_spread_market_maker";
        case StrategyKind::InventoryAwareMarketMaker: return "inventory_aware_market_maker";
        case StrategyKind::TwapExecutor: return "twap_executor";
        case StrategyKind::ImmediateSweepExecutor: return "immediate_sweep_executor";
    }
    return "?";
}

std::string_view to_string(MarkConvention mark) noexcept {
    switch (mark) {
        case MarkConvention::ExchangeMid: return "exchange_mid";
        case MarkConvention::LastTrade: return "last_trade_price";
    }
    return "?";
}

std::string_view to_string(BackgroundAgentKind kind) noexcept {
    return kind == BackgroundAgentKind::MarketMaker ? "market_maker" : "noise_taker";
}

bool parse_duration(std::string_view text, Timestamp& out) noexcept {
    std::size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') ++digits;
    if (digits == 0) return false;
    std::uint64_t amount = 0;
    if (!parse_integer(text.substr(0, digits), amount)) return false;
    const auto unit = text.substr(digits);
    std::uint64_t scale = 0;
    if (unit == "ns") scale = 1;
    else if (unit == "us") scale = kNanosPerMicrosecond;
    else if (unit == "ms") scale = kNanosPerMillisecond;
    else if (unit == "s") scale = kNanosPerSecond;
    else return false;
    if (amount > std::numeric_limits<std::uint64_t>::max() / scale) return false;
    out = amount * scale;
    return true;
}

std::string format_duration(Timestamp nanoseconds) {
    if (nanoseconds != 0 && nanoseconds % kNanosPerSecond == 0) return std::to_string(nanoseconds / kNanosPerSecond) + "s";
    if (nanoseconds != 0 && nanoseconds % kNanosPerMillisecond == 0) return std::to_string(nanoseconds / kNanosPerMillisecond) + "ms";
    if (nanoseconds != 0 && nanoseconds % kNanosPerMicrosecond == 0) return std::to_string(nanoseconds / kNanosPerMicrosecond) + "us";
    return std::to_string(nanoseconds) + "ns";
}

ExperimentConfig default_experiment_config() {
    ExperimentConfig config;
    config.instrument.symbol = Symbol("SYNTH");
    config.instrument.tick_size = 100;              // 0.01
    config.instrument.lot_size = 1;
    config.instrument.max_order_qty = 100'000;
    config.instrument.min_price = 500'000;          // 50.00
    config.instrument.max_price = 1'500'000;        // 150.00
    config.instrument.reference_price = 1'000'000;  // 100.00
    config.instrument.initial_phase = Phase::Continuous;
    config.metrics.markout_horizons_ns[0] = 100 * kNanosPerMillisecond;
    config.metrics.markout_horizons_ns[1] = 1 * kNanosPerSecond;
    config.metrics.markout_horizons_ns[2] = 5 * kNanosPerSecond;
    config.metrics.markout_horizon_count = 3;
    return config;
}

ExperimentConfig parse_experiment_config(std::string_view text) { return Parser(text).parse(); }

Timestamp background_agent_timer_interval(const BackgroundAgentConfig& agent) noexcept {
    if (agent.timer_interval_ns != 0) return agent.timer_interval_ns;
    const std::uint32_t millis = agent.kind == BackgroundAgentKind::MarketMaker ? agent.market_maker.requote_ms
                                                                                : agent.noise_taker.mean_interval_ms;
    return std::max<Timestamp>(1, Timestamp{millis} * kNanosPerMillisecond);
}

RecorderStepBound recorder_step_bound(const ExperimentConfig& config) noexcept {
    const std::size_t open_orders = config.risk.max_open_orders;
    const std::size_t lot = std::max<Quantity>(1, config.instrument.lot_size);
    // Fills of one strategy order inside one command: every fill takes at
    // least one lot and needs a distinct resting order.
    const std::size_t fills_per_strategy_order =
        std::max<std::size_t>(1, std::min<std::size_t>(config.risk.max_order_qty / lot, config.engine.max_orders));

    // Engine commands one step can submit. A strategy arrival or scripted
    // command is one. A market maker timer sends at most one request per order
    // slot in each of its three passes plus one new order per target level; a
    // noise taker cancels expired orders and places at most one arrival.
    std::size_t commands = 1;
    for (const BackgroundAgentConfig& agent : config.agents) {
        std::size_t agent_commands = 0;
        if (agent.kind == BackgroundAgentKind::MarketMaker) {
            const std::size_t levels = std::min<std::size_t>(agent.market_maker.levels, sim::bots::MarketMaker::kMaxLevels);
            agent_commands = 3 * agent.market_maker.order_capacity + 2 * levels;
        } else {
            agent_commands = agent.noise_taker.order_capacity + 1;
        }
        commands = std::max(commands, agent_commands);
    }

    // Lifecycle rows (ledger fills are never more) per kind of step.
    // Strategy arrival: arrival, accepted or rejected, replaced, rested or
    // canceled, two legs per fill (a self match fills two strategy orders),
    // and self-trade cancels of resting strategy orders.
    const std::size_t arrival_rows = 4 + 2 * fills_per_strategy_order + open_orders;
    // Background or scripted step: each command fills or cancels every
    // resting strategy order at most once, and the strategy's resting
    // quantity bounds the fills of the whole step.
    const std::size_t background_rows =
        std::min(commands * open_orders, open_orders * fills_per_strategy_order) + 2 * open_orders;
    // Strategy callback: the report delivery row plus the requests and risk
    // rejects of the compiled strategies (at most one request per open order
    // and one new order per side). A future built-in strategy that sends more in one
    // callback can still overflow; the run then fails as recorder_overflow.
    const std::size_t callback_rows = 1 + 2 * (open_orders + 1);
    return RecorderStepBound{commands, std::max({arrival_rows, background_rows, callback_rows})};
}

std::size_t effective_recorder_flush_margin(const ExperimentConfig& config) noexcept {
    const RecorderStepBound bound = recorder_step_bound(config);
    return std::max({config.limits.recorder_flush_margin, bound.commands, bound.records});
}

void validate_experiment_config(ExperimentConfig& config) {
    check(config.strategy.kind <= StrategyKind::ImmediateSweepExecutor, "unknown strategy kind");
    check(config.accounting.mark <= MarkConvention::LastTrade, "unknown mark convention");
    auto& instrument = config.instrument;
    check(config.name.size() <= 128, "experiment name is longer than 128 bytes");
    check(valid_instrument_config(instrument), "invalid instrument rules (tick, lot, band, reference)");
    check(instrument.initial_phase == Phase::Continuous, "only a continuous initial phase is supported");
    check(instrument.symbol.view().size() > 0, "instrument symbol is empty");
    const auto levels = (instrument.max_price - instrument.min_price) / instrument.tick_size + 1;
    check(levels <= 1'000'000, "instrument price band has more than 1000000 ticks");

    check(config.engine.max_orders >= 16 && config.engine.max_orders <= (1u << 22), "engine max_orders must be 16..4194304");

    const auto& latency = config.latency;
    for (const Timestamp value : {latency.feed_ns, latency.order_ns, latency.cancel_ns, latency.ack_ns,
                                  latency.strategy_compute_ns, latency.feed_jitter_ns, latency.order_jitter_ns,
                                  latency.cancel_jitter_ns, latency.ack_jitter_ns})
        check(value <= kMaxLatencyNs, "latency and jitter components must be at most 10s");

    constexpr std::int64_t kMaxFee = 100 * kPriceScale;
    for (const std::int64_t fee : {config.fees.maker_fee_per_share, config.fees.taker_fee_per_share, config.fees.auction_fee_per_share})
        check(fee >= -kMaxFee && fee <= kMaxFee, "fees must be within +/-100.00 per share");

    const auto& risk = config.risk;
    check(risk.max_long_position >= 0 && risk.max_short_position >= 0, "position limits must be nonnegative");
    check(risk.max_long_position <= 1'000'000'000 && risk.max_short_position <= 1'000'000'000, "position limits must be at most 1e9");
    check(risk.max_order_qty > 0, "risk max_order_qty must be positive");
    check(risk.max_open_orders >= 1 && risk.max_open_orders <= 4096, "risk max_open_orders must be 1..4096");

    check(config.accounting.initial_cash >= 0 && config.accounting.initial_cash <= std::int64_t{1} << 52,
          "initial_cash must be 0..450359962737.0496");

    const auto& metrics = config.metrics;
    check(metrics.markout_horizon_count <= kMaxMarkoutHorizons, "too many markout horizons");
    check(metrics.sample_interval_ns > 0, "sample_interval must be positive");
    for (std::size_t i = 0; i < metrics.markout_horizon_count; ++i) {
        check(metrics.markout_horizons_ns[i] > 0 && metrics.markout_horizons_ns[i] <= kMaxDurationNs, "markout horizons must be positive");
        // Each horizon names its summary metrics (markout_<duration>_*), so a
        // repeated value (1s, 1000ms) would write the same JSON key twice.
        check(i == 0 || metrics.markout_horizons_ns[i] > metrics.markout_horizons_ns[i - 1],
              "markout horizons must be strictly increasing (no duplicates)");
    }

    auto& limits = config.limits;
    check(limits.duration_ns > 0 && limits.duration_ns <= kMaxDurationNs, "duration must be positive and at most 7 days");
    check(limits.max_simulation_events > 0, "max_simulation_events must be positive");
    // Bound every cumulative monetary metric, including doubled markouts,
    // before entering the allocation-free loop. Each simulated event can
    // execute at most max_open_orders existing strategy orders plus one new
    // order. Leave room for valuation and reconciliation intermediates.
    const long double monetary_bound = static_cast<long double>(limits.max_simulation_events) *
        (static_cast<long double>(risk.max_open_orders) + 1) * risk.max_order_qty *
        (static_cast<long double>(instrument.max_price) + kMaxFee);
    check(monetary_bound <= static_cast<long double>(std::numeric_limits<std::int64_t>::max()) / 4,
          "event, order and price bounds exceed safe metric arithmetic capacity");
    check(static_cast<long double>(std::max(risk.max_long_position, risk.max_short_position)) * instrument.max_price <=
              static_cast<long double>(std::numeric_limits<std::int64_t>::max()) / 4,
          "position value exceeds safe metric arithmetic capacity");
    check(limits.simulation_queue_capacity >= 64 && limits.simulation_queue_capacity <= (1u << 24), "simulation_queue_capacity must be 64..16777216");
    check(limits.observation_queue_capacity >= 64 && limits.observation_queue_capacity <= (1u << 24), "observation_queue_capacity must be 64..16777216");
    check(limits.recorder_command_capacity >= 64 && limits.recorder_command_capacity <= (1u << 24), "recorder_command_capacity must be 64..16777216");
    check(limits.recorder_record_capacity >= 64 && limits.recorder_record_capacity <= (1u << 24), "recorder_record_capacity must be 64..16777216");
    check(limits.recorder_flush_margin >= 16 && 2 * limits.recorder_flush_margin <= limits.recorder_command_capacity &&
              2 * limits.recorder_flush_margin <= limits.recorder_record_capacity,
          "recorder_flush_margin must be at least 16 and at most half of each recorder capacity");

    const auto& strategy = config.strategy;
    const Quantity lot = instrument.lot_size;
    if (is_market_maker_kind(strategy.kind)) {
        check(strategy.quoting.half_spread_ticks >= 1 && strategy.quoting.half_spread_ticks <= 10'000, "half_spread_ticks must be 1..10000");
        check(strategy.quoting.quote_size > 0 && on_grid(strategy.quoting.quote_size, lot) &&
                  strategy.quoting.quote_size <= instrument.max_order_qty,
              "quote_size must be a positive multiple of lot_size within max_order_qty");
        check(strategy.quoting.requote_interval_ns > 0, "requote_interval must be positive");
        check(strategy.quoting.requote_threshold_ticks >= 1, "requote_threshold_ticks must be at least 1");
    }
    if (strategy.kind == StrategyKind::InventoryAwareMarketMaker) {
        check(strategy.inventory.skew_unit > 0, "skew_unit must be positive");
        check(strategy.inventory.skew_ticks_per_unit <= 10'000, "skew_ticks_per_unit must be at most 10000");
    }
    if (strategy.kind == StrategyKind::TwapExecutor || strategy.kind == StrategyKind::ImmediateSweepExecutor) {
        check(strategy.parent.parent_qty > 0 && on_grid(strategy.parent.parent_qty, lot), "parent_qty must be a positive multiple of lot_size");
        check(strategy.parent.execution_duration_ns <= kMaxDurationNs, "execution_duration exceeds 7 days");
        check(strategy.parent.start_ns < limits.duration_ns, "start must be before the run duration ends");
        check(strategy.parent.limit_offset_ticks <= 100'000, "limit_offset_ticks must be at most 100000");
    }
    if (strategy.kind == StrategyKind::TwapExecutor) {
        check(strategy.parent.slice_interval_ns > 0, "slice_interval must be positive");
        check(strategy.parent.execution_duration_ns >= strategy.parent.slice_interval_ns, "execution_duration must be at least one slice_interval");
        check(strategy.parent.execution_duration_ns / strategy.parent.slice_interval_ns <= 1'000'000, "at most 1000000 TWAP slices");
    }

    check(config.agents.size() <= kMaxBackgroundAgents, "at most 16 background agents");
    for (std::size_t index = 0; index < config.agents.size(); ++index) {
        auto& agent = config.agents[index];
        sim::bots::BotConfig& identity = agent.kind == BackgroundAgentKind::MarketMaker
                                             ? static_cast<sim::bots::BotConfig&>(agent.market_maker)
                                             : static_cast<sim::bots::BotConfig&>(agent.noise_taker);
        identity.name.assign(agent.name);
        identity.session_id = background_agent_session(index);
        identity.instrument = kResearchInstrumentId;
        if (identity.owner == 0) identity.owner = static_cast<OwnerId>(identity.session_id);
        check(identity.owner != kStrategyOwnerId, "owner 1 is reserved for the research strategy");
        if (agent.kind == BackgroundAgentKind::NoiseTaker)
            check(agent.noise_taker.min_size <= agent.noise_taker.max_size, "noise taker min_size exceeds max_size");
        check(agent.timer_interval_ns <= kMaxDurationNs, "agent timer_interval too large");
        const std::size_t order_capacity =
            agent.kind == BackgroundAgentKind::MarketMaker ? agent.market_maker.order_capacity : agent.noise_taker.order_capacity;
        check(order_capacity >= 1 && order_capacity <= 65'536, "agent order_capacity must be 1..65536");
    }

    // Free recorder space is checked only between steps (after agent
    // order_capacity is known to be bounded).
    const std::size_t step_margin = effective_recorder_flush_margin(config);
    check(2 * step_margin <= limits.recorder_command_capacity && 2 * step_margin <= limits.recorder_record_capacity,
          "recorder capacities must be at least twice the records one step can produce (" + std::to_string(step_margin) +
              ", from max_open_orders, risk max_order_qty, lot_size and agent order_capacity)");
}

ConfigEntries strategy_parameter_entries(const StrategyConfig& strategy) {
    ConfigEntries entries;
    entries.emplace_back("stp", std::string(stp_text(strategy.stp)));
    if (is_market_maker_kind(strategy.kind)) {
        entries.emplace_back("half_spread_ticks", std::to_string(strategy.quoting.half_spread_ticks));
        entries.emplace_back("quote_size", std::to_string(strategy.quoting.quote_size));
        entries.emplace_back("requote_interval_ns", std::to_string(strategy.quoting.requote_interval_ns));
        entries.emplace_back("requote_threshold_ticks", std::to_string(strategy.quoting.requote_threshold_ticks));
        entries.emplace_back("post_only", boolean_text(strategy.quoting.post_only));
    }
    if (strategy.kind == StrategyKind::InventoryAwareMarketMaker) {
        entries.emplace_back("skew_ticks_per_unit", std::to_string(strategy.inventory.skew_ticks_per_unit));
        entries.emplace_back("skew_unit", std::to_string(strategy.inventory.skew_unit));
        entries.emplace_back("scale_size_with_inventory", boolean_text(strategy.inventory.scale_size_with_inventory));
    }
    if (strategy.kind == StrategyKind::TwapExecutor || strategy.kind == StrategyKind::ImmediateSweepExecutor) {
        entries.emplace_back("side", strategy.parent.side == Side::BUY ? "buy" : "sell");
        entries.emplace_back("parent_qty", std::to_string(strategy.parent.parent_qty));
        entries.emplace_back("start_ns", std::to_string(strategy.parent.start_ns));
        entries.emplace_back("limit_offset_ticks", std::to_string(strategy.parent.limit_offset_ticks));
        entries.emplace_back("execution_duration_ns", std::to_string(strategy.parent.execution_duration_ns));
    }
    if (strategy.kind == StrategyKind::TwapExecutor) {
        entries.emplace_back("slice_interval_ns", std::to_string(strategy.parent.slice_interval_ns));
    }
    return entries;
}

ConfigEntries background_agent_parameter_entries(const BackgroundAgentConfig& agent) {
    ConfigEntries entries;
    entries.emplace_back("timer_interval_ns", std::to_string(background_agent_timer_interval(agent)));
    if (agent.kind == BackgroundAgentKind::MarketMaker) {
        const auto& maker = agent.market_maker;
        entries.emplace_back("owner", std::to_string(maker.owner));
        entries.emplace_back("stp", std::string(stp_text(maker.stp)));
        entries.emplace_back("enabled", boolean_text(maker.enabled));
        entries.emplace_back("levels", std::to_string(maker.levels));
        entries.emplace_back("size", std::to_string(maker.size));
        entries.emplace_back("spread_ticks", std::to_string(maker.spread_ticks));
        entries.emplace_back("fair_value_ticks", std::to_string(maker.fair_value_ticks));
        entries.emplace_back("fair_value_step_ms", std::to_string(maker.fair_value_step_ms));
        entries.emplace_back("fair_value_step_ticks", std::to_string(maker.fair_value_step_ticks));
        entries.emplace_back("requote_ms", std::to_string(maker.requote_ms));
        entries.emplace_back("skew_ticks_per_unit", std::to_string(maker.skew_ticks_per_unit));
        entries.emplace_back("cancel_on_halt", boolean_text(maker.cancel_on_halt));
        entries.emplace_back("order_capacity", std::to_string(maker.order_capacity));
    } else {
        const auto& taker = agent.noise_taker;
        entries.emplace_back("owner", std::to_string(taker.owner));
        entries.emplace_back("stp", std::string(stp_text(taker.stp)));
        entries.emplace_back("enabled", boolean_text(taker.enabled));
        entries.emplace_back("mean_interval_ms", std::to_string(taker.mean_interval_ms));
        entries.emplace_back("min_size", std::to_string(taker.min_size));
        entries.emplace_back("max_size", std::to_string(taker.max_size));
        entries.emplace_back("cross_ticks", std::to_string(taker.cross_ticks));
        entries.emplace_back("limit_ppm", std::to_string(taker.limit_ppm));
        entries.emplace_back("limit_lifetime_ms", std::to_string(taker.limit_lifetime_ms));
        entries.emplace_back("fair_value_ticks", std::to_string(taker.fair_value_ticks));
        entries.emplace_back("order_capacity", std::to_string(taker.order_capacity));
    }
    return entries;
}

ConfigEntries canonical_config_entries(const ExperimentConfig& config) {
    ConfigEntries entries;
    auto add = [&](std::string key, std::string value) { entries.emplace_back(std::move(key), std::move(value)); };
    add("experiment.name", config.name);
    add("experiment.description", config.description);
    add("experiment.duration_ns", std::to_string(config.limits.duration_ns));
    add("experiment.max_simulation_events", std::to_string(config.limits.max_simulation_events));
    const auto& instrument = config.instrument;
    add("instrument.symbol", std::string(instrument.symbol.view()));
    add("instrument.tick_size", price_text(instrument.tick_size));
    add("instrument.lot_size", std::to_string(instrument.lot_size));
    add("instrument.max_order_qty", std::to_string(instrument.max_order_qty));
    add("instrument.min_price", price_text(instrument.min_price));
    add("instrument.max_price", price_text(instrument.max_price));
    add("instrument.reference_price", price_text(instrument.reference_price));
    add("instrument.initial_phase", std::string(to_string(instrument.initial_phase)));
    add("engine.max_orders", std::to_string(config.engine.max_orders));
    const auto& latency = config.latency;
    add("latency.feed_ns", std::to_string(latency.feed_ns));
    add("latency.order_ns", std::to_string(latency.order_ns));
    add("latency.cancel_ns", std::to_string(latency.cancel_ns));
    add("latency.ack_ns", std::to_string(latency.ack_ns));
    add("latency.strategy_compute_ns", std::to_string(latency.strategy_compute_ns));
    add("latency.feed_jitter_ns", std::to_string(latency.feed_jitter_ns));
    add("latency.order_jitter_ns", std::to_string(latency.order_jitter_ns));
    add("latency.cancel_jitter_ns", std::to_string(latency.cancel_jitter_ns));
    add("latency.ack_jitter_ns", std::to_string(latency.ack_jitter_ns));
    add("latency.order_channel_fifo", boolean_text(latency.order_channel_fifo));
    add("fees.maker_fee_per_share", price_text(config.fees.maker_fee_per_share));
    add("fees.taker_fee_per_share", price_text(config.fees.taker_fee_per_share));
    add("fees.auction_fee_per_share", price_text(config.fees.auction_fee_per_share));
    add("risk.max_long_position", std::to_string(config.risk.max_long_position));
    add("risk.max_short_position", std::to_string(config.risk.max_short_position));
    add("risk.max_order_qty", std::to_string(config.risk.max_order_qty));
    add("risk.max_open_orders", std::to_string(config.risk.max_open_orders));
    add("accounting.initial_cash", price_text(config.accounting.initial_cash));
    add("accounting.mark", std::string(to_string(config.accounting.mark)));
    std::string horizons;
    for (std::size_t i = 0; i < config.metrics.markout_horizon_count; ++i) {
        if (i) horizons += ",";
        horizons += std::to_string(config.metrics.markout_horizons_ns[i]);
    }
    add("metrics.markout_horizons_ns", horizons);
    add("metrics.sample_interval_ns", std::to_string(config.metrics.sample_interval_ns));
    add("runner.simulation_queue_capacity", std::to_string(config.limits.simulation_queue_capacity));
    add("runner.observation_queue_capacity", std::to_string(config.limits.observation_queue_capacity));
    add("runner.recorder_command_capacity", std::to_string(config.limits.recorder_command_capacity));
    add("runner.recorder_record_capacity", std::to_string(config.limits.recorder_record_capacity));
    add("runner.recorder_flush_margin", std::to_string(config.limits.recorder_flush_margin));
    add("strategy.kind", std::string(to_string(config.strategy.kind)));
    for (auto& entry : strategy_parameter_entries(config.strategy)) add("strategy." + entry.first, entry.second);
    for (std::size_t index = 0; index < config.agents.size(); ++index) {
        const auto& agent = config.agents[index];
        const std::string prefix = "agent." + std::to_string(index) + ".";
        add(prefix + "kind", std::string(to_string(agent.kind)));
        add(prefix + "name", agent.name);
        for (auto& entry : background_agent_parameter_entries(agent)) add(prefix + entry.first, entry.second);
    }
    return entries;
}

std::string canonical_config_text(const ExperimentConfig& config) {
    std::string text = "exsim.research.config/1\n";
    for (const auto& entry : canonical_config_entries(config)) {
        text += entry.first;
        text += '=';
        text += entry.second;
        text += '\n';
    }
    return text;
}

std::uint64_t experiment_config_hash(const ExperimentConfig& config) { return fnv1a64(canonical_config_text(config)); }

}  // namespace exsim::research
