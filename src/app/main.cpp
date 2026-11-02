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

// ---------------------------------------------------------------------------
// MD5 implementations available from the CLI.
//
// To add another one: implement `hashlib::<name>` and add a row here. The CLI
// choices, the "--algorithm all" expansion, the default behaviour and the
// benchmark table are all derived from this list, so nothing else needs to
// change.
// ---------------------------------------------------------------------------

using Md5Function = hashlib::Digest (*)(std::span<const std::byte>);

struct Algorithm {
    std::string_view name;
    std::string_view description;
    Md5Function function;
};

constexpr std::array kAlgorithms{
    Algorithm{"naive", "readable reference implementation, allocates", hashlib::naive_md5},
    Algorithm{"fast", "allocation-free, fully unrolled", hashlib::md5_fast},
};

constexpr std::string_view kAllAlgorithms = "all";

std::string ToLowerAscii(std::string_view text) {
    std::string lower;
    lower.reserve(text.size());
    for (char ch : text) {
        lower.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    return lower;
}

std::string JoinAlgorithmNames(std::string_view separator) {
    std::string joined;
    for (const Algorithm& algorithm : kAlgorithms) {
        if (!joined.empty()) {
            joined += separator;
        }
        joined += algorithm.name;
    }
    return joined;
}

std::string AlgorithmHelp() {
    std::string help;
    for (const Algorithm& algorithm : kAlgorithms) {
        if (!help.empty()) {
            help += ", ";
        }
        help += std::string(algorithm.name) + " (" + std::string(algorithm.description) + ")";
    }
    return help;
}

std::string JoinSelectedNames(const std::vector<const Algorithm*>& algorithms) {
    std::string joined;
    for (const Algorithm* algorithm : algorithms) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += algorithm->name;
    }
    return joined;
}

std::optional<const Algorithm*> FindAlgorithm(std::string_view name) {
    const std::string lower = ToLowerAscii(name);
    for (const Algorithm& algorithm : kAlgorithms) {
        if (lower == algorithm.name) {
            return &algorithm;
        }
    }
    return std::nullopt;
}

struct Selection {
    std::vector<const Algorithm*> algorithms;
    std::string error;  // empty when the selection is valid
};

std::string_view TrimAscii(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// `requested` is the raw -a value: empty means "everything", otherwise it is a
// comma-separated list (CLI11 also joins repeated -a flags with commas), and the
// name "all" expands to all of them. So no -a, `-a fast`, `-a fast,naive` and
// `-a all` all do what they look like.
Selection SelectAlgorithms(std::string_view requested) {
    Selection selection;
    if (requested.empty()) {
        for (const Algorithm& algorithm : kAlgorithms) {
            selection.algorithms.push_back(&algorithm);
        }
        return selection;
    }

    std::size_t start = 0;
    while (start <= requested.size()) {
        const std::size_t comma = requested.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? requested.size() : comma;
        const std::string_view token = TrimAscii(requested.substr(start, end - start));

        if (ToLowerAscii(token) == kAllAlgorithms) {
            for (const Algorithm& algorithm : kAlgorithms) {
                selection.algorithms.push_back(&algorithm);
            }
        } else if (const std::optional<const Algorithm*> algorithm = FindAlgorithm(token)) {
            selection.algorithms.push_back(*algorithm);
        } else if (!token.empty()) {
            selection.error = "unknown MD5 implementation '" + std::string(token) +
                              "'; available: " + JoinAlgorithmNames(", ");
            return selection;
        }

        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }

    // -a all -a fast would otherwise hash twice.
    std::vector<const Algorithm*> unique;
    for (const Algorithm* algorithm : selection.algorithms) {
        if (std::ranges::find(unique, algorithm) == unique.end()) {
            unique.push_back(algorithm);
        }
    }
    selection.algorithms = std::move(unique);

    if (selection.algorithms.empty()) {
        selection.error = "no MD5 implementation selected; available: " + JoinAlgorithmNames(", ");
    }
    return selection;
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

// Fixed-point formatting without pulling in <format>.
std::string ToFixed(double value, int decimals) {
    long long scale = 1;
    for (int i = 0; i < decimals; ++i) {
        scale *= 10;
    }
    const long long scaled = static_cast<long long>(value * static_cast<double>(scale) + 0.5);
    std::string text = std::to_string(scaled / scale);
    if (decimals > 0) {
        const std::string digits = std::to_string(scaled % scale);
        text += '.';
        text.append(static_cast<std::size_t>(decimals) - std::min(digits.size(),
                                                                  static_cast<std::size_t>(decimals)),
                    '0');
        text += digits;
    }
    return text;
}

// Runs every selected implementation and reports the digests. Running more than
// one is also a cheap correctness check: the variants must agree.
void PrintDigests(std::span<const std::byte> input,
                  const std::vector<const Algorithm*>& algorithms) {
    if (algorithms.empty()) {
        return;
    }
    LOGIFACE_LOG(info, "Hashing " + std::to_string(input.size()) + " byte(s)");

    std::size_t widest = 0;
    for (const Algorithm* algorithm : algorithms) {
        widest = std::max(widest, algorithm->name.size());
    }

    std::optional<hashlib::Digest> reference;
    for (const Algorithm* algorithm : algorithms) {
        const hashlib::Digest digest = algorithm->function(input);

        std::string label(algorithm->name);
        label.append(widest - label.size(), ' ');
        LOGIFACE_LOG(info, "md5 (" + label + ") = " + ToHex(digest));

        if (!reference.has_value()) {
            reference = digest;
        } else if (digest != *reference) {
            LOGIFACE_LOG(error, "md5 (" + std::string(algorithm->name) +
                                    ") disagrees with md5 (" + std::string(algorithms.front()->name) +
                                    ") -- one of them is buggy");
        }
    }
}

void Benchmark(std::span<const std::byte> input, const std::vector<const Algorithm*>& algorithms,
               int iterations) {
    if (iterations <= 0) {
        LOGIFACE_LOG(warn, "Skipping benchmark: --benchmark-count must be positive");
        return;
    }
    if (algorithms.empty()) {
        return;
    }

    // A mismatch here would make the timings meaningless, so check once first.
    const hashlib::Digest reference = algorithms.front()->function(input);
    for (const Algorithm* algorithm : algorithms) {
        if (algorithm->function(input) != reference) {
            LOGIFACE_LOG(error, "Skipping benchmark: md5 (" + std::string(algorithm->name) +
                                    ") disagrees with md5 (" +
                                    std::string(algorithms.front()->name) + ")");
            return;
        }
    }

    for (const Algorithm* algorithm : algorithms) {
        Timer::Timer timer(true);
        for (int i = 0; i < iterations; i++) {
            const hashlib::Digest digest = algorithm->function(input);
            DoNotOptimize(digest);
        }
        timer.Stop();

        const double nanosecondsPerCall =
            timer.ElapsedNs() / static_cast<double>(iterations);
        std::string line = "md5 (" + std::string(algorithm->name) + "): " +
                           std::to_string(iterations) + " call(s) in " +
                           ToFixed(timer.ElapsedMs(), 1) + " ms, " +
                           ToFixed(nanosecondsPerCall, 1) + " ns/call";
        if (!input.empty()) {
            // bytes per nanosecond is GB/s
            line += ", " + ToFixed(static_cast<double>(input.size()) / nanosecondsPerCall, 2) +
                    " GB/s";
        }
        LOGIFACE_LOG(info, line);
    }

    const auto* logger = Logiface::GetLogger();
    if (logger != nullptr &&
        static_cast<int>(logger->GetLevel()) <= static_cast<int>(Logiface::Level::trace)) {
        LOGIFACE_LOG(warn,
                     "Benchmark ran with trace logging enabled; any implementation that logs "
                     "per call then includes that cost in its timing");
    }
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

    // A single string rather than std::vector<std::string>: a vector option
    // greedily eats a trailing positional argument ("-a fast hello" would hash
    // "fast"), and CLI11 then rejects the count. Joining repeated flags with
    // ',' gives repeatable options without that ambiguity.
    std::string algorithm_list;
    app.add_option("-a,--algorithm", algorithm_list,
                   "MD5 implementation(s) to run: " + AlgorithmHelp() + ". Repeat the flag or "
                   "comma separate for more than one (default: all of them)")
        ->delimiter(',')
        ->multi_option_policy(CLI::MultiOptionPolicy::Join)
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

    Selection selection = SelectAlgorithms(algorithm_list);
    if (!selection.error.empty()) {
        return app.exit(CLI::ValidationError("--algorithm", selection.error));
    }

    LOGIFACE_LOG(debug, "Running " + std::to_string(selection.algorithms.size()) +
                            " implementation(s): " + JoinSelectedNames(selection.algorithms));

    const std::span<const std::byte> input = std::as_bytes(std::span{text.data(), text.size()});

    PrintDigests(input, selection.algorithms);

    if (benchmark) {
        Benchmark(input, selection.algorithms, benchmark_count);
    }

    LOGIFACE_LOG(info, "App completed");

    return 0;
}

void initializeLogger(Logiface::Level level) {
    static Logiface::ConsoleLogger app_logger;
    app_logger.SetLevel(level);
    Logiface::SetLogger(&app_logger);
}
