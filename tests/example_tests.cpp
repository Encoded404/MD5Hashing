#include <gtest/gtest.h>

#include <logging/logging.hpp>

import lib_example;

#include "test_logging.hpp"

using modern_cmake_template::example;

namespace {
// Install the per-test file logger once for the test binary.
[[maybe_unused]] const bool kLoggerInstalled = [] {
    TestLogging::InstallPerTestFileLogger();
    return true;
}();
}  // namespace

TEST(ExampleTest, ExampleFunctionDoublesInput) {
    example ex;

    LOGIFACE_LOG(info, "[ExampleFunctionDoublesInput] calling with 0");
    EXPECT_EQ(ex.exampleFunction(0), 0);

    LOGIFACE_LOG(info, "[ExampleFunctionDoublesInput] calling with 1");
    EXPECT_EQ(ex.exampleFunction(1), 2);

    LOGIFACE_LOG(info, "[ExampleFunctionDoublesInput] calling with 5");
    EXPECT_EQ(ex.exampleFunction(5), 10);

    LOGIFACE_LOG(info, "[ExampleFunctionDoublesInput] calling with -3");
    EXPECT_EQ(ex.exampleFunction(-3), -6);
}

TEST(ExampleTest, AnotherExampleFunctionSquaresPlusTen) {
    example ex;

    LOGIFACE_LOG(info, "[AnotherExampleFunctionSquaresPlusTen] calling with 0");
    EXPECT_EQ(ex.anotherExampleFunction(0), 10);      // 0*0 + 10

    LOGIFACE_LOG(info, "[AnotherExampleFunctionSquaresPlusTen] calling with 1");
    EXPECT_EQ(ex.anotherExampleFunction(1), 11);      // 1*1 + 10

    LOGIFACE_LOG(info, "[AnotherExampleFunctionSquaresPlusTen] calling with 3");
    EXPECT_EQ(ex.anotherExampleFunction(3), 19);      // 9 (3*3) + 10

    LOGIFACE_LOG(info, "[AnotherExampleFunctionSquaresPlusTen] calling with -4");
    EXPECT_EQ(ex.anotherExampleFunction(-4), 26);     // 16 (-4*-4) + 10
}