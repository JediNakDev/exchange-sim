#include "exsim/api/experiment_config_translation.hpp"

#include "exsim/api/api_json.hpp"
#include "exsim/api/sha256.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>

namespace exsim::api {

namespace {

using research::JsonValue;
using Kind = research::JsonValue::Kind;

// Largest fee magnitude the runner allows per share (its kMaxFee), used to
// reproduce the runner's monetary arithmetic bound.
constexpr long double kRunnerMaxFeePriceUnits = 100.0L * 10'000.0L;
constexpr std::uint64_t kRunnerDefaultMaxSimulationEvents = 50'000'000;

std::string escape_pointer_token(std::string_view token) {
    std::string escaped;
    for (const char character : token) {
        if (character == '~') escaped += "~0";
        else if (character == '/') escaped += "~1";
        else escaped += character;
    }
    return escaped;
}

std::string child_pointer(const std::string& parent, std::string_view name) { return parent + "/" + escape_pointer_token(name); }
std::string index_pointer(const std::string& parent, std::size_t index) { return parent + "/" + std::to_string(index); }

bool is_price_decimal(std::string_view text) {
    std::int64_t units = 0;
    return parse_exact_fixed_decimal(text, 4, false, units);
}
bool is_signed_currency_decimal(std::string_view text) {
    std::int64_t units = 0;
    return parse_exact_fixed_decimal(text, 4, true, units);
}
bool is_unsigned_integer(std::string_view text) {
    std::uint64_t value = 0;
    return parse_unsigned_integer_text(text, value);
}
bool is_exact_decimal(std::string_view text) { return parse_scaled_decimal(text).has_value(); }
bool matches_identifier_pattern(std::string_view text) {  // ^[a-z0-9][a-z0-9_.-]{1,63}$
    if (text.size() < 2 || text.size() > 64) return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        const bool alphanumeric = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        if (!alphanumeric && (index == 0 || (character != '_' && character != '.' && character != '-'))) return false;
    }
    return true;
}
bool matches_symbol_pattern(std::string_view text) {  // ^[A-Z0-9.]{1,8}$
    if (text.empty() || text.size() > 8) return false;
    return std::all_of(text.begin(), text.end(), [](char character) {
        return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '.';
    });
}
bool matches_experiment_id_pattern(std::string_view text) {  // ^exp_[0-9a-z]{8,32}$
    if (text.substr(0, 4) != "exp_" || text.size() < 12 || text.size() > 36) return false;
    return std::all_of(text.begin() + 4, text.end(), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
    });
}

// Schema-shape reader: records an issue per problem and keeps going so a
// client sees every problem at once.
class ShapeReader {
public:
    explicit ShapeReader(std::vector<FieldIssue>& issues) : issues_(issues) {}

    void add(const std::string& pointer, std::string issue) { issues_.push_back({pointer, std::move(issue)}); }

    bool object(const JsonValue* value, const std::string& pointer, std::initializer_list<std::string_view> required,
                std::initializer_list<std::string_view> optional = {}) {
        if (value == nullptr) return false;
        if (value->kind != Kind::Object) {
            add(pointer, "must be an object");
            return false;
        }
        std::set<std::string_view> seen;
        for (const auto& member : value->members) {
            if (!seen.insert(member.name).second) add(child_pointer(pointer, member.name), "is repeated");
            const bool known = std::find(required.begin(), required.end(), member.name) != required.end() ||
                               std::find(optional.begin(), optional.end(), member.name) != optional.end();
            if (!known) add(child_pointer(pointer, member.name), "is not a field of this object");
        }
        for (const std::string_view name : required)
            if (value->find(name) == nullptr) add(child_pointer(pointer, name), "is required");
        return true;
    }

    std::optional<std::string> text(const JsonValue* value, const std::string& pointer, const std::function<bool(std::string_view)>& valid,
                                    std::string_view expectation) {
        if (value == nullptr) return std::nullopt;
        if (value->kind != Kind::String) {
            add(pointer, "must be a string");
            return std::nullopt;
        }
        if (valid && !valid(value->text)) {
            add(pointer, "must be " + std::string(expectation));
            return std::nullopt;
        }
        return value->text;
    }

    std::optional<std::string> enumeration(const JsonValue* value, const std::string& pointer, std::initializer_list<std::string_view> allowed) {
        std::string expectation = "one of";
        for (const std::string_view option : allowed) expectation += " " + std::string(option);
        return text(value, pointer, [&](std::string_view candidate) { return std::find(allowed.begin(), allowed.end(), candidate) != allowed.end(); },
                    expectation);
    }

    std::optional<std::uint64_t> unsigned_integer_text(const JsonValue* value, const std::string& pointer) {
        const auto parsed = text(value, pointer, is_unsigned_integer, "an unsigned integer string");
        if (!parsed) return std::nullopt;
        std::uint64_t result = 0;
        (void)parse_unsigned_integer_text(*parsed, result);
        return result;
    }

    std::optional<std::int64_t> integer_number(const JsonValue* value, const std::string& pointer, std::int64_t minimum, std::int64_t maximum) {
        if (value == nullptr) return std::nullopt;
        std::int64_t result = 0;
        if (value->kind != Kind::Number || !parse_signed_integer_text(value->text, result)) {
            add(pointer, "must be an integer");
            return std::nullopt;
        }
        if (result < minimum || result > maximum) {
            add(pointer, "must be between " + std::to_string(minimum) + " and " + std::to_string(maximum));
            return std::nullopt;
        }
        return result;
    }

    std::optional<bool> boolean(const JsonValue* value, const std::string& pointer) {
        if (value == nullptr) return std::nullopt;
        if (value->kind != Kind::Boolean) {
            add(pointer, "must be a boolean");
            return std::nullopt;
        }
        return value->boolean;
    }

    const JsonValue* array(const JsonValue* value, const std::string& pointer, std::size_t minimum, std::size_t maximum) {
        if (value == nullptr) return nullptr;
        if (value->kind != Kind::Array) {
            add(pointer, "must be an array");
            return nullptr;
        }
        if (value->items.size() < minimum || value->items.size() > maximum) {
            add(pointer, "must have " + std::to_string(minimum) + " to " + std::to_string(maximum) + " items");
            return nullptr;
        }
        return value;
    }

private:
    std::vector<FieldIssue>& issues_;
};

const JsonValue* member_of(const JsonValue* object, std::string_view name) {
    return object != nullptr && object->kind == Kind::Object ? object->find(name) : nullptr;
}

// Runner experiment-file text built line by line, remembering which JSON
// pointer produced each line so runner ConfigErrors map back to fields.
class RunnerConfigText {
public:
    void section(std::string_view header, const std::string& pointer) { line("[" + std::string(header) + "]", pointer); }
    void entry(std::string_view key, std::string_view value, const std::string& pointer) {
        line(std::string(key) + " = " + std::string(value), pointer);
    }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] std::string pointer_for_line(std::size_t line_number) const {
        if (line_number == 0 || line_number > line_pointers_.size()) return "/config";
        return line_pointers_[line_number - 1];
    }

private:
    void line(const std::string& content, const std::string& pointer) {
        text_ += content;
        text_ += '\n';
        line_pointers_.push_back(pointer);
    }
    std::string text_;
    std::vector<std::string> line_pointers_;
};

struct ModelParameterRule {
    std::string_view api_name;
    std::string_view unit;
    std::string_view runner_key;
    enum class Conversion { Integer, NanosecondsToMilliseconds, NanosecondsDuration, RatioToPartsPerMillion } conversion;
};

constexpr ModelParameterRule kMarketMakerParameterRules[] = {
    {"levels", "count", "levels", ModelParameterRule::Conversion::Integer},
    {"quoteSizeShares", "shares", "size", ModelParameterRule::Conversion::Integer},
    {"spreadTicks", "ticks", "spread_ticks", ModelParameterRule::Conversion::Integer},
    {"fairValueStepIntervalNs", "nanoseconds", "fair_value_step_ms", ModelParameterRule::Conversion::NanosecondsToMilliseconds},
    {"fairValueStepTicks", "ticks", "fair_value_step_ticks", ModelParameterRule::Conversion::Integer},
    {"requoteIntervalNs", "nanoseconds", "requote_ms", ModelParameterRule::Conversion::NanosecondsToMilliseconds},
    {"inventorySkewTicksPerUnit", "ticks", "skew_ticks_per_unit", ModelParameterRule::Conversion::Integer},
    {"timerIntervalNs", "nanoseconds", "timer_interval", ModelParameterRule::Conversion::NanosecondsDuration},
};

constexpr ModelParameterRule kNoiseTakerParameterRules[] = {
    {"meanInterarrivalNs", "nanoseconds", "mean_interval_ms", ModelParameterRule::Conversion::NanosecondsToMilliseconds},
    {"minimumOrderShares", "shares", "min_size", ModelParameterRule::Conversion::Integer},
    {"maximumOrderShares", "shares", "max_size", ModelParameterRule::Conversion::Integer},
    {"crossTicks", "ticks", "cross_ticks", ModelParameterRule::Conversion::Integer},
    {"limitOrderProbability", "ratio", "limit_ppm", ModelParameterRule::Conversion::RatioToPartsPerMillion},
    {"limitOrderLifetimeNs", "nanoseconds", "limit_lifetime_ms", ModelParameterRule::Conversion::NanosecondsToMilliseconds},
    {"timerIntervalNs", "nanoseconds", "timer_interval", ModelParameterRule::Conversion::NanosecondsDuration},
};

// Converts one model parameter value to runner text; nullopt with `problem` set otherwise.
std::optional<std::string> convert_model_parameter(const ModelParameterRule& rule, std::string_view value, std::string& problem) {
    if (rule.conversion == ModelParameterRule::Conversion::RatioToPartsPerMillion) {
        const auto ratio = parse_scaled_decimal(value);
        if (!ratio || ratio->units < 0 || ratio->fraction_digits > 6) {
            problem = "must be a ratio between 0 and 1 with at most 6 decimals";
            return std::nullopt;
        }
        std::int64_t parts_per_million = ratio->units;
        for (int digits = ratio->fraction_digits; digits < 6; ++digits) parts_per_million *= 10;
        if (parts_per_million > 1'000'000) {
            problem = "must be a ratio between 0 and 1 with at most 6 decimals";
            return std::nullopt;
        }
        return std::to_string(parts_per_million);
    }
    std::uint64_t integer = 0;
    if (!parse_unsigned_integer_text(value, integer)) {
        problem = "must be a non-negative integer";
        return std::nullopt;
    }
    switch (rule.conversion) {
        case ModelParameterRule::Conversion::Integer: return std::to_string(integer);
        case ModelParameterRule::Conversion::NanosecondsDuration: return std::to_string(integer) + "ns";
        case ModelParameterRule::Conversion::NanosecondsToMilliseconds:
            if (integer % 1'000'000 != 0) {
                problem = "must be a whole number of milliseconds (a multiple of 1000000 ns)";
                return std::nullopt;
            }
            return std::to_string(integer / 1'000'000);
        case ModelParameterRule::Conversion::RatioToPartsPerMillion: break;
    }
    return std::nullopt;
}

std::uint64_t runner_event_budget(const research::ExperimentConfig& config) {
    const long double per_event = (static_cast<long double>(config.risk.max_open_orders) + 1) *
                                  static_cast<long double>(config.risk.max_order_qty) *
                                  (static_cast<long double>(config.instrument.max_price) + kRunnerMaxFeePriceUnits);
    if (per_event <= 0) return kRunnerDefaultMaxSimulationEvents;
    const long double allowed = std::floor(static_cast<long double>(std::numeric_limits<std::int64_t>::max()) / 4 / per_event * 0.999L);
    if (allowed < 1) return 1;
    return allowed >= static_cast<long double>(kRunnerDefaultMaxSimulationEvents) ? kRunnerDefaultMaxSimulationEvents
                                                                                   : static_cast<std::uint64_t>(allowed);
}

void stable_serialize_into(const JsonValue& value, std::string& out) {
    switch (value.kind) {
        case Kind::Array:
            out += '[';
            for (std::size_t index = 0; index < value.items.size(); ++index) {
                if (index != 0) out += ',';
                stable_serialize_into(value.items[index], out);
            }
            out += ']';
            return;
        case Kind::Object: {
            std::vector<const research::JsonMember*> sorted;
            for (const auto& member : value.members) sorted.push_back(&member);
            std::stable_sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) { return left->name < right->name; });
            out += '{';
            for (std::size_t index = 0; index < sorted.size(); ++index) {
                if (index != 0) out += ',';
                out += research::json_escape(sorted[index]->name);
                out += ':';
                stable_serialize_into(sorted[index]->value, out);
            }
            out += '}';
            return;
        }
        default: out += serialize_json_value(value); return;
    }
}

}  // namespace

std::string stable_serialize_json_value(const JsonValue& value) {
    std::string out;
    stable_serialize_into(value, out);
    return out;
}

std::string configuration_digest_hex(const JsonValue& config) { return sha256_hex(stable_serialize_json_value(config)); }

ConfigTranslationResult translate_experiment_config(const JsonValue& config, std::string_view pointer_prefix) {
    ConfigTranslationResult result;
    ShapeReader shape(result.shape_issues);
    const std::string root(pointer_prefix);
    auto semantic = [&](const std::string& pointer, std::string issue) { result.semantic_issues.push_back({pointer, std::move(issue)}); };

    if (!shape.object(&config, root,
                      {"marketWorld", "instrument", "scenario", "strategy", "seedSet", "latency", "fees", "risk", "initialCash", "markConvention",
                       "markoutHorizonsNs", "bounds"}))
        return result;

    TranslatedExperimentConfig translated;
    RunnerConfigText runner_text;

    // ---- market world -------------------------------------------------------
    const std::string world_pointer = child_pointer(root, "marketWorld");
    if (const auto world = shape.enumeration(config.find("marketWorld"), world_pointer,
                                             {"interactive_synthetic", "historical_reconstruction", "historical_counterfactual_replay"})) {
        translated.market_world = *world;
        if (*world != kExecutableMarketWorld) {
            result.market_world_supported = false;
            semantic(world_pointer, "only interactive_synthetic is executable in v1");
        }
    }

    // ---- seeds and bounds (needed for [experiment]) -------------------------
    const std::string seeds_pointer = child_pointer(root, "seedSet");
    const JsonValue* seed_set = config.find("seedSet");
    if (shape.object(seed_set, seeds_pointer, {"masterSeeds"})) {
        const std::string list_pointer = child_pointer(seeds_pointer, "masterSeeds");
        if (const JsonValue* seeds = shape.array(seed_set->find("masterSeeds"), list_pointer, 1, 256)) {
            std::set<std::uint64_t> unique_seeds;
            for (std::size_t index = 0; index < seeds->items.size(); ++index) {
                const auto seed = shape.unsigned_integer_text(&seeds->items[index], index_pointer(list_pointer, index));
                if (!seed) continue;
                if (!unique_seeds.insert(*seed).second) shape.add(index_pointer(list_pointer, index), "repeats an earlier seed");
                translated.master_seeds.push_back(*seed);
            }
        }
    }

    const std::string bounds_pointer = child_pointer(root, "bounds");
    const JsonValue* bounds = config.find("bounds");
    std::optional<std::uint64_t> end_time_ns;
    if (shape.object(bounds, bounds_pointer, {"endSimulationTimeNs", "maxCanonicalEventCount", "wallClockTimeLimitSeconds"})) {
        end_time_ns = shape.unsigned_integer_text(bounds->find("endSimulationTimeNs"), child_pointer(bounds_pointer, "endSimulationTimeNs"));
        if (const auto maximum = shape.unsigned_integer_text(bounds->find("maxCanonicalEventCount"), child_pointer(bounds_pointer, "maxCanonicalEventCount"))) {
            translated.max_canonical_event_count = *maximum;
            if (*maximum == 0) semantic(child_pointer(bounds_pointer, "maxCanonicalEventCount"), "must be at least 1");
        }
        if (const auto seconds = shape.integer_number(bounds->find("wallClockTimeLimitSeconds"), child_pointer(bounds_pointer, "wallClockTimeLimitSeconds"), 1, 86400))
            translated.wall_clock_time_limit_seconds = static_cast<std::uint32_t>(*seconds);
    }

    runner_text.section("experiment", root);
    runner_text.entry("name", "api_experiment", root);
    if (!translated.master_seeds.empty()) runner_text.entry("seed", std::to_string(translated.master_seeds.front()), seeds_pointer);
    if (end_time_ns) runner_text.entry("duration", std::to_string(*end_time_ns) + "ns", child_pointer(bounds_pointer, "endSimulationTimeNs"));

    // ---- instrument --------------------------------------------------------
    const std::string instrument_pointer = child_pointer(root, "instrument");
    const JsonValue* instrument = config.find("instrument");
    if (shape.object(instrument, instrument_pointer,
                     {"symbol", "tickSize", "lotSize", "maxOrderQuantity", "minPrice", "maxPrice", "referencePrice", "contractMultiplier"})) {
        runner_text.section("instrument", instrument_pointer);
        const auto read_into = [&](std::string_view api_name, std::string_view runner_key, const std::function<bool(std::string_view)>& valid,
                                   std::string_view expectation) {
            const std::string pointer = child_pointer(instrument_pointer, api_name);
            if (const auto value = shape.text(instrument->find(api_name), pointer, valid, expectation)) runner_text.entry(runner_key, *value, pointer);
        };
        read_into("symbol", "symbol", matches_symbol_pattern, "1 to 8 characters of A-Z, 0-9 or '.'");
        read_into("tickSize", "tick_size", is_price_decimal, "a price with exactly 4 decimals");
        read_into("lotSize", "lot_size", is_unsigned_integer, "an unsigned integer string");
        read_into("maxOrderQuantity", "max_order_qty", is_unsigned_integer, "an unsigned integer string");
        read_into("minPrice", "min_price", is_price_decimal, "a price with exactly 4 decimals");
        read_into("maxPrice", "max_price", is_price_decimal, "a price with exactly 4 decimals");
        read_into("referencePrice", "reference_price", is_price_decimal, "a price with exactly 4 decimals");
        (void)shape.enumeration(instrument->find("contractMultiplier"), child_pointer(instrument_pointer, "contractMultiplier"), {"1"});
    }

    // ---- latency -----------------------------------------------------------
    const std::string latency_pointer = child_pointer(root, "latency");
    const JsonValue* latency = config.find("latency");
    if (shape.object(latency, latency_pointer,
                     {"latencyModel", "feedLatencyNs", "orderEntryLatencyNs", "acknowledgmentLatencyNs", "cancelLatencyNs", "jitterNs"},
                     {"strategyComputeNs", "orderChannelFifo"})) {
        runner_text.section("latency", latency_pointer);
        const auto model = shape.enumeration(latency->find("latencyModel"), child_pointer(latency_pointer, "latencyModel"), {"constant", "uniform_jitter"});
        const auto duration_into = [&](std::string_view api_name, std::string_view runner_key) -> std::optional<std::uint64_t> {
            const std::string pointer = child_pointer(latency_pointer, api_name);
            const auto value = shape.unsigned_integer_text(latency->find(api_name), pointer);
            if (value) runner_text.entry(runner_key, std::to_string(*value) + "ns", pointer);
            return value;
        };
        duration_into("feedLatencyNs", "feed");
        duration_into("orderEntryLatencyNs", "order");
        duration_into("acknowledgmentLatencyNs", "ack");
        duration_into("cancelLatencyNs", "cancel");
        if (latency->find("strategyComputeNs") != nullptr) duration_into("strategyComputeNs", "strategy_compute");
        const std::string jitter_pointer = child_pointer(latency_pointer, "jitterNs");
        if (const auto jitter = shape.unsigned_integer_text(latency->find("jitterNs"), jitter_pointer)) {
            if (model && *model == "constant" && *jitter != 0) semantic(jitter_pointer, "must be \"0\" for the constant latency model");
            for (const std::string_view key : {"feed_jitter", "order_jitter", "cancel_jitter", "ack_jitter"})
                runner_text.entry(key, std::to_string(*jitter) + "ns", jitter_pointer);
        }
        const std::string fifo_pointer = child_pointer(latency_pointer, "orderChannelFifo");
        if (const auto fifo = shape.boolean(latency->find("orderChannelFifo"), fifo_pointer))
            runner_text.entry("order_channel_fifo", *fifo ? "true" : "false", fifo_pointer);
    }

    // ---- fees ----------------------------------------------------------------
    const std::string fees_pointer = child_pointer(root, "fees");
    const JsonValue* fees = config.find("fees");
    if (shape.object(fees, fees_pointer, {"feeScheduleId", "makerFeePerShare", "takerFeePerShare"})) {
        runner_text.section("fees", fees_pointer);
        (void)shape.text(fees->find("feeScheduleId"), child_pointer(fees_pointer, "feeScheduleId"), nullptr, "a string");
        for (const auto& [api_name, runner_key] : {std::pair<std::string_view, std::string_view>{"makerFeePerShare", "maker_fee_per_share"},
                                                   {"takerFeePerShare", "taker_fee_per_share"}}) {
            const std::string pointer = child_pointer(fees_pointer, api_name);
            if (const auto value = shape.text(fees->find(api_name), pointer, is_signed_currency_decimal, "a signed amount with exactly 4 decimals"))
                runner_text.entry(runner_key, *value, pointer);
        }
    }

    // ---- risk ----------------------------------------------------------------
    const std::string risk_pointer = child_pointer(root, "risk");
    const JsonValue* risk = config.find("risk");
    if (shape.object(risk, risk_pointer, {"maxAbsolutePositionShares", "maxOpenOrders", "maxOrderQuantity", "onBreach"})) {
        runner_text.section("risk", risk_pointer);
        const std::string position_pointer = child_pointer(risk_pointer, "maxAbsolutePositionShares");
        if (const auto position = shape.unsigned_integer_text(risk->find("maxAbsolutePositionShares"), position_pointer)) {
            runner_text.entry("max_long_position", std::to_string(*position), position_pointer);
            runner_text.entry("max_short_position", std::to_string(*position), position_pointer);
        }
        const std::string open_pointer = child_pointer(risk_pointer, "maxOpenOrders");
        if (const auto open_orders = shape.unsigned_integer_text(risk->find("maxOpenOrders"), open_pointer))
            runner_text.entry("max_open_orders", std::to_string(*open_orders), open_pointer);
        const std::string quantity_pointer = child_pointer(risk_pointer, "maxOrderQuantity");
        if (const auto quantity = shape.unsigned_integer_text(risk->find("maxOrderQuantity"), quantity_pointer))
            runner_text.entry("max_order_qty", std::to_string(*quantity), quantity_pointer);
        const std::string breach_pointer = child_pointer(risk_pointer, "onBreach");
        if (const auto breach = shape.enumeration(risk->find("onBreach"), breach_pointer, {"block_orders_that_increase_exposure", "cancel_all_and_stop_strategy"}))
            if (*breach != "block_orders_that_increase_exposure")
                semantic(breach_pointer, "the runner implements only block_orders_that_increase_exposure");
    }

    // ---- accounting ----------------------------------------------------------
    runner_text.section("accounting", root);
    const std::string cash_pointer = child_pointer(root, "initialCash");
    if (const auto cash = shape.text(config.find("initialCash"), cash_pointer, is_signed_currency_decimal, "an amount with exactly 4 decimals"))
        runner_text.entry("initial_cash", *cash, cash_pointer);
    const std::string mark_pointer = child_pointer(root, "markConvention");
    if (const auto mark = shape.enumeration(config.find("markConvention"), mark_pointer, {"exchange_mid", "strategy_visible_mid", "last_trade_price"})) {
        translated.mark_convention = *mark;
        if (*mark == "strategy_visible_mid") semantic(mark_pointer, "strategy_visible_mid is not implemented by the runner; use exchange_mid or last_trade_price");
        else runner_text.entry("mark", *mark, mark_pointer);
    }

    // ---- metrics -------------------------------------------------------------
    const std::string horizons_pointer = child_pointer(root, "markoutHorizonsNs");
    if (const JsonValue* horizons = shape.array(config.find("markoutHorizonsNs"), horizons_pointer, 1, 8)) {
        std::string joined;
        for (std::size_t index = 0; index < horizons->items.size(); ++index) {
            const auto horizon = shape.unsigned_integer_text(&horizons->items[index], index_pointer(horizons_pointer, index));
            if (!horizon) continue;
            if (*horizon == 0) semantic(index_pointer(horizons_pointer, index), "must be positive");
            if (!joined.empty()) joined += ", ";
            joined += std::to_string(*horizon) + "ns";
        }
        runner_text.section("metrics", horizons_pointer);
        if (!joined.empty()) runner_text.entry("markout_horizons", joined, horizons_pointer);
    }

    // ---- strategy ------------------------------------------------------------
    const std::string strategy_pointer = child_pointer(root, "strategy");
    const JsonValue* strategy = config.find("strategy");
    if (strategy != nullptr) {
        const std::string kind_pointer = child_pointer(strategy_pointer, "strategyKind");
        const auto kind = shape.enumeration(member_of(strategy, "strategyKind"), kind_pointer,
                                            {"fixed_spread_market_maker", "inventory_aware_market_maker", "twap_execution"});
        if (strategy->kind != Kind::Object) {
            shape.add(strategy_pointer, "must be an object");
        } else if (!kind && strategy->find("strategyKind") == nullptr) {
            shape.add(kind_pointer, "is required");
        } else if (kind && *kind == "twap_execution") {
            translated.strategy_kind = *kind;
            (void)shape.object(strategy, strategy_pointer, {"strategyKind", "parentSide", "parentQuantity", "sliceIntervalNs", "sliceOrderType"});
            semantic(kind_pointer, "twap_execution is not executable through this service yet; executable kinds are listed by GET /v1/service");
        } else if (kind) {
            translated.strategy_kind = *kind;
            const bool inventory_aware = *kind == "inventory_aware_market_maker";
            const bool shaped = inventory_aware
                ? shape.object(strategy, strategy_pointer,
                               {"strategyKind", "halfSpreadTicks", "quoteQuantity", "requoteIntervalNs", "requoteThresholdTicks", "postOnly",
                                "inventorySkewTicksPerUnit", "inventorySkewUnitShares", "scaleSizeWithInventory"})
                : shape.object(strategy, strategy_pointer,
                               {"strategyKind", "halfSpreadTicks", "quoteQuantity", "requoteIntervalNs", "requoteThresholdTicks", "postOnly"});
            if (shaped) {
                runner_text.section("strategy", strategy_pointer);
                runner_text.entry("kind", *kind, kind_pointer);
                const auto integer_into = [&](std::string_view api_name, std::string_view runner_key, std::string_view suffix) {
                    const std::string pointer = child_pointer(strategy_pointer, api_name);
                    if (const auto value = shape.unsigned_integer_text(strategy->find(api_name), pointer))
                        runner_text.entry(runner_key, std::to_string(*value) + std::string(suffix), pointer);
                };
                const auto boolean_into = [&](std::string_view api_name, std::string_view runner_key) {
                    const std::string pointer = child_pointer(strategy_pointer, api_name);
                    if (const auto value = shape.boolean(strategy->find(api_name), pointer)) runner_text.entry(runner_key, *value ? "true" : "false", pointer);
                };
                integer_into("halfSpreadTicks", "half_spread_ticks", "");
                integer_into("quoteQuantity", "quote_size", "");
                integer_into("requoteIntervalNs", "requote_interval", "ns");
                integer_into("requoteThresholdTicks", "requote_threshold_ticks", "");
                boolean_into("postOnly", "post_only");
                if (inventory_aware) {
                    integer_into("inventorySkewTicksPerUnit", "skew_ticks_per_unit", "");
                    integer_into("inventorySkewUnitShares", "skew_unit", "");
                    boolean_into("scaleSizeWithInventory", "scale_size_with_inventory");
                }
            }
        }
    }

    // ---- scenario: background agents ----------------------------------------
    const std::string scenario_pointer = child_pointer(root, "scenario");
    const JsonValue* scenario = config.find("scenario");
    if (shape.object(scenario, scenario_pointer, {"scenarioId", "description", "backgroundAgents"})) {
        (void)shape.text(scenario->find("scenarioId"), child_pointer(scenario_pointer, "scenarioId"), matches_identifier_pattern,
                         "2 to 64 characters of a-z, 0-9, '_', '.' or '-'");
        (void)shape.text(scenario->find("description"), child_pointer(scenario_pointer, "description"), nullptr, "a string");
        const std::string agents_pointer = child_pointer(scenario_pointer, "backgroundAgents");
        if (const JsonValue* agents = shape.array(scenario->find("backgroundAgents"), agents_pointer, 0, 64)) {
            std::map<std::string, std::size_t> agents_by_kind;
            std::size_t total_agents = 0;
            for (std::size_t agent_index = 0; agent_index < agents->items.size(); ++agent_index) {
                const JsonValue& agent = agents->items[agent_index];
                const std::string agent_pointer = index_pointer(agents_pointer, agent_index);
                if (!shape.object(&agent, agent_pointer, {"agentKind", "agentCount", "parameters"})) continue;
                const auto agent_kind = shape.enumeration(agent.find("agentKind"), child_pointer(agent_pointer, "agentKind"), {"market_maker", "noise_taker"});
                const auto agent_count = shape.integer_number(agent.find("agentCount"), child_pointer(agent_pointer, "agentCount"), 0, 64);
                const std::string parameters_pointer = child_pointer(agent_pointer, "parameters");
                const JsonValue* parameters = shape.array(agent.find("parameters"), parameters_pointer, 0, 64);
                std::vector<std::pair<std::string, std::string>> runner_entries;
                std::vector<std::string> runner_entry_pointers;
                if (parameters != nullptr) {
                    std::set<std::string> seen_names;
                    for (std::size_t parameter_index = 0; parameter_index < parameters->items.size(); ++parameter_index) {
                        const JsonValue& parameter = parameters->items[parameter_index];
                        const std::string parameter_pointer = index_pointer(parameters_pointer, parameter_index);
                        if (!shape.object(&parameter, parameter_pointer, {"name", "value", "unit", "description"})) continue;
                        const auto name = shape.text(parameter.find("name"), child_pointer(parameter_pointer, "name"),
                                                     [](std::string_view candidate) {
                                                         if (candidate.empty() || candidate.front() < 'a' || candidate.front() > 'z') return false;
                                                         return std::all_of(candidate.begin(), candidate.end(), [](char character) {
                                                             return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                                                                    (character >= '0' && character <= '9');
                                                         });
                                                     },
                                                     "a camelCase name");
                        const auto value = shape.text(parameter.find("value"), child_pointer(parameter_pointer, "value"), is_exact_decimal, "an exact decimal string");
                        const auto unit = shape.enumeration(parameter.find("unit"), child_pointer(parameter_pointer, "unit"),
                                                            {"ticks", "shares", "nanoseconds", "ratio", "count", "currency"});
                        (void)shape.text(parameter.find("description"), child_pointer(parameter_pointer, "description"), nullptr, "a string");
                        if (!name || !value || !unit || !agent_kind) continue;
                        if (!seen_names.insert(*name).second) {
                            semantic(child_pointer(parameter_pointer, "name"), "repeats an earlier parameter");
                            continue;
                        }
                        const ModelParameterRule* rule = nullptr;
                        if (*agent_kind == "market_maker") {
                            for (const auto& candidate : kMarketMakerParameterRules)
                                if (candidate.api_name == *name) rule = &candidate;
                        } else {
                            for (const auto& candidate : kNoiseTakerParameterRules)
                                if (candidate.api_name == *name) rule = &candidate;
                        }
                        if (rule == nullptr) {
                            std::string known;
                            if (*agent_kind == "market_maker") {
                                for (const auto& candidate : kMarketMakerParameterRules) known += " " + std::string(candidate.api_name);
                            } else {
                                for (const auto& candidate : kNoiseTakerParameterRules) known += " " + std::string(candidate.api_name);
                            }
                            semantic(child_pointer(parameter_pointer, "name"), "is not a parameter of the runner's " + *agent_kind + " model; known:" + known);
                            continue;
                        }
                        if (*unit != rule->unit) {
                            semantic(child_pointer(parameter_pointer, "unit"), "must be " + std::string(rule->unit) + " for " + *name);
                            continue;
                        }
                        std::string problem;
                        const auto runner_value = convert_model_parameter(*rule, *value, problem);
                        if (!runner_value) {
                            semantic(child_pointer(parameter_pointer, "value"), problem);
                            continue;
                        }
                        runner_entries.emplace_back(std::string(rule->runner_key), *runner_value);
                        runner_entry_pointers.push_back(child_pointer(parameter_pointer, "value"));
                    }
                }
                if (!agent_kind || !agent_count) continue;
                total_agents += static_cast<std::size_t>(*agent_count);
                for (std::int64_t copy = 0; copy < *agent_count; ++copy) {
                    const std::size_t ordinal = ++agents_by_kind[*agent_kind];
                    runner_text.section("agent " + *agent_kind + " " + *agent_kind + "_" + std::to_string(ordinal), agent_pointer);
                    for (std::size_t entry = 0; entry < runner_entries.size(); ++entry)
                        runner_text.entry(runner_entries[entry].first, runner_entries[entry].second, runner_entry_pointers[entry]);
                }
            }
            if (total_agents > research::kMaxBackgroundAgents)
                semantic(agents_pointer, "the runner supports at most " + std::to_string(research::kMaxBackgroundAgents) + " background agents in total");
        }
    }

    if (!result.shape_issues.empty() || !result.semantic_issues.empty()) return result;

    // The runner validates everything it executes (prices on the tick grid,
    // positive sizes, bot parameter ranges, ...). Its first error is mapped
    // back to the field that produced the offending line.
    try {
        research::ExperimentConfig parsed = research::parse_experiment_config(runner_text.text());
        translated.runner_max_simulation_events = runner_event_budget(parsed);
        std::string text_with_budget = runner_text.text();
        const std::string marker = "name = api_experiment\n";
        const std::size_t insert_at = text_with_budget.find(marker) + marker.size();
        text_with_budget.insert(insert_at, "max_simulation_events = " + std::to_string(translated.runner_max_simulation_events) + "\n");
        translated.runner_config = research::parse_experiment_config(text_with_budget);
        translated.runner_config_text = std::move(text_with_budget);
    } catch (const research::ConfigError& error) {
        std::string message = error.what();
        const std::string line_prefix = "line " + std::to_string(error.line()) + ": ";
        if (error.line() != 0 && message.rfind(line_prefix, 0) == 0) message.erase(0, line_prefix.size());
        semantic(runner_text.pointer_for_line(error.line()), "rejected by the research runner: " + message);
        return result;
    } catch (const std::exception& error) {
        semantic(root, std::string("rejected by the research runner: ") + error.what());
        return result;
    }

    translated.config_json = serialize_json_value(config);
    translated.config_digest_hex = configuration_digest_hex(config);
    result.translated = std::move(translated);
    return result;
}

ExperimentCreateFields read_experiment_create_request(const JsonValue& body, std::vector<FieldIssue>& issues) {
    ExperimentCreateFields fields;
    ShapeReader shape(issues);
    if (!shape.object(&body, "", {"name", "derivedFromBaselineId", "config"}, {"description", "parentExperimentId"})) return fields;
    if (const auto name = shape.text(body.find("name"), "/name", [](std::string_view text) { return !text.empty() && text.size() <= 120; },
                                     "1 to 120 characters"))
        fields.name = *name;
    if (const auto description = shape.text(body.find("description"), "/description", [](std::string_view text) { return text.size() <= 2000; },
                                            "at most 2000 characters"))
        fields.description = *description;
    if (const auto baseline = shape.text(body.find("derivedFromBaselineId"), "/derivedFromBaselineId", matches_identifier_pattern, "a baseline id"))
        fields.derived_from_baseline_id = *baseline;
    if (const auto parent = shape.text(body.find("parentExperimentId"), "/parentExperimentId", matches_experiment_id_pattern, "an experiment id"))
        fields.parent_experiment_id = *parent;
    fields.config = body.find("config");
    return fields;
}

// ---- Baselines -----------------------------------------------------------------

namespace {

std::string baseline_config_json(int half_spread_ticks) {
    // Mirrors config/research/fixed_spread_narrow.conf (and _wide) with the
    // API's single jitter value applied to every latency channel.
    return R"({"marketWorld":"interactive_synthetic",)"
           R"("instrument":{"symbol":"SYNTH","tickSize":"0.0100","lotSize":"1","maxOrderQuantity":"100000","minPrice":"50.0000","maxPrice":"150.0000","referencePrice":"100.0000","contractMultiplier":"1"},)"
           R"("scenario":{"scenarioId":"synthetic.mm-and-noise-taker.v1","description":"One background market maker quoting three levels around a stepping fair value, and one noise taker sending crossing orders (runner models exsim::sim::bots).","backgroundAgents":[)"
           R"({"agentKind":"market_maker","agentCount":1,"parameters":[)"
           R"({"name":"levels","value":"3","unit":"count","description":"Price levels quoted per side."},)"
           R"({"name":"quoteSizeShares","value":"100","unit":"shares","description":"Quantity per quoted level."},)"
           R"({"name":"spreadTicks","value":"1","unit":"ticks","description":"Half spread around the fair value."},)"
           R"({"name":"fairValueStepIntervalNs","value":"500000000","unit":"nanoseconds","description":"Interval between fair value random-walk steps."},)"
           R"({"name":"fairValueStepTicks","value":"1","unit":"ticks","description":"Size of one fair value step."},)"
           R"({"name":"requoteIntervalNs","value":"100000000","unit":"nanoseconds","description":"Requote period."}]},)"
           R"({"agentKind":"noise_taker","agentCount":1,"parameters":[)"
           R"({"name":"meanInterarrivalNs","value":"20000000","unit":"nanoseconds","description":"Mean exponential time between orders."},)"
           R"({"name":"minimumOrderShares","value":"1","unit":"shares","description":"Smallest order."},)"
           R"({"name":"maximumOrderShares","value":"200","unit":"shares","description":"Largest order."},)"
           R"({"name":"crossTicks","value":"2","unit":"ticks","description":"Ticks through the touch for crossing orders."},)"
           R"({"name":"limitOrderProbability","value":"0.1","unit":"ratio","description":"Probability an order is a resting limit order."},)"
           R"({"name":"limitOrderLifetimeNs","value":"1000000000","unit":"nanoseconds","description":"Lifetime of a resting limit order."}]}]},)"
           R"("strategy":{"strategyKind":"fixed_spread_market_maker","halfSpreadTicks":")" +
           std::to_string(half_spread_ticks) +
           R"(","quoteQuantity":"100","requoteIntervalNs":"50000000","requoteThresholdTicks":"1","postOnly":true},)"
           R"("seedSet":{"masterSeeds":["1","2","3","4","5"]},)"
           R"("latency":{"latencyModel":"uniform_jitter","feedLatencyNs":"50000","orderEntryLatencyNs":"100000","acknowledgmentLatencyNs":"100000","cancelLatencyNs":"80000","jitterNs":"20000","strategyComputeNs":"5000","orderChannelFifo":true},)"
           R"("fees":{"feeScheduleId":"maker-rebate-20-taker-30","makerFeePerShare":"-0.0020","takerFeePerShare":"0.0030"},)"
           R"("risk":{"maxAbsolutePositionShares":"500","maxOpenOrders":"8","maxOrderQuantity":"200","onBreach":"block_orders_that_increase_exposure"},)"
           R"("initialCash":"1000000.0000","markConvention":"exchange_mid","markoutHorizonsNs":["100000000","1000000000","5000000000"],)"
           R"("bounds":{"endSimulationTimeNs":"60000000000","maxCanonicalEventCount":"5000000","wallClockTimeLimitSeconds":600}})";
}

}  // namespace

const std::vector<BaselineConfiguration>& baseline_configurations() {
    static const std::vector<BaselineConfiguration> baselines = {
        {"fixed-spread-2-ticks", "Fixed spread, 2-tick width",
         "Quotes one tick either side of the strategy-visible mid (runner config fixed_spread_narrow).", baseline_config_json(1)},
        {"fixed-spread-4-ticks", "Fixed spread, 4-tick width",
         "Quotes two ticks either side of the strategy-visible mid (runner config fixed_spread_wide).", baseline_config_json(2)},
    };
    return baselines;
}

const BaselineConfiguration* find_baseline(std::string_view baseline_id) {
    for (const auto& baseline : baseline_configurations())
        if (baseline.baseline_id == baseline_id) return &baseline;
    return nullptr;
}

// ---- Assumptions ---------------------------------------------------------------

namespace {

std::string category_for_top_level_key(std::string_view key) {
    if (key == "marketWorld") return "market_world";
    if (key == "seedSet") return "seeds";
    if (key == "initialCash" || key == "markConvention" || key == "markoutHorizonsNs") return "accounting";
    return std::string(key);  // instrument, scenario, strategy, latency, fees, risk, bounds
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::optional<std::string> unit_from_field_name(std::string_view name) {
    if (ends_with(name, "Ns") || ends_with(name, "NsList")) return "ns";
    if (ends_with(name, "Ticks")) return "ticks";
    if (ends_with(name, "Seconds")) return "s";
    if (ends_with(name, "FeePerShare") || ends_with(name, "Price") || name == "tickSize") return "currency/share";
    if (name == "initialCash") return "currency";
    if (ends_with(name, "Shares") || ends_with(name, "Quantity") || name == "lotSize") return "shares";
    return std::nullopt;
}

std::string scalar_display(const JsonValue& value) {
    switch (value.kind) {
        case Kind::String: return value.text;
        case Kind::Number: return value.text;
        case Kind::Boolean: return value.boolean ? "true" : "false";
        case Kind::Null: return "null";
        default: return serialize_json_value(value);
    }
}

std::string parameter_unit_label(std::string_view unit) { return unit == "nanoseconds" ? "ns" : std::string(unit); }

void visit_assumption(const JsonValue& value, std::vector<std::string>& path, const std::string& category, std::vector<AssumptionLine>& lines) {
    std::string pointer;
    std::string label;
    for (std::size_t index = 0; index < path.size(); ++index) {
        pointer += "/" + escape_pointer_token(path[index]);
        label += (index == 0 ? "" : ".") + path[index];
    }
    const std::string& field_name = path.back();
    if (value.kind == Kind::Array) {
        const bool all_scalar = std::all_of(value.items.begin(), value.items.end(),
                                            [](const JsonValue& item) { return item.kind != Kind::Object && item.kind != Kind::Array; });
        if (all_scalar) {
            std::string joined;
            for (std::size_t index = 0; index < value.items.size(); ++index) joined += (index == 0 ? "" : ", ") + scalar_display(value.items[index]);
            lines.push_back({category, pointer, label, joined, unit_from_field_name(field_name)});
            return;
        }
        for (std::size_t index = 0; index < value.items.size(); ++index) {
            path.push_back(std::to_string(index));
            visit_assumption(value.items[index], path, category, lines);
            path.pop_back();
        }
        return;
    }
    if (value.kind == Kind::Object) {
        const JsonValue* agent_kind = value.find("agentKind");
        const JsonValue* parameters = value.find("parameters");
        if (agent_kind != nullptr && parameters != nullptr) {
            const JsonValue* count = value.find("agentCount");
            lines.push_back({category, pointer + "/agentCount", scalar_display(*agent_kind) + ".agentCount", count ? scalar_display(*count) : "", "count"});
            for (std::size_t index = 0; index < parameters->items.size(); ++index) {
                const JsonValue& parameter = parameters->items[index];
                const JsonValue* name = parameter.find("name");
                const JsonValue* parameter_value = parameter.find("value");
                const JsonValue* unit = parameter.find("unit");
                lines.push_back({category, pointer + "/parameters/" + std::to_string(index) + "/value",
                                 scalar_display(*agent_kind) + "." + (name ? scalar_display(*name) : ""),
                                 parameter_value ? scalar_display(*parameter_value) : "",
                                 unit ? std::optional<std::string>(parameter_unit_label(scalar_display(*unit))) : std::nullopt});
            }
            return;
        }
        for (const auto& member : value.members) {
            path.push_back(member.name);
            visit_assumption(member.value, path, category, lines);
            path.pop_back();
        }
        return;
    }
    lines.push_back({category, pointer, label, scalar_display(value), unit_from_field_name(field_name)});
}

}  // namespace

std::vector<AssumptionLine> flatten_experiment_config(const JsonValue& config) {
    std::vector<AssumptionLine> lines;
    if (config.kind != Kind::Object) return lines;
    for (const auto& member : config.members) {
        if (member.name == "scenario" && member.value.kind == Kind::Object) {
            if (const JsonValue* scenario_id = member.value.find("scenarioId"))
                lines.push_back({"scenario", "/scenario/scenarioId", "scenario.scenarioId", scalar_display(*scenario_id), std::nullopt});
            if (const JsonValue* agents = member.value.find("backgroundAgents"); agents != nullptr && agents->kind == Kind::Array) {
                for (std::size_t index = 0; index < agents->items.size(); ++index) {
                    std::vector<std::string> path = {"scenario", "backgroundAgents", std::to_string(index)};
                    visit_assumption(agents->items[index], path, "scenario", lines);
                }
            }
            continue;
        }
        std::vector<std::string> path = {member.name};
        visit_assumption(member.value, path, category_for_top_level_key(member.name), lines);
    }
    return lines;
}

std::vector<AssumptionDifference> list_assumption_differences(const JsonValue& baseline_config, const JsonValue& candidate_config) {
    const auto baseline_lines = flatten_experiment_config(baseline_config);
    const auto candidate_lines = flatten_experiment_config(candidate_config);
    std::map<std::string, const AssumptionLine*> baseline_by_path;
    std::map<std::string, const AssumptionLine*> candidate_by_path;
    for (const auto& line : baseline_lines) baseline_by_path.emplace(line.field_path, &line);
    for (const auto& line : candidate_lines) candidate_by_path.emplace(line.field_path, &line);
    std::vector<std::string> ordered_paths;
    std::set<std::string> seen;
    for (const auto& line : baseline_lines)
        if (seen.insert(line.field_path).second) ordered_paths.push_back(line.field_path);
    for (const auto& line : candidate_lines)
        if (seen.insert(line.field_path).second) ordered_paths.push_back(line.field_path);
    std::vector<AssumptionDifference> differences;
    for (const auto& path : ordered_paths) {
        const auto baseline_found = baseline_by_path.find(path);
        const auto candidate_found = candidate_by_path.find(path);
        const AssumptionLine* baseline_line = baseline_found == baseline_by_path.end() ? nullptr : baseline_found->second;
        const AssumptionLine* candidate_line = candidate_found == candidate_by_path.end() ? nullptr : candidate_found->second;
        if (baseline_line != nullptr && candidate_line != nullptr && baseline_line->value == candidate_line->value) continue;
        AssumptionDifference difference;
        difference.reference = candidate_line != nullptr ? *candidate_line : *baseline_line;
        difference.baseline_value = baseline_line != nullptr ? baseline_line->value : "(absent)";
        difference.candidate_value = candidate_line != nullptr ? candidate_line->value : "(absent)";
        differences.push_back(std::move(difference));
    }
    return differences;
}

std::uint64_t estimate_canonical_events_per_run(const research::ExperimentConfig& runner_config) {
    // Per simulated second: a market maker requote cancels and re-adds its
    // levels (about 6 events per level and side), a noise taker order is about
    // 5 events, and the strategy requote cycle about 16 events. A labelled
    // estimate only; the run reports the exact count.
    const long double seconds = static_cast<long double>(runner_config.limits.duration_ns) / 1e9L;
    long double events_per_second = 0;
    for (const auto& agent : runner_config.agents) {
        if (agent.kind == research::BackgroundAgentKind::MarketMaker) {
            const long double requotes = 1000.0L / std::max<std::uint32_t>(1, agent.market_maker.requote_ms);
            events_per_second += requotes * 12.0L * agent.market_maker.levels;
        } else {
            events_per_second += 1000.0L / std::max<std::uint32_t>(1, agent.noise_taker.mean_interval_ms) * 5.0L;
        }
    }
    const long double strategy_cycles = 1e9L / static_cast<long double>(std::max<Timestamp>(1, runner_config.strategy.quoting.requote_interval_ns));
    events_per_second += strategy_cycles * 16.0L;
    return static_cast<std::uint64_t>(std::llround(events_per_second * seconds));
}

double estimate_wall_clock_seconds_per_run(std::uint64_t canonical_events) {
    return static_cast<double>(canonical_events) / 1'500'000.0;  // measured order of magnitude on the baselines
}

}  // namespace exsim::api
