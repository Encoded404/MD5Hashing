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
    std::uint64_t max_candidates = 0;
};

enum class Status {
    found,
    exhausted,
    invalid,
};

struct Stats {
    /// Candidates that were actually hashed.
    std::uint64_t candidates = 0;
    /// Permutations generated, or wordlist entries read (whichever the solver does).
    std::uint64_t scanned = 0;
    std::chrono::nanoseconds elapsed{0};
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
    /// Upper bound on hashed candidates (for permutations, the exact count).
    std::uint64_t estimated_candidates = 0;
    /// Work the strategy performs before/while hashing (lines read, etc.).
    std::uint64_t estimated_scanned = 0;
};

/// Every strategy this build knows about, in display order.
std::span<const SolverInfo> Solvers();

/// Chooses a strategy without running it. `requested` is "auto" or a solver id.
/// Returns nullopt and fills `error` when no strategy fits the problem.
std::optional<Plan> PlanSearch(const SearchProblem& problem, std::string_view requested,
                               std::string& error);

/// Runs the selected strategy. `requested` is "auto" or a solver id.
Outcome Solve(const SearchProblem& problem, std::string_view requested = "auto");

/// Parses exactly 32 hexadecimal characters (either case).
std::optional<Digest> ParseDigest(std::string_view hex);

std::string ToHex(std::span<const std::byte> bytes);

/// Number of distinct permutations of `letters` (n! divided by the factorials
/// of the repeated characters). Saturates at UINT64_MAX instead of overflowing.
std::uint64_t CountDistinctPermutations(std::string_view letters);

/// True when both strings contain the same bytes with the same multiplicities.
bool IsAnagram(std::string_view left, std::string_view right);

}  // namespace solve
