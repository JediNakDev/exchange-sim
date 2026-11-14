#include "../../framework/test.hpp"
#include <protocol/itch50.hpp>
#include <random>
using namespace exsim::protocol::itch50;
TEST_CASE(proto_itch50_golden, SystemEvent) {
    std::mt19937 rng(123);
    char gold[12], result[12];
    SystemEvent m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'S';
        gold[11] = "OSQMEC"[run % 6];
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.stock_locate),
                 ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[2])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.tracking_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[3])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[4])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[5])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[9])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[10])) << 0)));
        CHECK_EQ(m.event, gold[11]);
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_itch50_golden, StockDirectory) {
    std::mt19937 rng(123);
    char gold[39], result[39];
    StockDirectory m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'R';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.stock_locate),
                 ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[2])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.tracking_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[3])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[4])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[5])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[9])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[10])) << 0)));
        for (unsigned i = 0; i < 8; ++i) CHECK_EQ(m.stock.chars[i], gold[11 + i]);
        CHECK_EQ(m.market_category, gold[19]);
        CHECK_EQ(m.financial_status, gold[20]);
        CHECK_EQ(static_cast<std::uint64_t>(m.round_lot_size),
                 ((std::uint64_t(static_cast<unsigned char>(gold[21])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[22])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[23])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[24])) << 0)));
        CHECK_EQ(m.round_lots_only, gold[25]);
        CHECK_EQ(m.issue_classification, gold[26]);
        for (unsigned i = 0; i < 2; ++i) CHECK_EQ(m.issue_subtype[i], gold[27 + i]);
        CHECK_EQ(m.authenticity, gold[29]);
        CHECK_EQ(m.short_sale_threshold, gold[30]);
        CHECK_EQ(m.ipo_flag, gold[31]);
        CHECK_EQ(m.luld_tier, gold[32]);
        CHECK_EQ(m.etp_flag, gold[33]);
        CHECK_EQ(static_cast<std::uint64_t>(m.etp_leverage),
                 ((std::uint64_t(static_cast<unsigned char>(gold[34])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[35])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[36])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[37])) << 0)));
        CHECK_EQ(m.inverse_indicator, gold[38]);
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_itch50_golden, StockTradingAction) {
    std::mt19937 rng(123);
    char gold[25], result[25];
    StockTradingAction m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'H';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.stock_locate),
                 ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[2])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.tracking_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[3])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[4])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[5])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[9])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[10])) << 0)));
        for (unsigned i = 0; i < 8; ++i) CHECK_EQ(m.stock.chars[i], gold[11 + i]);
        CHECK_EQ(m.trading_state, gold[19]);
        CHECK_EQ(m.reserved, gold[20]);
        for (unsigned i = 0; i < 4; ++i) CHECK_EQ(m.reason[i], gold[21 + i]);
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_itch50_golden, AddOrder) {
    std::mt19937 rng(123);
    char gold[36], result[36];
    AddOrder m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'A';
        gold[19] = run % 2 ? 'B' : 'S';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.stock_locate),
                 ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[2])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.tracking_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[3])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[4])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[5])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[9])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[10])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.order_reference),
                 ((std::uint64_t(static_cast<unsigned char>(gold[11])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[12])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[13])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[14])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[15])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[16])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[17])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[18])) << 0)));
        CHECK_EQ(m.side, gold[19] == 'B' ? exsim::Side::BUY : exsim::Side::SELL);
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[20])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[21])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[22])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[23])) << 0)));
        for (unsigned i = 0; i < 8; ++i) CHECK_EQ(m.stock.chars[i], gold[24 + i]);
        CHECK_EQ(static_cast<std::uint64_t>(m.price), ((std::uint64_t(static_cast<unsigned char>(gold[32])) << 24) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[33])) << 16) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[34])) << 8) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[35])) << 0)));
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_itch50_golden, AddOrderMPID) {
    std::mt19937 rng(123);
    char gold[40], result[40];
    AddOrderMPID m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'F';
        gold[19] = run % 2 ? 'B' : 'S';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.stock_locate),
                 ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[2])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.tracking_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[3])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[4])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[5])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[9])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[10])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.order_reference),
                 ((std::uint64_t(static_cast<unsigned char>(gold[11])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[12])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[13])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[14])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[15])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[16])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[17])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[18])) << 0)));
        CHECK_EQ(m.side, gold[19] == 'B' ? exsim::Side::BUY : exsim::Side::SELL);
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[20])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[21])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[22])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[23])) << 0)));
        for (unsigned i = 0; i < 8; ++i) CHECK_EQ(m.stock.chars[i], gold[24 + i]);
        CHECK_EQ(static_cast<std::uint64_t>(m.price), ((std::uint64_t(static_cast<unsigned char>(gold[32])) << 24) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[33])) << 16) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[34])) << 8) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[35])) << 0)));
        for (unsigned i = 0; i < 4; ++i) CHECK_EQ(m.attribution[i], gold[36 + i]);
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_itch50_golden, OrderExecuted) {
    std::mt19937 rng(123);
    char gold[31], result[31];
    OrderExecuted m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'E';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.stock_locate),
                 ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[2])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.tracking_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[3])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[4])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[5])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[9])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[10])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.order_reference),
                 ((std::uint64_t(static_cast<unsigned char>(gold[11])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[12])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[13])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[14])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[15])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[16])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[17])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[18])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[19])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[20])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[21])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[22])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.match_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[23])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[24])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[25])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[26])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[27])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[28])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[29])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[30])) << 0)));
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_itch50_golden, OrderExecutedWithPrice) {
    std::mt19937 rng(123);
    char gold[36], result[36];
    OrderExecutedWithPrice m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'C';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.stock_locate),
                 ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[2])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.tracking_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[3])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[4])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[5])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[9])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[10])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.order_reference),
                 ((std::uint64_t(static_cast<unsigned char>(gold[11])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[12])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[13])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[14])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[15])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[16])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[17])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[18])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[19])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[20])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[21])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[22])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.match_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[23])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[24])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[25])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[26])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[27])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[28])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[29])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[30])) << 0)));
        CHECK_EQ(m.printable, gold[31]);
        CHECK_EQ(static_cast<std::uint64_t>(m.price), ((std::uint64_t(static_cast<unsigned char>(gold[32])) << 24) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[33])) << 16) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[34])) << 8) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[35])) << 0)));
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_itch50_golden, OrderCancel) {
    std::mt19937 rng(123);
    char gold[23], result[23];
    OrderCancel m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'X';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.stock_locate),
                 ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[2])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.tracking_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[3])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[4])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[5])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[9])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[10])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.order_reference),
                 ((std::uint64_t(static_cast<unsigned char>(gold[11])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[12])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[13])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[14])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[15])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[16])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[17])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[18])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[19])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[20])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[21])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[22])) << 0)));
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_itch50_golden, OrderDelete) {
    std::mt19937 rng(123);
    char gold[19], result[19];
    OrderDelete m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'D';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.stock_locate),
                 ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[2])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.tracking_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[3])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[4])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[5])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[9])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[10])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.order_reference),
                 ((std::uint64_t(static_cast<unsigned char>(gold[11])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[12])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[13])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[14])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[15])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[16])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[17])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[18])) << 0)));
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_itch50_golden, OrderReplace) {
    std::mt19937 rng(123);
    char gold[35], result[35];
    OrderReplace m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'U';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.stock_locate),
                 ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[2])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.tracking_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[3])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[4])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[5])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[9])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[10])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.original_reference),
                 ((std::uint64_t(static_cast<unsigned char>(gold[11])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[12])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[13])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[14])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[15])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[16])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[17])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[18])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.new_reference),
                 ((std::uint64_t(static_cast<unsigned char>(gold[19])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[20])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[21])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[22])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[23])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[24])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[25])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[26])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[27])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[28])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[29])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[30])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.price), ((std::uint64_t(static_cast<unsigned char>(gold[31])) << 24) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[32])) << 16) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[33])) << 8) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[34])) << 0)));
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_itch50_golden, Trade) {
    std::mt19937 rng(123);
    char gold[44], result[44];
    Trade m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'P';
        gold[19] = run % 2 ? 'B' : 'S';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.stock_locate),
                 ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[2])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.tracking_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[3])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[4])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[5])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[9])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[10])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.order_reference),
                 ((std::uint64_t(static_cast<unsigned char>(gold[11])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[12])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[13])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[14])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[15])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[16])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[17])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[18])) << 0)));
        CHECK_EQ(m.side, gold[19] == 'B' ? exsim::Side::BUY : exsim::Side::SELL);
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[20])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[21])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[22])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[23])) << 0)));
        for (unsigned i = 0; i < 8; ++i) CHECK_EQ(m.stock.chars[i], gold[24 + i]);
        CHECK_EQ(static_cast<std::uint64_t>(m.price), ((std::uint64_t(static_cast<unsigned char>(gold[32])) << 24) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[33])) << 16) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[34])) << 8) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[35])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.match_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[36])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[37])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[38])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[39])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[40])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[41])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[42])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[43])) << 0)));
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_itch50_golden, CrossTrade) {
    std::mt19937 rng(123);
    char gold[40], result[40];
    CrossTrade m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'Q';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.stock_locate),
                 ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[2])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.tracking_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[3])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[4])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[5])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[9])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[10])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[11])) << 56) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[12])) << 48) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[13])) << 40) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[14])) << 32) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[15])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[16])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[17])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[18])) << 0)));
        for (unsigned i = 0; i < 8; ++i) CHECK_EQ(m.stock.chars[i], gold[19 + i]);
        CHECK_EQ(static_cast<std::uint64_t>(m.price), ((std::uint64_t(static_cast<unsigned char>(gold[27])) << 24) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[28])) << 16) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[29])) << 8) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[30])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.match_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[31])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[32])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[33])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[34])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[35])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[36])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[37])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[38])) << 0)));
        CHECK_EQ(m.cross_type, gold[39]);
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_itch50_golden, NetOrderImbalance) {
    std::mt19937 rng(123);
    char gold[50], result[50];
    NetOrderImbalance m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'I';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.stock_locate),
                 ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[2])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.tracking_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[3])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[4])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[5])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[9])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[10])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.paired_shares),
                 ((std::uint64_t(static_cast<unsigned char>(gold[11])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[12])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[13])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[14])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[15])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[16])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[17])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[18])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.imbalance_shares),
                 ((std::uint64_t(static_cast<unsigned char>(gold[19])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[20])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[21])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[22])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[23])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[24])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[25])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[26])) << 0)));
        CHECK_EQ(m.imbalance_direction, gold[27]);
        for (unsigned i = 0; i < 8; ++i) CHECK_EQ(m.stock.chars[i], gold[28 + i]);
        CHECK_EQ(static_cast<std::uint64_t>(m.far_price), ((std::uint64_t(static_cast<unsigned char>(gold[36])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[37])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[38])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[39])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.near_price),
                 ((std::uint64_t(static_cast<unsigned char>(gold[40])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[41])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[42])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[43])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.current_reference_price),
                 ((std::uint64_t(static_cast<unsigned char>(gold[44])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[45])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[46])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[47])) << 0)));
        CHECK_EQ(m.cross_type, gold[48]);
        CHECK_EQ(m.price_variation, gold[49]);
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
