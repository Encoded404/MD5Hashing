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
import solve;

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

// The batch API hashes `batchSize` independent messages per call. Report the
// cost of the call itself and the cost per individual digest; the latter is
// what matters for throughput and is the number that beats md5_fast.
void BenchmarkBatch(std::span<const std::byte> input, int iterations, int batchSize) {
    const hashlib::BatchInfo info = hashlib::md5_batch_info();

    // Independent copies, so the kernel reads different buffers. MD5's cost is
    // data-independent, but identical pointers would not measure the real
    // memory behaviour.
    std::string message(input.size(), '\0');
    if (!input.empty()) {
        std::memcpy(message.data(), input.data(), input.size());
    }
    std::vector<std::string> messages(static_cast<std::size_t>(batchSize), message);

    std::vector<std::span<const std::byte>> inputs;
    inputs.reserve(messages.size());
    for (const std::string& text : messages) {
        inputs.emplace_back(std::as_bytes(std::span{text.data(), text.size()}));
    }

    std::vector<hashlib::Digest> digests(messages.size());

    // A mismatch would make the timing meaningless.
    hashlib::md5_batch(inputs, digests);
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (digests[i] != hashlib::md5_fast(inputs[i])) {
            LOGIFACE_LOG(error, "Skipping batch benchmark: md5_batch disagrees with md5_fast");
            return;
        }
    }

    Timer::Timer timer(true);
    for (int i = 0; i < iterations; ++i) {
        hashlib::md5_batch(inputs, digests);
        DoNotOptimize(digests);
    }
    timer.Stop();

    const double nanosecondsPerCall = timer.ElapsedNs() / static_cast<double>(iterations);
    const double nanosecondsPerHash = nanosecondsPerCall / static_cast<double>(batchSize);

    std::string line = "md5_batch (" + std::string(info.isa) + ", " +
                       std::to_string(info.lanes) + " lane(s)): " + std::to_string(iterations) +
                       " call(s) x " + std::to_string(batchSize) + " hash(es) in " +
                       Timer::FormatDuration(std::chrono::nanoseconds{timer.ElapsedNsInt()}) +
                       ", " +
                       ToFixed(nanosecondsPerCall, 1) + " ns/call, " +
                       ToFixed(nanosecondsPerHash, 1) + " ns/hash";
    if (!input.empty()) {
        // bytes per nanosecond is GB/s
        line += ", " +
                ToFixed(static_cast<double>(batchSize) * static_cast<double>(input.size()) /
                            nanosecondsPerCall,
                        2) +
                " GB/s";
    }
    std::cout << line << '\n';
}

void Benchmark(std::span<const std::byte> input, const std::vector<const Algorithm*>& algorithms,
               int iterations, int batchSize) {
    if (iterations <= 0) {
        LOGIFACE_LOG(warn, "Skipping benchmark: --benchmark-count must be positive");
        return;
    }

    if (batchSize > 0) {
        BenchmarkBatch(input, iterations, batchSize);
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
                           Timer::FormatDuration(std::chrono::nanoseconds{timer.ElapsedNsInt()}) +
                           ", " + ToFixed(nanosecondsPerCall, 1) + " ns/call";
        if (!input.empty()) {
            // bytes per nanosecond is GB/s
            line += ", " + ToFixed(static_cast<double>(input.size()) / nanosecondsPerCall, 2) +
                    " GB/s";
        }
        std::cout << line << '\n';
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

// ---------------------------------------------------------------------------
// solve mode
// ---------------------------------------------------------------------------

namespace {

struct SolveOptions {
    std::string digest;
    std::string same_as;
    std::string anagram_of;
    std::string wordlist;
    std::string solver = "auto";
    std::uint64_t max_candidates = 0;
    bool explain = false;
    bool dry_run = false;
    bool list_solvers = false;
};

// `--wordlist '~/Downloads/wordlist'` is a common way to pass a path from a shell where the tilde was quoted.
// Expand a leading `~` instead of handing a literal tilde to the filesystem.
std::filesystem::path ExpandHome(const std::filesystem::path& path) {
    const std::string text = path.string();
    if (text.empty() || text.front() != '~') {
        return path;
    }
    if (text.size() > 1 && text[1] != '/' && text[1] != '\\') {
        return path;
    }

#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (home == nullptr) {
        home = std::getenv("HOME");
    }
#else
    const char* home = std::getenv("HOME");
#endif
    if (home == nullptr || *home == '\0') {
        return path;
    }
    if (text.size() == 1) {
        return std::filesystem::path{home};
    }
    return std::filesystem::path{home} / text.substr(2);
}

std::optional<hashlib::Digest> ResolveTarget(const SolveOptions& options, std::string& error) {    if (!options.digest.empty() && !options.same_as.empty()) {
        error = "--digest and --same-as are mutually exclusive";
        return std::nullopt;
    }
    if (!options.digest.empty()) {
        const std::optional<hashlib::Digest> digest = solve::ParseDigest(options.digest);
        if (!digest) {
            error = "--digest must be exactly 32 hexadecimal characters";
        }
        return digest;
    }
    if (!options.same_as.empty()) {
        const std::span<const std::byte> bytes =
            std::as_bytes(std::span{options.same_as.data(), options.same_as.size()});
        return hashlib::md5_fast(bytes);
    }
    error = "one of --digest or --same-as is required";
    return std::nullopt;
}

int RunSolve(const SolveOptions& options, CLI::App& app) {
    if (options.list_solvers) {
        for (const solve::SolverInfo& info : solve::Solvers()) {
            std::cout << info.id << "  " << info.summary
                      << (info.complete ? "  [exhaustive]" : "  [wordlist only]") << '\n';
        }
        return 0;
    }

    std::string error;
    const std::optional<hashlib::Digest> target = ResolveTarget(options, error);
    if (!target) {
        return app.exit(CLI::ValidationError("solve", error));
    }

    solve::SearchProblem problem;
    problem.target.digest = *target;
    problem.anagram = solve::Anagram{options.anagram_of};
    if (!options.wordlist.empty()) {
        problem.wordlist =
            solve::Wordlist{ExpandHome(std::filesystem::path{options.wordlist})};
    }
    problem.max_candidates = options.max_candidates;

    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, options.solver, error);
    if (!plan) {
        return app.exit(CLI::ValidationError("solve", error));
    }

    if (options.explain || options.dry_run) {
        LOGIFACE_LOG(info, "solver: " + std::string(plan->solver) + " (" + plan->reason + ")");
    }
    if (plan->solver == "permutations" && plan->estimated_candidates > 50'000'000 &&
        options.max_candidates == 0) {
        LOGIFACE_LOG(warn, "about " + std::to_string(plan->estimated_candidates) +
                               " permutations will be tried; pass --max-candidates to bound "
                               "the search or --wordlist to use a dictionary");
    }
    if (options.dry_run) {
        return 0;
    }
    if (!options.same_as.empty() && options.same_as == options.anagram_of) {
        LOGIFACE_LOG(warn,
                     "--same-as equals --anagram-of; the identity permutation is a candidate "
                     "and will match immediately");
    }

    LOGIFACE_LOG(info, "searching with the " + std::string(plan->solver) + " solver");
    const solve::Outcome outcome = solve::Solve(problem, options.solver);

    switch (outcome.status) {
        case solve::Status::found:
            std::cout << outcome.solution->candidate << '\n';
            LOGIFACE_LOG(info, "found after " + std::to_string(outcome.stats.candidates) +
                                   " hash(es) (" + std::to_string(outcome.stats.scanned) +
                                   " search step(s)) in " +
                                   Timer::FormatDuration(outcome.stats.elapsed));
            return 0;
        case solve::Status::exhausted:
            LOGIFACE_LOG(error, outcome.message);
            if (plan->solver == "dictionary") {
                LOGIFACE_LOG(info,
                             "the answer may not be in the wordlist; rerun with "
                             "--solver permutations for an exhaustive search");
            }
            return 1;
        case solve::Status::invalid:
            return app.exit(CLI::ValidationError("solve", outcome.message));
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Compute the MD5 digest of a string, or solve an anagram-hash challenge"};

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

    int benchmark_batch = 16;
    app.add_option("--benchmark-batch", benchmark_batch,
                   "Messages per md5_batch call in the benchmark (0 disables the batch "
                   "benchmark)")
        ->default_val(16)
        ->check(CLI::NonNegativeNumber)
        ->capture_default_str();

    // `solve` is a subcommand so its options only appear in its own help.
    // The parent positional `text` and options are still usable, and fallthrough() lets parent flags such as --log-level appear after the subcommand.
    auto* solve_command = app.add_subcommand(
        "solve", "Find a string with a given MD5 digest (anagram constraint, optional wordlist)");
    solve_command->fallthrough();

    std::string solve_digest;
    solve_command->add_option("--digest", solve_digest,
                              "Target MD5 digest as exactly 32 hexadecimal characters");
    std::string solve_same_as;
    solve_command->add_option("--same-as", solve_same_as,
                              "Use the MD5 of this string as the target instead of --digest");
    std::string solve_anagram;
    solve_command->add_option("--anagram-of", solve_anagram,
                              "Candidates must have exactly these characters; spaces select "
                              "the phrase solver (which tries 2-8 word counts)");
    std::string solve_wordlist;
    solve_command->add_option(
        "--wordlist", solve_wordlist,
        "Dictionary file: plain text (one entry per line), CSV/TSV or a JSON array; "
        "leading ~ is expanded");
    std::string solve_solver = "auto";
    solve_command
        ->add_option("--solver", solve_solver,
                     "Search strategy: auto, permutations, dictionary or phrase")
        ->check(CLI::IsMember({"auto", "permutations", "dictionary", "phrase"},
                              CLI::ignore_case))
        ->capture_default_str();
    std::uint64_t solve_max_candidates = 0;
    solve_command
        ->add_option("--max-candidates", solve_max_candidates,
                     "Stop after hashing this many candidates (0 = no limit)")
        ->capture_default_str();
    bool solve_explain = false;
    solve_command->add_flag("--explain", solve_explain,
                            "Print the chosen solver and its cost estimate");
    bool solve_dry_run = false;
    solve_command->add_flag("--dry-run", solve_dry_run, "Plan only; do not search");
    bool solve_list_solvers = false;
    solve_command->add_flag("--list-solvers", solve_list_solvers,
                            "List the available solvers and exit");

    app.require_subcommand(0, 1);

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

    if (solve_command->parsed()) {
        if (benchmark) {
            return app.exit(CLI::ValidationError(
                "--benchmark", "--benchmark is only available in hash mode"));
        }
        if (!algorithm_list.empty()) {
            LOGIFACE_LOG(warn,
                         "-a/--algorithm selects a hash implementation in hash mode; solve mode "
                         "always uses md5_batch");
        }

        SolveOptions options;
        options.digest = std::move(solve_digest);
        options.same_as = std::move(solve_same_as);
        options.anagram_of = std::move(solve_anagram);
        options.wordlist = std::move(solve_wordlist);
        options.solver = std::move(solve_solver);
        options.max_candidates = solve_max_candidates;
        options.explain = solve_explain;
        options.dry_run = solve_dry_run;
        options.list_solvers = solve_list_solvers;
        return RunSolve(options, app);
    }

    Selection selection = SelectAlgorithms(algorithm_list);
    if (!selection.error.empty()) {
        return app.exit(CLI::ValidationError("--algorithm", selection.error));
    }

    LOGIFACE_LOG(debug, "Running " + std::to_string(selection.algorithms.size()) +
                            " implementation(s): " + JoinSelectedNames(selection.algorithms));

    const std::span<const std::byte> input = std::as_bytes(std::span{text.data(), text.size()});

    PrintDigests(input, selection.algorithms);

    if (benchmark) {
        Benchmark(input, selection.algorithms, benchmark_count, benchmark_batch);
    }

    LOGIFACE_LOG(info, "App completed");

    return 0;
}

void initializeLogger(Logiface::Level level) {
    static Logiface::ConsoleLogger app_logger;
    app_logger.SetLevel(level);
    Logiface::SetLogger(&app_logger);
}
