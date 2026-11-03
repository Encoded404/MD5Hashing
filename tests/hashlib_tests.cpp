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
//
// The fill is position-dependent on purpose.
// a single repeated character makes every 32-bit message word identical, which hides a wrong message-word index.
TEST(HashLibTest, Md5ImplementationsAgree) {
    const auto checkLength = [](std::size_t length) {
        SCOPED_TRACE(std::to_string(length));
        std::string input(length, '\0');
        for (std::size_t i = 0; i < length; ++i) {
            input[i] = static_cast<char>(i * 37 + 11);
        }

        const std::span<const std::byte> bytes = std::as_bytes(std::span{input.data(), input.size()});
        const hashlib::Digest reference = hashlib::naive_md5(bytes);
        for (const auto& [name, function] : kImplementations) {
            SCOPED_TRACE(std::string(name));
            EXPECT_EQ(function(bytes), reference);
        }
    };

    for (std::size_t length = 0; length <= 200; ++length) {
        checkLength(length);
    }

    // Block and tail boundaries beyond the sweep above, plus a few multi-block sizes.
    constexpr std::array kBoundaryLengths{
        std::size_t{255},  std::size_t{256},  std::size_t{511},  std::size_t{512},
        std::size_t{513},  std::size_t{1000}, std::size_t{1023}, std::size_t{1024},
        std::size_t{4095}, std::size_t{4096}, std::size_t{4097}, std::size_t{65535},
        std::size_t{65536}, std::size_t{65537},
    };
    for (const std::size_t length : kBoundaryLengths) {
        checkLength(length);
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

// The batch entry point must agree with the single-message implementations.
// Each lane is padded independently, so mixed lengths (and lengths on both
// sides of the 55/56 and 63/64 boundaries) are the interesting cases.
TEST(HashLibTest, Md5BatchKnownAnswerVectors) {
    constexpr std::array kCases{
        std::pair{std::string_view{""}, std::string_view{"d41d8cd98f00b204e9800998ecf8427e"}},
        std::pair{std::string_view{"a"}, std::string_view{"0cc175b9c0f1b6a831c399e269772661"}},
        std::pair{std::string_view{"abc"}, std::string_view{"900150983cd24fb0d6963f7d28e17f72"}},
        std::pair{std::string_view{"message digest"},
                  std::string_view{"f96b697d7cb7938d525a2f31aaf161d0"}},
        std::pair{std::string_view{"The quick brown fox jumps over the lazy dog"},
                  std::string_view{"9e107d9d372bb6826bd81d3542a419d6"}},
    };

    std::vector<std::span<const std::byte>> inputs;
    inputs.reserve(kCases.size());
    for (const auto& [text, expected] : kCases) {
        (void)expected;
        inputs.emplace_back(std::as_bytes(std::span{text.data(), text.size()}));
    }

    std::vector<hashlib::Digest> digests(inputs.size());
    hashlib::md5_batch(inputs, digests);

    for (std::size_t i = 0; i < kCases.size(); ++i) {
        SCOPED_TRACE(std::string(kCases[i].first));
        EXPECT_EQ(ToHex(digests[i]), kCases[i].second);
    }
}

TEST(HashLibTest, Md5BatchReportsKernel) {
    const hashlib::BatchInfo info = hashlib::md5_batch_info();
    EXPECT_FALSE(info.isa.empty());
    EXPECT_TRUE(info.isa == "avx512f" || info.isa == "avx2" || info.isa == "sse2" ||
                info.isa == "scalar");
    // One lane per scalar message, or the ISA width times the number of
    // interleaved groups the kernel hashes per call (4/8/16, 8/16, 16/32/64).
    EXPECT_TRUE(info.lanes == 1 || info.lanes == 4 || info.lanes == 8 ||
                info.lanes == 16 || info.lanes == 32 || info.lanes == 64);
}

TEST(HashLibTest, Md5BatchMatchesSingleForMixedLengths) {
    constexpr std::array<std::size_t, 19> kLengths{
        0, 1, 2, 3, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 191, 192, 255, 1000};

    for (const std::size_t batchSize :
         {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4}, std::size_t{7},
          std::size_t{8}, std::size_t{9}, std::size_t{15}, std::size_t{16}, std::size_t{17},
          std::size_t{31}, std::size_t{64}, std::size_t{257}}) {
        SCOPED_TRACE("batch size " + std::to_string(batchSize));

        std::vector<std::string> messages;
        messages.reserve(batchSize);
        for (std::size_t i = 0; i < batchSize; ++i) {
            const std::size_t length = kLengths[(i * 7 + batchSize) % kLengths.size()];
            std::string message(length, '\0');
            for (std::size_t j = 0; j < length; ++j) {
                // Position-dependent: repeated bytes hide a wrong message index.
                message[j] = static_cast<char>((i * 131 + j * 37 + 11) & 0xFF);
            }
            messages.push_back(std::move(message));
        }

        std::vector<std::span<const std::byte>> inputs;
        inputs.reserve(batchSize);
        for (const std::string& message : messages) {
            inputs.emplace_back(std::as_bytes(std::span{message.data(), message.size()}));
        }

        // Deliberately oversized: the API promises to write only inputs.size().
        std::vector<hashlib::Digest> digests(batchSize + 3);
        hashlib::md5_batch(inputs, digests);

        for (std::size_t i = 0; i < batchSize; ++i) {
            SCOPED_TRACE("index " + std::to_string(i));
            EXPECT_EQ(digests[i], hashlib::md5_fast(inputs[i]));
        }
    }
}

TEST(HashLibTest, Md5BatchHandlesEmptyBatch) {
    std::vector<std::span<const std::byte>> inputs;
    std::vector<hashlib::Digest> digests;
    hashlib::md5_batch(inputs, digests);
    EXPECT_TRUE(digests.empty());
}

// The runtime picks the widest kernel the CPU supports, so an ordinary run only
// ever executes one of the compiled kernels. HASHLIB_FORCE_ISA selects one
// explicitly, and tests/CMakeLists.txt registers a CTest entry per kernel so all
// of them run on a machine that supports them. This test makes the selection
// visible: it skips (rather than passing silently) when the requested kernel is
// not available, which is what happens on an older CPU or a non-x86 build.
TEST(HashLibTest, Md5BatchUsesForcedKernel) {
    const char* forced = std::getenv("HASHLIB_FORCE_ISA");
    if (forced == nullptr || *forced == '\0') {
        GTEST_SKIP() << "HASHLIB_FORCE_ISA not set";
    }

    const std::string requested{forced};
    const hashlib::BatchInfo info = hashlib::md5_batch_info();
    if (info.isa != requested) {
        GTEST_SKIP() << "kernel '" << requested << "' is not available on this CPU; the "
                     << "dispatcher selected '" << info.isa << "'";
    }

    // Hash a batch of differing lengths with the forced kernel and check it
    // against the single-message path, so the forced kernel is exercised rather
    // than only reported.
    constexpr std::size_t kBatchSize = 16;
    std::vector<std::string> messages;
    messages.reserve(kBatchSize);
    for (std::size_t i = 0; i < kBatchSize; ++i) {
        messages.emplace_back(1 + i * 13, static_cast<char>('a' + i));
    }

    std::vector<std::span<const std::byte>> inputs;
    inputs.reserve(kBatchSize);
    for (const std::string& message : messages) {
        inputs.emplace_back(std::as_bytes(std::span{message.data(), message.size()}));
    }

    std::vector<hashlib::Digest> digests(inputs.size());
    hashlib::md5_batch(inputs, digests);

    for (std::size_t i = 0; i < inputs.size(); ++i) {
        SCOPED_TRACE("index " + std::to_string(i));
        EXPECT_EQ(digests[i], hashlib::md5_fast(inputs[i]));
    }
}
