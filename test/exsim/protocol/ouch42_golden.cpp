#include "../../framework/test.hpp"
#include <protocol/ouch42.hpp>
#include <random>
using namespace exsim::protocol::ouch42;
TEST_CASE(proto_ouch42_golden, EnterOrder) {
    std::mt19937 rng(123);
    char gold[49], result[49];
    EnterOrder m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'O';
        gold[15] = "BSTE"[run % 4];
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        for (unsigned i = 0; i < 14; ++i) CHECK_EQ(m.token[i], gold[1 + i]);
        CHECK_EQ(m.side, gold[15]);
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[16])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[17])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[18])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[19])) << 0)));
        for (unsigned i = 0; i < 8; ++i) CHECK_EQ(m.stock.chars[i], gold[20 + i]);
        CHECK_EQ(static_cast<std::uint64_t>(m.price), ((std::uint64_t(static_cast<unsigned char>(gold[28])) << 24) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[29])) << 16) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[30])) << 8) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[31])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.time_in_force),
                 ((std::uint64_t(static_cast<unsigned char>(gold[32])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[33])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[34])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[35])) << 0)));
        for (unsigned i = 0; i < 4; ++i) CHECK_EQ(m.firm[i], gold[36 + i]);
        CHECK_EQ(m.display, gold[40]);
        CHECK_EQ(m.capacity, gold[41]);
        CHECK_EQ(m.ise, gold[42]);
        CHECK_EQ(static_cast<std::uint64_t>(m.minimum_quantity),
                 ((std::uint64_t(static_cast<unsigned char>(gold[43])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[44])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[45])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[46])) << 0)));
        CHECK_EQ(m.cross_type, gold[47]);
        CHECK_EQ(m.customer_type, gold[48]);
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_ouch42_golden, ReplaceOrder) {
    std::mt19937 rng(123);
    char gold[47], result[47];
    ReplaceOrder m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'U';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        for (unsigned i = 0; i < 14; ++i) CHECK_EQ(m.existing_token[i], gold[1 + i]);
        for (unsigned i = 0; i < 14; ++i) CHECK_EQ(m.replacement_token[i], gold[15 + i]);
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[29])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[30])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[31])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[32])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.price), ((std::uint64_t(static_cast<unsigned char>(gold[33])) << 24) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[34])) << 16) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[35])) << 8) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[36])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.time_in_force),
                 ((std::uint64_t(static_cast<unsigned char>(gold[37])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[38])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[39])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[40])) << 0)));
        CHECK_EQ(m.display, gold[41]);
        CHECK_EQ(m.ise, gold[42]);
        CHECK_EQ(static_cast<std::uint64_t>(m.minimum_quantity),
                 ((std::uint64_t(static_cast<unsigned char>(gold[43])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[44])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[45])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[46])) << 0)));
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_ouch42_golden, CancelOrder) {
    std::mt19937 rng(123);
    char gold[19], result[19];
    CancelOrder m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'X';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        for (unsigned i = 0; i < 14; ++i) CHECK_EQ(m.token[i], gold[1 + i]);
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[15])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[16])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[17])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[18])) << 0)));
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_ouch42_golden, ModifyOrder) {
    std::mt19937 rng(123);
    char gold[20], result[20];
    ModifyOrder m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'M';
        gold[15] = "BSTE"[run % 4];
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        for (unsigned i = 0; i < 14; ++i) CHECK_EQ(m.token[i], gold[1 + i]);
        CHECK_EQ(m.side, gold[15]);
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[16])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[17])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[18])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[19])) << 0)));
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_ouch42_golden, SystemEvent) {
    std::mt19937 rng(123);
    char gold[10], result[10];
    SystemEvent m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'S';
        gold[9] = "SE"[run % 2];
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 56) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[2])) << 48) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[3])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[4])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[5])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 0)));
        CHECK_EQ(m.event, gold[9]);
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_ouch42_golden, OrderAccepted) {
    std::mt19937 rng(123);
    char gold[66], result[66];
    OrderAccepted m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'A';
        gold[64] = "LD"[run % 2];
        gold[23] = "BSTE"[run % 4];
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 56) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[2])) << 48) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[3])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[4])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[5])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 0)));
        for (unsigned i = 0; i < 14; ++i) CHECK_EQ(m.token[i], gold[9 + i]);
        CHECK_EQ(m.side, gold[23]);
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[24])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[25])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[26])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[27])) << 0)));
        for (unsigned i = 0; i < 8; ++i) CHECK_EQ(m.stock.chars[i], gold[28 + i]);
        CHECK_EQ(static_cast<std::uint64_t>(m.price), ((std::uint64_t(static_cast<unsigned char>(gold[36])) << 24) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[37])) << 16) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[38])) << 8) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[39])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.time_in_force),
                 ((std::uint64_t(static_cast<unsigned char>(gold[40])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[41])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[42])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[43])) << 0)));
        for (unsigned i = 0; i < 4; ++i) CHECK_EQ(m.firm[i], gold[44 + i]);
        CHECK_EQ(m.display, gold[48]);
        CHECK_EQ(static_cast<std::uint64_t>(m.order_reference),
                 ((std::uint64_t(static_cast<unsigned char>(gold[49])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[50])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[51])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[52])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[53])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[54])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[55])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[56])) << 0)));
        CHECK_EQ(m.capacity, gold[57]);
        CHECK_EQ(m.ise, gold[58]);
        CHECK_EQ(static_cast<std::uint64_t>(m.minimum_quantity),
                 ((std::uint64_t(static_cast<unsigned char>(gold[59])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[60])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[61])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[62])) << 0)));
        CHECK_EQ(m.cross_type, gold[63]);
        CHECK_EQ(m.order_state, gold[64]);
        CHECK_EQ(m.bbo_weight, gold[65]);
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_ouch42_golden, OrderCanceled) {
    std::mt19937 rng(123);
    char gold[28], result[28];
    OrderCanceled m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'C';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 56) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[2])) << 48) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[3])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[4])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[5])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 0)));
        for (unsigned i = 0; i < 14; ++i) CHECK_EQ(m.token[i], gold[9 + i]);
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[23])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[24])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[25])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[26])) << 0)));
        CHECK_EQ(m.reason, gold[27]);
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_ouch42_golden, OrderExecuted) {
    std::mt19937 rng(123);
    char gold[40], result[40];
    OrderExecuted m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'E';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 56) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[2])) << 48) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[3])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[4])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[5])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 0)));
        for (unsigned i = 0; i < 14; ++i) CHECK_EQ(m.token[i], gold[9 + i]);
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[23])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[24])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[25])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[26])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.price), ((std::uint64_t(static_cast<unsigned char>(gold[27])) << 24) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[28])) << 16) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[29])) << 8) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[30])) << 0)));
        CHECK_EQ(m.liquidity, gold[31]);
        CHECK_EQ(static_cast<std::uint64_t>(m.match_number),
                 ((std::uint64_t(static_cast<unsigned char>(gold[32])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[33])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[34])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[35])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[36])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[37])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[38])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[39])) << 0)));
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_ouch42_golden, OrderRejected) {
    std::mt19937 rng(123);
    char gold[24], result[24];
    OrderRejected m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'J';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 56) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[2])) << 48) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[3])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[4])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[5])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 0)));
        for (unsigned i = 0; i < 14; ++i) CHECK_EQ(m.token[i], gold[9 + i]);
        CHECK_EQ(m.reason, gold[23]);
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_ouch42_golden, CancelPending) {
    std::mt19937 rng(123);
    char gold[23], result[23];
    CancelPending m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'P';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 56) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[2])) << 48) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[3])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[4])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[5])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 0)));
        for (unsigned i = 0; i < 14; ++i) CHECK_EQ(m.token[i], gold[9 + i]);
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_ouch42_golden, CancelReject) {
    std::mt19937 rng(123);
    char gold[23], result[23];
    CancelReject m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'I';
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 56) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[2])) << 48) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[3])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[4])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[5])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 0)));
        for (unsigned i = 0; i < 14; ++i) CHECK_EQ(m.token[i], gold[9 + i]);
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_ouch42_golden, OrderModified) {
    std::mt19937 rng(123);
    char gold[28], result[28];
    OrderModified m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'M';
        gold[23] = "BSTE"[run % 4];
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 56) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[2])) << 48) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[3])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[4])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[5])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 0)));
        for (unsigned i = 0; i < 14; ++i) CHECK_EQ(m.token[i], gold[9 + i]);
        CHECK_EQ(m.side, gold[23]);
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[24])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[25])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[26])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[27])) << 0)));
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
TEST_CASE(proto_ouch42_golden, OrderReplaced) {
    std::mt19937 rng(123);
    char gold[80], result[80];
    OrderReplaced m;
    for (unsigned run = 0; run < 102; ++run) {
        for (char& c : gold) c = static_cast<char>(run == 0 ? 0 : run == 1 ? 255 : rng() % 256);
        gold[0] = 'U';
        gold[64] = "LD"[run % 2];
        gold[23] = "BSTE"[run % 4];
        REQUIRE_EQ(decode(gold, sizeof gold, m), Status::ok);
        CHECK_EQ(static_cast<std::uint64_t>(m.timestamp), ((std::uint64_t(static_cast<unsigned char>(gold[1])) << 56) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[2])) << 48) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[3])) << 40) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[4])) << 32) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[5])) << 24) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[6])) << 16) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[7])) << 8) |
                                                           (std::uint64_t(static_cast<unsigned char>(gold[8])) << 0)));
        for (unsigned i = 0; i < 14; ++i) CHECK_EQ(m.replacement_token[i], gold[9 + i]);
        CHECK_EQ(m.side, gold[23]);
        CHECK_EQ(static_cast<std::uint64_t>(m.shares), ((std::uint64_t(static_cast<unsigned char>(gold[24])) << 24) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[25])) << 16) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[26])) << 8) |
                                                        (std::uint64_t(static_cast<unsigned char>(gold[27])) << 0)));
        for (unsigned i = 0; i < 8; ++i) CHECK_EQ(m.stock.chars[i], gold[28 + i]);
        CHECK_EQ(static_cast<std::uint64_t>(m.price), ((std::uint64_t(static_cast<unsigned char>(gold[36])) << 24) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[37])) << 16) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[38])) << 8) |
                                                       (std::uint64_t(static_cast<unsigned char>(gold[39])) << 0)));
        CHECK_EQ(static_cast<std::uint64_t>(m.time_in_force),
                 ((std::uint64_t(static_cast<unsigned char>(gold[40])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[41])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[42])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[43])) << 0)));
        for (unsigned i = 0; i < 4; ++i) CHECK_EQ(m.firm[i], gold[44 + i]);
        CHECK_EQ(m.display, gold[48]);
        CHECK_EQ(static_cast<std::uint64_t>(m.order_reference),
                 ((std::uint64_t(static_cast<unsigned char>(gold[49])) << 56) |
                  (std::uint64_t(static_cast<unsigned char>(gold[50])) << 48) |
                  (std::uint64_t(static_cast<unsigned char>(gold[51])) << 40) |
                  (std::uint64_t(static_cast<unsigned char>(gold[52])) << 32) |
                  (std::uint64_t(static_cast<unsigned char>(gold[53])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[54])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[55])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[56])) << 0)));
        CHECK_EQ(m.capacity, gold[57]);
        CHECK_EQ(m.ise, gold[58]);
        CHECK_EQ(static_cast<std::uint64_t>(m.minimum_quantity),
                 ((std::uint64_t(static_cast<unsigned char>(gold[59])) << 24) |
                  (std::uint64_t(static_cast<unsigned char>(gold[60])) << 16) |
                  (std::uint64_t(static_cast<unsigned char>(gold[61])) << 8) |
                  (std::uint64_t(static_cast<unsigned char>(gold[62])) << 0)));
        CHECK_EQ(m.cross_type, gold[63]);
        CHECK_EQ(m.order_state, gold[64]);
        for (unsigned i = 0; i < 14; ++i) CHECK_EQ(m.previous_token[i], gold[65 + i]);
        CHECK_EQ(m.bbo_weight, gold[79]);
        CHECK_EQ(encode(m, result, sizeof result), sizeof result);
        CHECK_EQ(std::string_view(result, sizeof result), std::string_view(gold, sizeof gold));
    }
}
