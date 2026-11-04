export module solve;

import std;
export import solve.types;

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
