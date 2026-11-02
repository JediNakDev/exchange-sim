#include "exsim/server/config.hpp"
#include "../../framework/test.hpp"

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
    CHECK_EQ(cfg.users.size(), 4u);
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
