// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Minimal in-tree test framework, the same approach as AJA-Input-FFGL: a
// handful of macros instead of a dependency. If the suite outgrows it,
// doctest is a drop-in replacement for this style of macros.
//
//   #include "test_framework.h"
//   TEST_CASE(RingMath, FramesAvailableAcrossWrap) {
//       EXPECT_EQ(framesAvailable(...), 128u);
//       ASSERT_TRUE(ok);            // stops this test case on failure
//   }
//
// Test cases register themselves through static objects; main() calls
// Registry::instance().runAll(), which returns the number of failing cases.
// EXPECT_* records a failure and carries on; ASSERT_* returns from the test
// function.

#pragma once

#include <cstdio>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace ajaduplex::testing
{

class Registry
{
public:
    using TestFunction = void (*)();

    static Registry& instance()
    {
        static Registry registry;
        return registry;
    }

    void add(const char* suite, const char* name, TestFunction function)
    {
        tests_.push_back(Test{suite, name, function});
    }

    void fail(const char* file, int line, const std::string& message)
    {
        currentFailed_ = true;
        std::printf("    FAILED %s:%d: %s\n", file, line, message.c_str());
    }

    int runAll()
    {
        int failed = 0;
        for (const Test& test : tests_)
        {
            currentFailed_ = false;
            test.function();
            std::printf("%s %s.%s\n", currentFailed_ ? "[FAIL]" : "[ ok ]", test.suite, test.name);
            if (currentFailed_)
                ++failed;
        }
        std::printf("\n%zu test cases, %d failed\n", tests_.size(), failed);
        return failed;
    }

private:
    struct Test
    {
        const char* suite;
        const char* name;
        TestFunction function;
    };

    std::vector<Test> tests_;
    bool currentFailed_ = false;
};

struct Registrar
{
    Registrar(const char* suite, const char* name, Registry::TestFunction function)
    {
        Registry::instance().add(suite, name, function);
    }
};

/// Printable form of a compared value for failure messages.
template <typename T>
std::string toTestString(const T& value)
{
    using U = std::decay_t<T>;
    if constexpr (std::is_same_v<U, bool>)
        return value ? "true" : "false";
    else if constexpr (std::is_same_v<U, std::string>)
        return "\"" + value + "\"";
    else if constexpr (std::is_array_v<T> && (std::is_same_v<U, const char*> || std::is_same_v<U, char*>))
        return "\"" + std::string(value) + "\"";   // a string literal (T = char[N]): never null
    else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>)
        return value ? "\"" + std::string(value) + "\"" : std::string("nullptr");
    else if constexpr (std::is_enum_v<U>)
        return std::to_string(static_cast<long long>(static_cast<std::underlying_type_t<U>>(value)));
    else if constexpr (std::is_arithmetic_v<U>)
    {
        std::ostringstream out;
        out << +value;
        return out.str();
    }
    else
        return "<value>";
}

} // namespace ajaduplex::testing

#define AJADUPLEX_TEST_FAIL(message) \
    ::ajaduplex::testing::Registry::instance().fail(__FILE__, __LINE__, (message))

#define TEST_CASE(suite, name)                                                        \
    static void test_##suite##_##name();                                              \
    static const ::ajaduplex::testing::Registrar registrar_##suite##_##name(          \
        #suite, #name, &test_##suite##_##name);                                       \
    static void test_##suite##_##name()

#define EXPECT_TRUE(condition)                                                        \
    do {                                                                              \
        if (!(condition))                                                             \
            AJADUPLEX_TEST_FAIL("expected true: " #condition);                        \
    } while (0)

#define EXPECT_FALSE(condition)                                                       \
    do {                                                                              \
        if (condition)                                                                \
            AJADUPLEX_TEST_FAIL("expected false: " #condition);                       \
    } while (0)

#define AJADUPLEX_COMPARE(a, b, op, onFailure)                                        \
    do {                                                                              \
        const auto& lhs_ = (a);                                                       \
        const auto& rhs_ = (b);                                                       \
        if (!(lhs_ op rhs_))                                                          \
        {                                                                             \
            AJADUPLEX_TEST_FAIL(std::string(#a " " #op " " #b ": ") +                 \
                                ::ajaduplex::testing::toTestString(lhs_) + " vs " +   \
                                ::ajaduplex::testing::toTestString(rhs_));            \
            onFailure;                                                                \
        }                                                                             \
    } while (0)

#define EXPECT_EQ(a, b) AJADUPLEX_COMPARE(a, b, ==, (void)0)
#define EXPECT_NE(a, b) AJADUPLEX_COMPARE(a, b, !=, (void)0)
#define ASSERT_EQ(a, b) AJADUPLEX_COMPARE(a, b, ==, return)

#define ASSERT_TRUE(condition)                                                        \
    do {                                                                              \
        if (!(condition))                                                             \
        {                                                                             \
            AJADUPLEX_TEST_FAIL("expected true: " #condition);                        \
            return;                                                                   \
        }                                                                             \
    } while (0)

/// True when `text` contains `part`; for checking error messages.
#define EXPECT_CONTAINS(text, part)                                                   \
    do {                                                                              \
        const std::string text_ = (text);                                             \
        const std::string part_ = (part);                                             \
        if (text_.find(part_) == std::string::npos)                                   \
            AJADUPLEX_TEST_FAIL("expected \"" + text_ + "\" to contain \"" + part_ + "\""); \
    } while (0)
