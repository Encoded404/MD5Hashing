export module solve;

import std;
import hashlib;

/// Anagrams-as-preimages search.
///
/// The problem this module solves is deliberately narrow:
/// given the MD5 digest of some string, and the letters that string is made of, recover the string.
/// The letters are a hard constraint (the candidate must be an anagram of them), the digest is the exact target.
///
/// A wordlist is optional and enables two further strategies:
/// scan a dictionary for a single word with the right multiset.
/// compose a multi-word phrase from dictionary words, when the anagram contains spaces.
///
/// That is Trustpilot's anagram challenge, but nothing in this module is Trustpilot-specific.
export namespace solve {

/// The 16-byte MD5 digest the search compares against.
using Digest = hashlib::Digest;

/// The exact digest a candidate must produce.
struct Target {
    Digest digest{};
};

/// Candidates must use exactly the bytes of `letters` (multiset equality).
struct Anagram {
    std::string letters;
};

/// Optional dictionary. Supplying one makes the `dictionary` solver eligible.
/// The reader makes a best effort at the usual wordlist shapes:
/// one entry per line, CSV/TSV, comma/whitespace separated, or a JSON array of strings.
struct Wordlist {
    std::filesystem::path path;
};

struct SearchProblem {
    Target target;
    std::optional<Anagram> anagram;
    std::optional<Wordlist> wordlist;
    /// Stop after hashing this many candidates. 0 means no limit.
    /// The limit is checked between hash batches, so a batch in flight can
    /// still push the count past it.
    std::uint64_t max_candidates = 0;
};

enum class Status {
    found,
    exhausted,
    invalid,
};

/// Longest phrase, in words, that the phrase solver composes.
inline constexpr std::size_t kMaxPhraseWords = 8;

/// Per-phase wall-clock cost of one solve.
///
/// The fields exist in every configuration so `Stats` keeps one layout, but the
/// clock is only read when the build defines `SOLVE_TIMING` (Debug and
/// RelWithDebInfo, see src/SolveLib/CMakeLists.txt). Release compiles every
/// measurement out and leaves these at zero, so the search loop is unchanged.
struct Timing {
    /// The solver-specific work behind `generate`. One solver fills one of
    /// `permute`/`scan`+`filter`/`dfs`; `prepare` is common to all of them.
    struct Generate {
        /// Permutations solver: `next_permutation` plus emitting the candidate.
        std::chrono::nanoseconds permute{0};
        /// Dictionary solver: reading and tokenizing the wordlist.
        std::chrono::nanoseconds scan{0};
        /// Dictionary solver: the length/multiset check and emitting accepted
        /// tokens.
        std::chrono::nanoseconds filter{0};
        /// Phrase solver: depth-first composition of candidate phrases.
        std::chrono::nanoseconds dfs{0};
        /// Phrase solver: `dfs` split by phrase length. Index 0 is a two-word
        /// phrase, index `kMaxPhraseWords - 2` is `kMaxPhraseWords` words.
        std::array<std::chrono::nanoseconds, kMaxPhraseWords - 1> dfs_by_word_count{};
        /// Filling the span array handed to `hashlib::md5_batch`.
        std::chrono::nanoseconds prepare{0};

        [[nodiscard]] std::chrono::nanoseconds total() const noexcept {
            return permute + scan + filter + dfs + prepare;
        }
    };

    /// Choosing the strategy, including whatever wordlist counting the choice needs.
    /// Explicit solvers and the phrase strategy skip that count.
    std::chrono::nanoseconds planning{0};
    /// Building the candidate source:
    /// sorting the permutation seed.
    /// opening, reading and indexing the wordlist for the phrase solver.
    ///
    /// The phrase solver builds pair completions on demand as the search reaches two-word states, and builds the full pair table once it has asked for enough of them.
    /// Some of this number is therefore measured after the search has started, but it is still indexing, not searching.
    std::chrono::nanoseconds setup{0};
    /// Producing candidates, broken down by what the selected solver spends its
    /// time on.
    Generate generate{};
    /// Time inside `hashlib::md5_batch`.
    std::chrono::nanoseconds hash{0};
    /// Comparing candidate digests against the target.
    std::chrono::nanoseconds match{0};
    /// Wall-clock time of the whole `Solve` call. When the caller supplies a
    /// `Plan`, the planning that produced it happened before this call.
    std::chrono::nanoseconds total{0};
};

/// Work counters for the phrase solver's depth-first search. Counting is a
/// single increment per event (no clock reads), but like `Timing` it is only
/// recorded when `SOLVE_TIMING` is enabled; Release leaves these at zero.
struct Counters {
    /// Words examined by an internal node's loop (one `Fits` call each).
    std::uint64_t fits_calls = 0;
    /// `Fits` returned true and the word passed the letter-count check.
    std::uint64_t fits_passes = 0;
    /// Recursive DFS calls made after pruning.
    std::uint64_t edges = 0;
    /// Two-word subtrees resolved by the pair-completion index (one lookup
    /// each). Without two words left this is zero.
    std::uint64_t leaf_visits = 0;
    /// Word pairs examined in those subtrees.
    std::uint64_t leaf_candidates = 0;
    /// Word pairs whose letter counts matched exactly. The index only holds
    /// exact completions, so this equals `leaf_candidates`.
    std::uint64_t same_counts_hits = 0;
    /// Complete phrases handed to the hash batch.
    std::uint64_t phrases_emitted = 0;
};

struct Stats {
    /// Candidates that were actually hashed.
    std::uint64_t candidates = 0;
    /// Permutations generated, or wordlist entries read (whichever the solver does).
    std::uint64_t scanned = 0;
    /// Duration of the search loop (excluding planning and setup, including
    /// setup work the phrase solver defers until the search actually needs it).
    std::chrono::nanoseconds elapsed{0};
    /// Per-phase breakdown. Zero in Release builds, where it is not measured.
    Timing timing{};
    /// Phrase-solver work counters. Zero in Release builds.
    Counters counters{};
};

struct Solution {
    std::string candidate;
    Digest digest{};
    std::string solver;
};

struct Outcome {
    Status status = Status::invalid;
    std::optional<Solution> solution;
    Stats stats;
    std::string message;
};

/// Static description of a search strategy. Adding a strategy means adding a
/// row to the table in solve.cpp; the CLI and the selector derive from it.
struct SolverInfo {
    std::string_view id;
    std::string_view summary;
    bool needs_anagram = true;
    bool needs_wordlist = false;
    /// True when the strategy is exhaustive over the anagram class, false when
    /// it can only find answers present in the supplied wordlist.
    bool complete = true;
};

struct Plan {
    std::string_view solver;
    std::string reason;
    /// Upper bound on hashed candidates when it is cheap to know it (for permutations, the exact count).
    /// Zero when the strategy has no useful pre-search bound, or when the wordlist was not counted.
    std::uint64_t estimated_candidates = 0;
    /// Work the strategy performs before/while hashing when it is known (lines read, etc.).
    /// Zero when the solver counts entries during the search.
    std::uint64_t estimated_scanned = 0;
};

/// Every strategy this build knows about, in display order.
std::span<const SolverInfo> Solvers();

/// Chooses a strategy without running it. `requested` is "auto" or a solver id.
/// Returns nullopt and fills `error` when no strategy fits the problem.
///
/// Counting wordlist entries is a full scan, so it is only done when the automatic single-word choice needs the comparison.
/// It stops once the permutation count is reached.
/// An explicit dictionary or phrase request only validates the file header here, and its estimates are left at zero.
std::optional<Plan> PlanSearch(const SearchProblem& problem, std::string_view requested,
                               std::string& error);

/// Runs the selected strategy. `requested` is "auto" or a solver id.
Outcome Solve(const SearchProblem& problem, std::string_view requested = "auto");

/// Runs a strategy chosen by PlanSearch.
///
/// Passing the plan back avoids scanning the wordlist a second time (PlanSearch
/// counts entries before the search starts). The plan must describe this
/// problem; `Solve(problem, requested)` is the all-in-one form.
Outcome Solve(const SearchProblem& problem, const Plan& plan);

/// Parses exactly 32 hexadecimal characters (either case).
std::optional<Digest> ParseDigest(std::string_view hex);

std::string ToHex(std::span<const std::byte> bytes);

/// Number of distinct permutations of `letters` (n! divided by the factorials
/// of the repeated characters). Saturates at UINT64_MAX instead of overflowing.
std::uint64_t CountDistinctPermutations(std::string_view letters);

/// True when both strings contain the same bytes with the same multiplicities.
bool IsAnagram(std::string_view left, std::string_view right);

}  // namespace solve
