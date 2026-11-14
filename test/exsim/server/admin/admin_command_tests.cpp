#include "server/admin/admin_command.hpp"
#include "test/framework/test.hpp"

using namespace exsim;
using namespace exsim::server;

namespace {
AdminCommand ok(std::string_view line) {
    AdminCommand c;
    std::string err;
    const bool parsed = parse_admin(line, c, err);
    if (!parsed) testfw::report_failure(__FILE__, __LINE__, std::string(line) + " -> " + err);
    return c;
}
bool bad(std::string_view line) {
    AdminCommand c;
    std::string err;
    const bool parsed = parse_admin(line, c, err);
    return !parsed && !err.empty();
}
}  // namespace

TEST_CASE(server_admin, simple_commands) {
    CHECK(ok("help").op == AdminOp::Help);
    CHECK(ok("  STATUS \r\n").op == AdminOp::Status);
    CHECK(ok("shutdown").op == AdminOp::Shutdown);
    auto p = ok("phase AAPL pre_close");
    CHECK(p.op == AdminOp::Phase);
    CHECK(p.symbol == "AAPL");
    CHECK(p.phase == Phase::PreClose);
    CHECK(ok("phase * Continuous").symbol == "*");
    auto b = ok("book MSFT");
    CHECK(b.op == AdminOp::Book);
    CHECK_EQ(b.count, 5u);
    const auto deep_book = ok("book MSFT 20");
    CHECK_EQ(deep_book.count, 20u);
}

TEST_CASE(server_admin, md_faults) {
    auto d = ok("fault md drop 5");
    CHECK(d.op == AdminOp::FaultMdDrop);
    CHECK_EQ(d.count, 5u);
    CHECK(ok("fault md dup 1").op == AdminOp::FaultMdDup);
    auto r = ok("fault md reorder 3");
    CHECK(r.op == AdminOp::FaultMdReorder);
    CHECK_EQ(r.count, 3u);
    CHECK_EQ(r.delay, 1u);
    const auto delayed_reorder = ok("fault md reorder 3 4");
    CHECK_EQ(delayed_reorder.delay, 4u);
    auto rate = ok("fault md rate drop=1000 dup=20 reorder=300 delay=2 seed=9");
    CHECK(rate.op == AdminOp::FaultMdRate);
    CHECK_EQ(rate.rate.drop_ppm, 1000u);
    CHECK_EQ(rate.rate.duplicate_ppm, 20u);
    CHECK_EQ(rate.rate.reorder_ppm, 300u);
    CHECK_EQ(rate.rate.reorder_delay, 2u);
    CHECK_EQ(rate.rate.seed, 9u);
    CHECK(ok("fault md off").op == AdminOp::FaultMdOff);
}

TEST_CASE(server_admin, user_faults) {
    auto r = ok("fault user ALICE reject 2");
    CHECK(r.op == AdminOp::FaultUserReject);
    CHECK(r.user == "ALICE");
    CHECK_EQ(r.count, 2u);
    auto d = ok("fault user BOB delay 250");
    CHECK(d.op == AdminOp::FaultUserDelay);
    CHECK_EQ(d.delay, 250u);
    CHECK(ok("fault user BOB drop_out 3").op == AdminOp::FaultUserDropOut);
    CHECK(ok("fault user BOB disconnect").op == AdminOp::FaultUserDisconnect);
    CHECK(ok("fault user BOB off").op == AdminOp::FaultUserOff);
}

TEST_CASE(server_admin, rejects_bad_input) {
    CHECK(bad(""));
    CHECK(bad("frobnicate"));
    CHECK(bad("help me"));
    CHECK(bad("phase AAPL"));
    CHECK(bad("phase AAPL open"));
    CHECK(bad("book"));
    CHECK(bad("book AAPL 0"));
    CHECK(bad("book AAPL x"));
    CHECK(bad("fault"));
    CHECK(bad("fault md drop"));
    CHECK(bad("fault md drop 0"));
    CHECK(bad("fault md drop -1"));
    CHECK(bad("fault md reorder 1 0"));
    CHECK(bad("fault md rate drop=2000000"));
    CHECK(bad("fault md rate bogus=1"));
    CHECK(bad("fault md rate drop"));
    CHECK(bad("fault md explode 1"));
    CHECK(bad("fault user ALICE"));
    CHECK(bad("fault user ALICE reject"));
    CHECK(bad("fault user ALICE nuke"));
    CHECK(bad("bots maybe"));
}

TEST_CASE(server_admin, help_lists_every_verb) {
    const std::string_view h = admin_help();
    for (std::string_view verb : {"status", "phase", "book", "fault md", "fault user", "shutdown"}) {
        CHECK(h.find(verb) != std::string_view::npos);
    }
}

TEST_CASE(server_admin, reused_result_is_transactional) {
    AdminCommand command;
    std::string error;
    REQUIRE(parse_admin("book MsFt 12", command, error));
    CHECK(!parse_admin("fault md rate drop=1 DROP=2", command, error));
    CHECK(!error.empty());
    CHECK(command.op == AdminOp::Book);
    CHECK_EQ(command.symbol, "MsFt");
    CHECK_EQ(command.count, 12u);
    REQUIRE(parse_admin("status", command, error));
    CHECK(error.empty());
    CHECK(command.op == AdminOp::Status);
    CHECK(command.symbol.empty());
    CHECK_EQ(command.count, 0u);
}

TEST_CASE(server_admin, numeric_limits_and_complete_tokens) {
    const auto maximum_count = ok("fault md drop 4294967295");
    CHECK_EQ(maximum_count.count, UINT32_MAX);
    const auto maximum_seed = ok("fault md rate seed=18446744073709551615");
    CHECK_EQ(maximum_seed.rate.seed, UINT64_MAX);
    const auto full_rate = ok("fault md rate drop=1000000 dup=0 reorder=0");
    CHECK_EQ(full_rate.rate.drop_ppm, 1'000'000u);
    const auto no_user_delay = ok("fault user TraderA delay 0");
    CHECK_EQ(no_user_delay.delay, 0u);
    CHECK_EQ(no_user_delay.user, "TraderA");
    for (const auto invalid : {"fault md drop 4294967296", "fault md drop 1x",
                              "fault md drop +1", "fault md rate seed=18446744073709551616",
                              "fault md rate delay=0", "fault md rate drop=", "fault md rate =1",
                              "fault md rate seed=1 seed=2", "shutdown now", "book AAPL 1 2"}) {
        CHECK(bad(invalid));
    }
}
