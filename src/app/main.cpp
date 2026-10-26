// CLI11's headers are included textually, before `import std;`, for the same
// reason as gtest in the tests: the reverse order collides with the
// declarations owned by the std module.
#include <CLI/CLI.hpp>
#include <logging/logging_macros.hpp>

import std;
import logiface;
import logiface.ConsoleLogger;
import Timer;

import hashlib;

void initializeLogger(Logiface::Level level);
void BasicHash(std::span<const std::byte> input);

template <class T>
inline void DoNotOptimize(T const& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "m"(value) : "memory");
#else
    volatile auto* sink = &value;  // MSVC: force it into memory
    (void)*sink;
    _ReadWriteBarrier();
#endif
}

namespace {

std::string ToLowerAscii(std::string_view text) {
    std::string lower;
    lower.reserve(text.size());
    for (char ch : text) {
        lower.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    return lower;
}

std::optional<Logiface::Level> ToLogLevel(std::string_view name) {
    const std::string lower = ToLowerAscii(name);
    if (lower == "trace") return Logiface::Level::trace;
    if (lower == "debug") return Logiface::Level::debug;
    if (lower == "info") return Logiface::Level::info;
    if (lower == "warn" || lower == "warning") return Logiface::Level::warn;
    if (lower == "error") return Logiface::Level::error;
    if (lower == "critical") return Logiface::Level::critical;
    return std::nullopt;
}

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

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Compute the MD5 digest of a string"};

    std::string text = "The quick brown fox jumps over the lazy dog";
    app.add_option("text", text, "Text to hash")
        ->default_val("The quick brown fox jumps over the lazy dog")
        ->capture_default_str();

    std::string log_level = "info";
    app.add_option("-l,--log-level", log_level,
                   "Minimum log level: trace, debug, info, warn (warning), error, critical")
        ->default_val("info")
        ->check(CLI::IsMember(
            {"trace", "debug", "info", "warn", "warning", "error", "critical"},
            CLI::ignore_case))
        ->capture_default_str();

    bool benchmark = false;
    app.add_flag("-b,--benchmark", benchmark)
        ->default_val(false);

    int benchmark_count = 1000000;
    app.add_option("--benchmark-count", benchmark_count)
        ->default_val(1000000);

    CLI11_PARSE(app, argc, argv);

    // CLI11 already validated the value; the fallback only fires if the two
    // level lists drift apart, but it still exits through CLI11 rather than
    // terminating on an uncaught exception.
    const std::optional<Logiface::Level> level = ToLogLevel(log_level);
    if (!level) {
        return app.exit(CLI::ValidationError("--log-level", "unknown log level: " + log_level));
    }
    initializeLogger(*level);

    LOGIFACE_LOG(info, "HashApp CLI started");
    LOGIFACE_LOG(debug, std::string("Log level set to ") + Logiface::ToString(*level));

    const std::span<const std::byte> input = std::as_bytes(std::span{text.data(), text.size()});

    BasicHash(input);

    if (benchmark) {
        Timer::Timer timer(true);
        for (int i = 0; i < benchmark_count; i++) {
            hashlib::Digest digest = hashlib::naive_md5(input);
            //DoNotOptimize(digest); // not needed?
        }
        timer.Stop();
        LOGIFACE_LOG(info, "hashed " + std::to_string(benchmark_count) + " times in " + std::to_string(timer.ElapsedMs()) + " ms");
    }

    LOGIFACE_LOG(info, "App completed");

    return 0;
}

void initializeLogger(Logiface::Level level) {
    static Logiface::ConsoleLogger app_logger;
    app_logger.SetLevel(level);
    Logiface::SetLogger(&app_logger);
}

void BasicHash(std::span<const std::byte> input) {
    LOGIFACE_LOG(info, "Hashing " + std::to_string(input.size()) + " byte(s)");

    try {
        const hashlib::Digest digest = hashlib::naive_md5(input);
        LOGIFACE_LOG(info, "md5 = " + ToHex(digest));
    } catch (const std::logic_error& error) {
        LOGIFACE_LOG(warn, std::string("md5 unavailable: ") + error.what());
    }
}