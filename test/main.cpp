// Test runner: runs every registered TEST_CASE (optionally filtered).
//
//   run_tests                 run everything
//   run_tests itch            run tests whose "suite.name" contains "itch"
//   run_tests --list          list tests

#include "framework/test.hpp"

#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

namespace testfw {

namespace {
int g_current_failures = 0;
}

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void report_failure(const char* file, int line, const std::string& message) {
    ++g_current_failures;
    std::cout << "\n    " << file << ":" << line << ": " << message;
}

}  // namespace testfw

int main(int argc, char** argv) {
    std::string filter;
    bool list_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else {
            filter = argv[i];
        }
    }

    int run = 0;
    int failed = 0;
    std::vector<std::string> failed_names;
    for (const auto& tc : testfw::registry()) {
        const std::string full = std::string(tc.suite) + "." + tc.name;
        if (!filter.empty() && full.find(filter) == std::string::npos) {
            continue;
        }
        if (list_only) {
            std::cout << full << "\n";
            continue;
        }

        ++run;
        testfw::g_current_failures = 0;
        std::cout << "  " << full << " ... " << std::flush;
        const auto t0 = std::chrono::steady_clock::now();
        try {
            tc.fn();
        } catch (const testfw::AbortTest&) {
        } catch (const std::exception& e) {
            testfw::report_failure(__FILE__, __LINE__, std::string("uncaught exception: ") + e.what());
        } catch (...) {
            testfw::report_failure(__FILE__, __LINE__, "uncaught non-std exception");
        }
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        if (testfw::g_current_failures == 0) {
            std::cout << "ok (" << ms << " ms)\n";
        } else {
            ++failed;
            failed_names.push_back(full);
            std::cout << "\n  FAILED " << full << "\n";
        }
    }

    if (list_only) {
        return 0;
    }
    if (run == 0) {
        std::cerr << "No tests matched filter: " << filter << "\n";
        return 2;
    }
    std::cout << "\n" << run - failed << "/" << run << " tests passed\n";
    for (const auto& name : failed_names) {
        std::cout << "  FAILED: " << name << "\n";
    }
    return (failed == 0 && run > 0) ? 0 : 1;
}
