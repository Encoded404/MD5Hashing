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

using Md5Function = hashlib::Digest (*)(std::span<const std::byte>);

// Every MD5 implementation the library exports. Adding one to hashlib means
// adding a row here, and then every known-answer and boundary test covers it.
constexpr std::array kImplementations{
    std::pair{std::string_view{"naive_md5"}, Md5Function{hashlib::naive_md5}},
    std::pair{std::string_view{"md5_fast"}, Md5Function{hashlib::md5_fast}},
};

std::string ToHex(std::span<const std::byte> bytes) {
    static constexpr char kHexDigits[] = "0123456789abcdef";

    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::byte byte : bytes) {
        const unsigned value = std::to_integer<unsigned>(byte);
        text.push_back(kHexDigits[value >> 4]);
        text.push_back(kHexDigits[value & 0x0F]);
    }
    return text;
}

std::string Md5Hex(std::string_view text, Md5Function function) {
    const std::span<const std::byte> input = std::as_bytes(std::span{text.data(), text.size()});
    return ToHex(function(input));
}

}  // namespace

// Known-answer vectors from RFC 1321, Appendix A.2.
TEST(HashLibTest, Md5KnownAnswerVectors) {
    constexpr std::array kCases{
        std::pair{std::string_view{""}, std::string_view{"d41d8cd98f00b204e9800998ecf8427e"}},
        std::pair{std::string_view{"a"}, std::string_view{"0cc175b9c0f1b6a831c399e269772661"}},
        std::pair{std::string_view{"abc"}, std::string_view{"900150983cd24fb0d6963f7d28e17f72"}},
        std::pair{std::string_view{"message digest"},
                  std::string_view{"f96b697d7cb7938d525a2f31aaf161d0"}},
        std::pair{std::string_view{"abcdefghijklmnopqrstuvwxyz"},
                  std::string_view{"c3fcd3d76192e4007dfb496cca67e13b"}},
        std::pair{std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"},
                  std::string_view{"d174ab98d277d9f5a5611c2c9f419d9f"}},
        std::pair{std::string_view{"1234567890123456789012345678901234567890123456789012345678901234"
                                   "5678901234567890"},
                  std::string_view{"57edf4a22be3c955ac49da2e2107b67a"}},
    };

    for (const auto& [name, function] : kImplementations) {
        SCOPED_TRACE(std::string(name));
        for (const auto& [input, expected] : kCases) {
            SCOPED_TRACE(std::string(input));
            EXPECT_EQ(Md5Hex(input, function), expected);
        }
    }
}

// Padding edges: 55 bytes still fits the 0x80 marker plus the 8-byte length in
// a single 64-byte block, 56 forces an extra block, and 63/64 exercise the
// block boundary. 1000 bytes covers many blocks and a non-trivial tail.
TEST(HashLibTest, Md5PaddingBoundaries) {
    constexpr std::array kCases{
        std::pair{55U, std::string_view{"ef1772b6dff9a122358552954ad0df65"}},
        std::pair{56U, std::string_view{"3b0c8ac703f828b04c6c197006d17218"}},
        std::pair{63U, std::string_view{"b06521f39153d618550606be297466d5"}},
        std::pair{64U, std::string_view{"014842d480b571495a4a0363793f7367"}},
        std::pair{1000U, std::string_view{"cabe45dcc9ae5b66ba86600cca6b8ba8"}},
    };

    for (const auto& [name, function] : kImplementations) {
        SCOPED_TRACE(std::string(name));
        for (const auto& [length, expected] : kCases) {
            SCOPED_TRACE(std::to_string(length));
            const std::string input(length, 'a');
            EXPECT_EQ(Md5Hex(input, function), expected);
        }
    }
}

// Regression test: the block counter used to be std::uint16_t, so a padded
// input of exactly 4 MiB (65,536 blocks) wrapped to zero blocks and returned
// the empty-input digest. This input also forces the longest possible padding
// and produces exactly 65,536 blocks.
TEST(HashLibTest, Md5HandlesMoreThan64KBlocks) {
    const std::string input(4'194'232, 'a');
    for (const auto& [name, function] : kImplementations) {
        SCOPED_TRACE(std::string(name));
        EXPECT_EQ(Md5Hex(input, function), "a650f836e82882751694e0c25736af2c");
    }
}

// The implementations must agree for every length, including the lengths where
// the padded tail needs an extra block. This is the property that lets the CLI
// run several of them by default and compare the results.
TEST(HashLibTest, Md5ImplementationsAgree) {
    for (std::size_t length = 0; length <= 200; ++length) {
        SCOPED_TRACE(std::to_string(length));
        const std::string input(length, static_cast<char>('a' + length % 26));

        const hashlib::Digest reference = hashlib::naive_md5(
            std::as_bytes(std::span{input.data(), input.size()}));
        for (const auto& [name, function] : kImplementations) {
            SCOPED_TRACE(std::string(name));
            EXPECT_EQ(function(std::as_bytes(std::span{input.data(), input.size()})), reference);
        }
    }
}

// Binary input, not just text: the span API takes raw bytes.
TEST(HashLibTest, Md5HandlesBinaryBytes) {
    std::array<std::byte, 256> input{};
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::byte>(i);
    }

    const hashlib::Digest reference = hashlib::naive_md5(input);
    for (const auto& [name, function] : kImplementations) {
        SCOPED_TRACE(std::string(name));
        EXPECT_EQ(function(input), reference);
    }
}
