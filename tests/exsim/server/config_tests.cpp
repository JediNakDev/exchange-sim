#include "exsim/server/config.hpp"
#include "../../framework/test.hpp"

#include <algorithm>
#include <string>

using namespace exsim;
using namespace exsim::server;

namespace {
const char* kMinimal = R"(
[instrument AAPL]
tick_size = 0.01
min_price = 0.01
max_price = 1000
reference_price = 150.25

[user ALICE]
password = pw
session_id = 1
)";
}

TEST_CASE(server_config, prices) {
    Price p = 0;
    CHECK(parse_price("150.25", p));
    CHECK_EQ(p, 1'502'500);
    CHECK(parse_price("0.0001", p));
    CHECK_EQ(p, 1);
    CHECK(parse_price("7", p));
    CHECK_EQ(p, 70'000);
    CHECK(parse_price("-1.5", p));
    CHECK_EQ(p, -15'000);
    CHECK(parse_price(".5", p));
    CHECK_EQ(p, 5'000);
    CHECK(!parse_price("1.00001", p));
    CHECK(!parse_price("1.", p));
    CHECK(!parse_price("", p));
    CHECK(!parse_price("-", p));
    CHECK(!parse_price("1e3", p));
    CHECK(!parse_price("99999999999999999999", p));
    CHECK(!parse_price("922337203685478", p));  // * 10^4 overflows int64
    CHECK(format_price(1'502'500) == "150.25");
    CHECK(format_price(70'000) == "7");
    CHECK(format_price(1) == "0.0001");
    CHECK(format_price(-15'000) == "-1.5");
    CHECK(format_price(0) == "0");
}

TEST_CASE(server_config, price_int64_limits_and_transactional_overflow) {
    Price price = 0;
    REQUIRE(parse_price("922337203685477.5807", price));
    CHECK_EQ(price, INT64_MAX);
    REQUIRE(parse_price("-922337203685477.5808", price));
    CHECK_EQ(price, INT64_MIN);
    CHECK_EQ(format_price(price), "-922337203685477.5808");
    for (const auto text : {"922337203685477.5808", "-922337203685477.5809",
                            "-922337203685477.587.", "922337203685478", "--1"}) {
        price = 123;
        CHECK(!parse_price(text, price));
        CHECK_EQ(price, 123);
    }
}

TEST_CASE(server_config, engine_limit_errors_identify_the_source_line) {
    for (const auto key : {"max_instruments = 65536", "max_orders = 4294967296"}) {
        ServerConfig config;
        std::string error;
        CHECK(!parse_config(std::string("[engine]\n") + key + "\n" + kMinimal, config, error));
        CHECK(error.starts_with("2:"));
    }
}

TEST_CASE(server_config, minimal_uses_defaults) {
    ServerConfig cfg;
    std::string err;
    REQUIRE(parse_config(kMinimal, cfg, err));
    CHECK(err.empty());
    REQUIRE_EQ(cfg.instruments.size(), 1u);
    CHECK(cfg.instruments[0].symbol.view() == "AAPL");
    CHECK_EQ(cfg.instruments[0].tick_size, 100);
    CHECK_EQ(cfg.instruments[0].max_price, 10'000'000);
    CHECK_EQ(cfg.instruments[0].reference_price, 1'502'500);
    REQUIRE_EQ(cfg.users.size(), 1u);
    CHECK(cfg.users[0].protocol == Protocol::Ouch42);
    CHECK(cfg.users[0].cancel_on_disconnect);
    CHECK(cfg.exchange.name == "EXSIM");
    CHECK(cfg.find_user("ALICE") != nullptr);
    CHECK(cfg.find_user("BOB") == nullptr);
}

TEST_CASE(server_config, example_file_parses) {
    ServerConfig cfg;
    std::string err;
    const bool ok = load_config("config/exsim.conf", cfg, err);
    if (!ok) testfw::report_failure(__FILE__, __LINE__, err);
    REQUIRE(ok);
    CHECK_EQ(cfg.instruments.size(), 3u);
    CHECK_EQ(cfg.users.size(), 5u);
    CHECK_EQ(cfg.exchange.ouch_port, 15000);
    CHECK_EQ(cfg.market_data.port, 16000);
    CHECK(cfg.market_data.interface_address.empty());
    CHECK(cfg.instruments[0].initial_phase == Phase::PreOpen);
    const UserConfig* carol = cfg.find_user("CAROL");
    REQUIRE(carol != nullptr);
    CHECK(carol->protocol == Protocol::Fix44);
    CHECK(carol->stp == StpMode::CancelAggressor);
    CHECK(!carol->cancel_on_disconnect);
    CHECK(cfg.journal_path == "exsim.journal");
    CHECK(cfg.journal_failure_policy == JournalFailurePolicy::Shutdown);
    CHECK_EQ(cfg.journal_flush_interval_ms, 1000u);
    CHECK_EQ(cfg.exchange.trading_day_start_ns, 0u);
    CHECK_EQ(cfg.exchange.trading_day_end_ns, kNanosecondsPerDay);
    REQUIRE_EQ(cfg.bots.size(), 2u);
    CHECK(cfg.bots[0].type == BotType::MarketMaker);
    CHECK(cfg.bots[0].user == "MM1");
    CHECK_EQ(cfg.bots[0].market_maker.session_id, 100);
    CHECK_EQ(cfg.bots[0].market_maker.instrument, 3);  // SPY
    CHECK(cfg.bots[1].type == BotType::NoiseTaker);
    CHECK_EQ(cfg.bots[1].noise_taker.session_id, 101);
}

TEST_CASE(server_config, errors_carry_line_numbers) {
    auto error_of = [](const std::string& text) {
        ServerConfig cfg;
        std::string err;
        CHECK(!parse_config(text, cfg, err));
        return err;
    };
    CHECK(error_of("tick_size = 1\n") .rfind("1: ", 0) == 0);
    CHECK(error_of("\n[bogus]\n").rfind("2: unknown section", 0) == 0);
    CHECK(error_of("[instrument X]\nfoo = 1\n").rfind("2: unknown key", 0) == 0);
    CHECK(error_of("[instrument X]\ntick_size = abc\n").rfind("2: invalid value", 0) == 0);
    CHECK(error_of("[instrument TOOLONGSYM]\n").rfind("1: ", 0) == 0);
    CHECK(error_of("[exchange\n").rfind("1: unterminated", 0) == 0);
}

TEST_CASE(server_config, semantic_validation) {
    auto fails = [](const std::string& extra) {
        ServerConfig cfg;
        std::string err;
        return !parse_config(std::string(kMinimal) + extra, cfg, err);
    };
    CHECK(fails("[user ALICE]\nsession_id = 2\n"));                 // duplicate user
    CHECK(fails("[user BOB]\nsession_id = 1\n"));                   // duplicate session
    CHECK(fails("[user BOB]\npassword = x\n"));                     // missing session_id
    CHECK(fails("[user TOOLONGNAME]\nsession_id = 5\n"));           // OUCH username > 6
    CHECK(!fails("[user TOOLONGNAME]\nsession_id = 5\nprotocol = fix44\n"));
    CHECK(fails("[instrument AAPL]\n"));                            // duplicate instrument
    CHECK(fails("[instrument B]\nmin_price = 0.015\n"));            // off-tick band
    CHECK(fails("[instrument B]\nreference_price = 2000\n"));       // outside default band
    CHECK(fails("[exchange]\nsoup_session = ELEVENCHARS\n"));
    {
        ServerConfig cfg;
        std::string err;
        CHECK(!parse_config("[user A]\nsession_id = 1\n", cfg, err));  // no instruments
    }
}

namespace {
const char* kTwoUsersAndSpy = R"(
[instrument SPY]
tick_size = 0.01
lot_size = 10
max_order_qty = 1000
min_price = 0.01
max_price = 2000
reference_price = 500

[user MM1]
session_id = 100
owner = 7
stp = cancel_both

[user NT1]
session_id = 101
)";

ServerConfig parse_ok(const std::string& text) {
    ServerConfig cfg;
    std::string err;
    const bool ok = parse_config(text, cfg, err);
    if (!ok) testfw::report_failure(__FILE__, __LINE__, err);
    REQUIRE(ok);
    return cfg;
}

std::string error_of(const std::string& text) {
    ServerConfig cfg;
    std::string err;
    CHECK(!parse_config(text, cfg, err));
    return err;
}
}  // namespace

TEST_CASE(server_config, bot_sections_resolve_user_and_instrument) {
    const ServerConfig cfg = parse_ok(std::string(kMinimal) + kTwoUsersAndSpy + R"(
[bot maker]
type = market_maker
user = MM1
instrument = SPY
seed = 42
levels = 2
size = 55
requote_ms = 20

[bot taker]
type = NOISE_TAKER
user = NT1
instrument = AAPL
stp = cancel_aggressor
mean_interval_ms = 5
min_size = 1
max_size = 3
)");
    REQUIRE_EQ(cfg.bots.size(), 2u);
    const BotSectionConfig& maker = cfg.bots[0];
    CHECK(maker.name == "maker");
    CHECK(maker.type == BotType::MarketMaker);
    CHECK(maker.instrument_symbol == "SPY");
    CHECK_EQ(maker.market_maker.session_id, 100);
    CHECK_EQ(maker.market_maker.owner, 7u);
    CHECK(maker.market_maker.stp == StpMode::CancelBoth);  // inherited from the user
    CHECK_EQ(maker.market_maker.instrument, 2);
    CHECK_EQ(maker.market_maker.seed, 42u);
    CHECK_EQ(maker.market_maker.levels, 2u);
    CHECK_EQ(maker.market_maker.size, 55u);
    CHECK_EQ(maker.market_maker.requote_ms, 20u);
    CHECK(&maker.identity() == static_cast<const sim::bots::BotConfig*>(&maker.market_maker));

    const BotSectionConfig& taker = cfg.bots[1];
    CHECK(taker.type == BotType::NoiseTaker);
    CHECK_EQ(taker.noise_taker.session_id, 101);
    CHECK_EQ(taker.noise_taker.instrument, 1);
    CHECK(taker.noise_taker.stp == StpMode::CancelAggressor);  // set in the section
    CHECK_EQ(taker.noise_taker.mean_interval_ms, 5u);
    CHECK_EQ(taker.noise_taker.max_size, 3u);
    CHECK(to_string(taker.type) == "noise_taker");
}

TEST_CASE(server_config, bot_section_errors_carry_line_numbers) {
    const std::string base = std::string(kMinimal) + kTwoUsersAndSpy;
    const auto base_lines = static_cast<int>(std::count(base.begin(), base.end(), '\n'));
    const auto at = [base_lines](int line_in_extra, const std::string& message) {
        return std::to_string(base_lines + line_in_extra) + ": " + message;
    };
    // Lines of the extra text count from 1 at the "[bot ...]" header.
    CHECK(error_of(base + "[bot b]\nuser = MM1\ninstrument = SPY\n").rfind(at(1, "bot b: 'type' is required"), 0) == 0);
    CHECK(error_of(base + "[bot b]\ntype = arbitrageur\n").rfind(at(2, "invalid value 'arbitrageur' for 'type'"), 0) == 0);
    CHECK(error_of(base + "[bot b]\ntype = market_maker\ninstrument = SPY\n").rfind(at(1, "bot b: 'user' is required"), 0) == 0);
    CHECK(error_of(base + "[bot b]\ntype = market_maker\nuser = MM1\n").rfind(at(1, "bot b: 'instrument' is required"), 0) == 0);
    CHECK(error_of(base + "[bot b]\ntype = market_maker\nuser = MM1\ninstrument = SPY\nlevels = 0\n")
              .rfind(at(5, "invalid market_maker option 'levels = 0'"), 0) == 0);
    CHECK(error_of(base + "[bot b]\ntype = market_maker\nuser = MM1\ninstrument = SPY\nsession_id = 5\n")
              .rfind(at(5, "bot key 'session_id' is taken from the bot's user"), 0) == 0);
    CHECK(error_of(base + "[bot b]\ntype = noise_taker\nuser = NT1\ninstrument = SPY\nname = x\n")
              .rfind(at(5, "bot key 'name' is taken from the section header"), 0) == 0);
}

TEST_CASE(server_config, bot_semantic_validation) {
    const std::string base = std::string(kMinimal) + kTwoUsersAndSpy;
    const auto fails = [&base](const std::string& extra) {
        ServerConfig cfg;
        std::string err;
        return !parse_config(base + extra, cfg, err);
    };
    const std::string maker = "type = market_maker\nuser = MM1\ninstrument = SPY\n";
    CHECK(!fails("[bot a]\n" + maker));
    CHECK(fails("[bot a]\n" + maker + "[bot a]\ntype = noise_taker\nuser = NT1\ninstrument = SPY\n"));  // name twice
    CHECK(fails("[bot a]\n" + maker + "[bot b]\n" + maker));                                       // user twice
    CHECK(fails("[bot a]\ntype = market_maker\nuser = NOBODY\ninstrument = SPY\n"));                // unknown user
    CHECK(fails("[bot a]\ntype = market_maker\nuser = MM1\ninstrument = QQQ\n"));                   // unknown symbol
    CHECK(fails("[bot a]\n" + maker + "size = 5\n"));           // below SPY's lot size of 10
    CHECK(fails("[bot a]\n" + maker + "levels = 64\norder_capacity = 100\n"));  // capacity < 2 * levels
    CHECK(fails("[bot a]\ntype = noise_taker\nuser = NT1\ninstrument = SPY\nmin_size = 1\nmax_size = 5\n"));
    CHECK(!fails("[bot a]\ntype = noise_taker\nuser = NT1\ninstrument = SPY\nmin_size = 1\nmax_size = 10\n"));
    CHECK(fails("[bot ]\n" + maker));
    std::string many_bots;
    for (std::size_t i = 0; i <= kMaxBots; ++i) {
        many_bots += "[user U" + std::to_string(i) + "]\nsession_id = " + std::to_string(200 + i) + "\n";
        many_bots += "[bot b" + std::to_string(i) + "]\ntype = noise_taker\nuser = U" + std::to_string(i) +
                     "\ninstrument = AAPL\n";
    }
    CHECK(fails(many_bots));  // more than kMaxBots
}

TEST_CASE(server_config, trading_day_and_journal_keys) {
    ServerConfig cfg = parse_ok(std::string(kMinimal) + R"(
[exchange]
trading_day_start = 09:30
trading_day_end = 16:00:00.5

[journal]
path = /tmp/x.journal
on_failure = continue
flush_interval_ms = 0
)");
    CHECK_EQ(cfg.exchange.trading_day_start_ns, (9 * 3600 + 30 * 60) * 1'000'000'000ull);
    CHECK_EQ(cfg.exchange.trading_day_end_ns, 16 * 3600 * 1'000'000'000ull + 500'000'000ull);
    CHECK(cfg.journal_failure_policy == JournalFailurePolicy::Continue);
    CHECK_EQ(cfg.journal_flush_interval_ms, 0u);

    const auto fails = [](const std::string& extra) {
        ServerConfig parsed;
        std::string err;
        return !parse_config(std::string(kMinimal) + extra, parsed, err);
    };
    CHECK(fails("[exchange]\ntrading_day_start = 16:00\ntrading_day_end = 09:30\n"));
    {
        // A day that crosses midnight is rejected at startup with a message
        // that says so (docs/EXCHANGE_CORE.md section 13).
        ServerConfig parsed;
        std::string err;
        CHECK(!parse_config(std::string(kMinimal) + "[exchange]\ntrading_day_start = 22:00\ntrading_day_end = 02:00\n",
                            parsed, err));
        CHECK(err.find("cross midnight") != std::string::npos);
        CHECK(err.find("22:00:00") != std::string::npos);
    }
    CHECK(fails("[exchange]\ntrading_day_start = 10:00\ntrading_day_end = 10:00\n"));
    CHECK(fails("[exchange]\ntrading_day_end = 24:01\n"));
    CHECK(fails("[exchange]\ntrading_day_start = 9h\n"));
    CHECK(fails("[journal]\non_failure = retry\n"));
    CHECK(fails("[journal]\nflush_interval_ms = soon\n"));
}

TEST_CASE(server_config, day_times_parse_and_format) {
    Timestamp ns = 0;
    CHECK(parse_day_time("00:00", ns));
    CHECK_EQ(ns, 0u);
    CHECK(parse_day_time("09:30:15", ns));
    CHECK_EQ(ns, (9 * 3600 + 30 * 60 + 15) * 1'000'000'000ull);
    CHECK(parse_day_time("23:59:59.999999999", ns));
    CHECK_EQ(ns, kNanosecondsPerDay - 1);
    CHECK(parse_day_time("24:00", ns));
    CHECK_EQ(ns, kNanosecondsPerDay);
    CHECK(!parse_day_time("24:00:01", ns));
    CHECK(!parse_day_time("12:60", ns));
    CHECK(!parse_day_time("12:00:60", ns));
    CHECK(!parse_day_time("12:00:00.1234567890", ns));
    CHECK(!parse_day_time("9:30", ns));
    CHECK(!parse_day_time("", ns));
    CHECK(format_day_time(0) == "00:00:00");
    CHECK(format_day_time(kNanosecondsPerDay) == "24:00:00");
    CHECK(format_day_time((9 * 3600 + 30 * 60) * 1'000'000'000ull + 250'000'000ull) == "09:30:00.25");
    CHECK(format_day_time(kNanosecondsPerDay - 1) == "23:59:59.999999999");
}
