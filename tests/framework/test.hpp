#ifndef TESTS_FRAMEWORK_TEST_HPP
#define TESTS_FRAMEWORK_TEST_HPP

// Minimal self-registering test framework (no external dependencies).
//
//   TEST_CASE(suite, name) { CHECK(x); CHECK_EQ(a, b); REQUIRE(ptr); }
//
// CHECK* records a failure and continues; REQUIRE* records and aborts the
// current test case. Test binaries are run by tests/main.cpp, which accepts a
// substring filter: `run_tests matching` runs every test whose
// "suite.name" contains "matching".

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace testfw {

using TestFn = void (*)();

struct TestCase {
    const char* suite;
    const char* name;
    TestFn fn;
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* suite, const char* name, TestFn fn) { registry().push_back({suite, name, fn}); }
};

struct AbortTest {};

void report_failure(const char* file, int line, const std::string& message);

template <typename T, typename = void>
struct is_streamable : std::false_type {};
template <typename T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <typename T>
std::string show(const T& v) {
    std::ostringstream os;
    if constexpr (std::is_enum_v<T>) {
        os << static_cast<long long>(static_cast<std::underlying_type_t<T>>(v));
    } else if constexpr (std::is_same_v<T, bool>) {
        os << (v ? "true" : "false");
    } else if constexpr (std::is_same_v<T, char> || std::is_same_v<T, signed char> ||
                         std::is_same_v<T, unsigned char>) {
        os << static_cast<int>(v);
    } else if constexpr (is_streamable<T>::value) {
        os << v;
    } else {
        os << "<unprintable>";
    }
    return os.str();
}

}  // namespace testfw

#define TESTFW_CONCAT_(a, b) a##b
#define TESTFW_CONCAT(a, b) TESTFW_CONCAT_(a, b)

#define TEST_CASE(suite, name)                                                              \
    static void TESTFW_CONCAT(test_fn_, TESTFW_CONCAT(suite, TESTFW_CONCAT(_, name)))();     \
    static const ::testfw::Registrar TESTFW_CONCAT(test_reg_, TESTFW_CONCAT(suite, TESTFW_CONCAT(_, name)))( \
        #suite, #name, &TESTFW_CONCAT(test_fn_, TESTFW_CONCAT(suite, TESTFW_CONCAT(_, name)))); \
    static void TESTFW_CONCAT(test_fn_, TESTFW_CONCAT(suite, TESTFW_CONCAT(_, name)))()

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) ::testfw::report_failure(__FILE__, __LINE__, "CHECK(" #expr ")"); \
    } while (0)

#define REQUIRE(expr)                                                                \
    do {                                                                             \
        if (!(expr)) {                                                               \
            ::testfw::report_failure(__FILE__, __LINE__, "REQUIRE(" #expr ")");      \
            throw ::testfw::AbortTest{};                                             \
        }                                                                            \
    } while (0)

#define TESTFW_CMP_(kind, a, b, op, fatal)                                                    \
    do {                                                                                      \
        const auto& testfw_a_ = (a);                                                          \
        const auto& testfw_b_ = (b);                                                          \
        if (!(testfw_a_ op testfw_b_)) {                                                      \
            ::testfw::report_failure(__FILE__, __LINE__,                                      \
                std::string(kind "(" #a ", " #b ")  lhs=") + ::testfw::show(testfw_a_) +       \
                    "  rhs=" + ::testfw::show(testfw_b_));                                    \
            if (fatal) throw ::testfw::AbortTest{};                                           \
        }                                                                                     \
    } while (0)

#define CHECK_EQ(a, b) TESTFW_CMP_("CHECK_EQ", a, b, ==, false)
#define CHECK_NE(a, b) TESTFW_CMP_("CHECK_NE", a, b, !=, false)
#define CHECK_LT(a, b) TESTFW_CMP_("CHECK_LT", a, b, <, false)
#define CHECK_LE(a, b) TESTFW_CMP_("CHECK_LE", a, b, <=, false)
#define CHECK_GT(a, b) TESTFW_CMP_("CHECK_GT", a, b, >, false)
#define CHECK_GE(a, b) TESTFW_CMP_("CHECK_GE", a, b, >=, false)
#define REQUIRE_EQ(a, b) TESTFW_CMP_("REQUIRE_EQ", a, b, ==, true)

// Fails the current test with a message and aborts it.
#define FAIL(msg)                                                         \
    do {                                                                  \
        ::testfw::report_failure(__FILE__, __LINE__, std::string(msg));   \
        throw ::testfw::AbortTest{};                                      \
    } while (0)

#endif
