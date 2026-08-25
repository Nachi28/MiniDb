#pragma once

// A ~100 line test framework so the suite builds with zero network access and
// zero dependencies. Swap in GoogleTest later if you want its matchers; the
// TEST/REQUIRE spelling below is deliberately source-compatible.

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace minitest {

struct AssertionFailure {
    std::string message;
};

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> body) {
        registry().push_back(TestCase{suite, name, std::move(body)});
    }
};

inline int run_all() {
    std::size_t passed = 0;
    std::vector<std::string> failures;

    for (const TestCase& test : registry()) {
        const std::string label = test.suite + "." + test.name;
        try {
            test.body();
            ++passed;
        } catch (const AssertionFailure& failure) {
            failures.push_back(label + "\n" + failure.message);
        } catch (const std::exception& e) {
            failures.push_back(label + "\n    unexpected exception: " + e.what());
        } catch (...) {
            failures.push_back(label + "\n    unexpected non-standard exception");
        }
    }

    for (const std::string& failure : failures) {
        std::cout << "[ FAILED ] " << failure << "\n";
    }
    std::cout << "\n" << passed << " passed, " << failures.size() << " failed, "
              << registry().size() << " total\n";
    return failures.empty() ? 0 : 1;
}

}  // namespace minitest

#define MINITEST_CONCAT_INNER(a, b) a##b
#define MINITEST_CONCAT(a, b) MINITEST_CONCAT_INNER(a, b)

#define TEST(suite_name, test_name)                                                    \
    static void MINITEST_CONCAT(suite_name, MINITEST_CONCAT(_, test_name))();          \
    static ::minitest::Registrar MINITEST_CONCAT(minitest_reg_, __LINE__)(             \
        #suite_name, #test_name,                                                       \
        MINITEST_CONCAT(suite_name, MINITEST_CONCAT(_, test_name)));                   \
    static void MINITEST_CONCAT(suite_name, MINITEST_CONCAT(_, test_name))()

#define REQUIRE(condition)                                                             \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::ostringstream minitest_oss;                                           \
            minitest_oss << "    " << __FILE__ << ":" << __LINE__                       \
                         << ": REQUIRE(" #condition ")";                               \
            throw ::minitest::AssertionFailure{minitest_oss.str()};                    \
        }                                                                              \
    } while (false)

#define REQUIRE_EQ(actual, expected)                                                   \
    do {                                                                               \
        const auto& minitest_lhs = (actual);                                           \
        const auto& minitest_rhs = (expected);                                         \
        if (!(minitest_lhs == minitest_rhs)) {                                         \
            std::ostringstream minitest_oss;                                           \
            minitest_oss << "    " << __FILE__ << ":" << __LINE__                       \
                         << ": REQUIRE_EQ(" #actual ", " #expected ")\n"                \
                         << "      actual:   " << minitest_lhs << "\n"                  \
                         << "      expected: " << minitest_rhs;                        \
            throw ::minitest::AssertionFailure{minitest_oss.str()};                    \
        }                                                                              \
    } while (false)

#define REQUIRE_THROWS_AS(expression, ExceptionType)                                   \
    do {                                                                               \
        bool minitest_threw = false;                                                   \
        try {                                                                          \
            (void)(expression);                                                        \
        } catch (const ExceptionType&) {                                               \
            minitest_threw = true;                                                     \
        } catch (...) {                                                                \
        }                                                                              \
        if (!minitest_threw) {                                                         \
            std::ostringstream minitest_oss;                                           \
            minitest_oss << "    " << __FILE__ << ":" << __LINE__                       \
                         << ": expected " #expression " to throw " #ExceptionType;      \
            throw ::minitest::AssertionFailure{minitest_oss.str()};                    \
        }                                                                              \
    } while (false)
