#include "../tests/order_tests.hpp"
#include "../tests/matching_tests.hpp"
#include "../tests/query_tests.hpp"
#include "../tests/itch_replay_tests.hpp"
#include <iostream>

int main() {
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "           LIMIT ORDER BOOK - UNIT TESTS                       \n";
    std::cout << "═══════════════════════════════════════════════════════════════\n\n";

    run_order_tests();
    run_matching_tests();
    run_query_tests();
    itch_replay_tests();

    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "                    ALL TESTS PASSED                           \n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";

    return 0;
}
