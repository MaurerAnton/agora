#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace agora::core::test {

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Case {
    std::string name;
    std::function<void()> run;
};

std::vector<Case>& registry();

struct Registrar {
    Registrar(std::string name, std::function<void()> run);
};

[[noreturn]] void fail(const char* file, int line, const std::string& message);

template <typename T>
std::string show(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

inline std::string show(const std::string& value) { return value; }

}  // namespace agora::core::test

#define TEST(name)                                                              \
    static void core_test_##name();                                             \
    static const ::agora::core::test::Registrar core_registrar_##name(          \
        #name, core_test_##name);                                               \
    static void core_test_##name()

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            ::agora::core::test::fail(__FILE__, __LINE__,                       \
                "CHECK failed: " #condition);                                   \
        }                                                                       \
    } while (0)

#define CHECK_EQ(expected, actual)                                              \
    do {                                                                        \
        const auto& core_check_expected = (expected);                           \
        const auto& core_check_actual = (actual);                               \
        if (!(core_check_expected == core_check_actual)) {                      \
            ::agora::core::test::fail(__FILE__, __LINE__,                       \
                std::string("expected ") +                                      \
                ::agora::core::test::show(core_check_expected) +                \
                " but got " +                                                   \
                ::agora::core::test::show(core_check_actual));                  \
        }                                                                       \
    } while (0)
