#ifndef TEST_FRAMEWORK_HPP
#define TEST_FRAMEWORK_HPP

#include <iostream>

#define RUN_TEST(name) do { \
    std::cout << "  " #name "... "; \
    name(); \
    std::cout << "PASSED\n"; \
} while(0)

#endif
