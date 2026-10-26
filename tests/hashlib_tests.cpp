// gtest pulls standard headers in textually. Include it (and the macro-only
// logging header) before `import std;`; the reverse order collides with the
// declarations owned by the std module. This is the same ordering used by
// VulkanEngineV5's test suite.
#include <gtest/gtest.h>
#include <logging/logging_macros.hpp>

import std;
import logiface;
import test_logging;

import hashlib;

namespace {
// Install the per-test file logger once for the test binary.
[[maybe_unused]] const bool kLoggerInstalled = [] {
    TestLogging::InstallPerTestFileLogger();
    return true;
}();
}  // namespace

// Placeholder test: the MD5 core is intentionally not implemented yet. Replace
// this with known-answer vectors (RFC 1321) once the algorithm lands. MD5 takes
// raw bytes, so "abc" is passed as three byte values here.
TEST(HashLibTest, Md5ReportsUnimplemented) {
    constexpr std::array<std::byte, 3> kInput{std::byte{0x61}, std::byte{0x62}, std::byte{0x63}};

    LOGIFACE_LOG(info, "[Md5ReportsUnimplemented] calling md5 with 3 byte(s)");
    EXPECT_THROW((void)hashlib::naive_md5(kInput), std::logic_error);
}
