module solve;

import std;
import solve.types;
import solve.detail;

namespace solve {

namespace {

std::string LowerAscii(std::string_view text) {
    std::string lower;
    lower.reserve(text.size());
    for (const char ch : text) {
        lower.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    return lower;
}

// The table the selector, the CLI help and --list-solvers derive from.
// Adding a strategy means adding a row here, a run entry point in engine.cpp or phrase.cpp, and a branch in SolveWithPlan.
constexpr std::array kSolvers{
    solve::SolverInfo{"permutations",
                      "enumerate the distinct anagrams and hash each",
                      true,
                      false,
                      true},
    solve::SolverInfo{"dictionary",
                      "scan a wordlist, keep the anagrams, hash those",
                      true,
                      true,
                      false},
    solve::SolverInfo{"phrase",
                      "compose a multi-word anagram from wordlist words",
                      true,
                      true,
                      false},
    solve::SolverInfo{"phrase-v1",
                      "the first phrase DFS, on the current SIMD hash batch",
                      true,
                      true,
                      false},
};

}  // namespace

std::span<const SolverInfo> Solvers() {
    return {kSolvers.data(), kSolvers.size()};
}

std::optional<Plan> PlanSearch(const SearchProblem& problem, std::string_view requested,
                               std::string& error) {
    error.clear();

    if (!problem.anagram) {
        error = "solving requires an anagram (--anagram-of)";
        return std::nullopt;
    }
    if (problem.anagram->letters.empty()) {
        error = "solving requires --anagram-of (the letters to anagram)";
        return std::nullopt;
    }

    const std::string requested_lower = LowerAscii(requested);
    const bool automatic = requested_lower.empty() || requested_lower == "auto";
    const bool wants_permutations = requested_lower == "permutations";
    const bool wants_dictionary = requested_lower == "dictionary";
    const bool wants_phrase = requested_lower == "phrase";
    const bool wants_phrase_v1 = requested_lower == "phrase-v1";
    if (!automatic && !wants_permutations && !wants_dictionary && !wants_phrase &&
        !wants_phrase_v1) {
        error = "unknown solver '" + std::string(requested) +
                "'; available: permutations, dictionary, phrase, phrase-v1, auto";
        return std::nullopt;
    }

    if (wants_dictionary && !problem.wordlist) {
        error = "solver 'dictionary' requires a wordlist (--wordlist)";
        return std::nullopt;
    }
    if ((wants_phrase || wants_phrase_v1) && !problem.wordlist) {
        error = "solver '" + std::string(wants_phrase_v1 ? "phrase-v1" : "phrase") +
                "' requires a wordlist (--wordlist)";
        return std::nullopt;
    }

    const bool multi_word = problem.anagram->letters.find(' ') != std::string::npos;
    if (multi_word && wants_dictionary) {
        error = "the anagram contains spaces; use --solver phrase";
        return std::nullopt;
    }
    if (multi_word && automatic && !problem.wordlist) {
        error = "a multi-word anagram requires --wordlist (the permutation space is too "
                "large to enumerate)";
        return std::nullopt;
    }
    if (!multi_word && (wants_phrase || wants_phrase_v1)) {
        error = "the phrase solver needs a multi-word anagram (at least one space)";
        return std::nullopt;
    }

    const std::uint64_t permutations = CountDistinctPermutations(problem.anagram->letters);

    // Counting wordlist entries is the expensive part of planning, and only the automatic single-word choice needs it.
    // For an explicit solver the count is not a decision input, and the phrase solver counts while it indexes.
    // Even when it is needed, the comparison is decided once the count reaches the number of permutations, so a large wordlist is read only far enough to answer it.
    std::uint64_t entries = 0;
    bool entries_exact = false;
    if (problem.wordlist && !wants_permutations) {
        detail::WordlistReader reader(problem.wordlist->path);
        if (!reader.error().empty()) {
            error = reader.error();
            return std::nullopt;
        }
        if (!reader.is_open()) {
            error = "cannot open wordlist: " + problem.wordlist->path.string();
            return std::nullopt;
        }

        if (automatic && !multi_word) {
            std::error_code size_error;
            const std::uintmax_t bytes =
                std::filesystem::file_size(problem.wordlist->path, size_error);
            // Every entry needs at least one byte, and consecutive entries are separated by a delimiter.
            // A file of N bytes therefore holds at most ceil(N / 2) entries.
            const std::uintmax_t max_entries = bytes / 2 + bytes % 2;
            if (!size_error && max_entries < permutations) {
                // This list is cheaper than the permutations unless it is empty, and one token answers that.
                std::string token;
                entries = reader.next(token) ? 1 : 0;
                if (!reader.error().empty()) {
                    error = reader.error();
                    return std::nullopt;
                }
                entries_exact = entries == 0;
            } else {
                const auto counted = detail::CountWordlistEntries(reader, permutations, error);
                if (!counted) {
                    return std::nullopt;
                }
                entries = counted->entries;
                entries_exact = !counted->reached_limit;
            }
        }
        // For every other strategy the header probe above only validated the file; the solver counts while it scans, or does not need a count.
    }

    Plan plan;
    if (wants_permutations) {
        plan.solver = "permutations";
        plan.estimated_candidates = permutations;
        plan.estimated_scanned = permutations;
        plan.reason = "requested; " + std::to_string(permutations) + " distinct permutation(s)";
        if (problem.wordlist) {
            plan.reason += " (the wordlist is not used by this solver)";
        }
        return plan;
    }

    if (wants_dictionary) {
        plan.solver = "dictionary";
        // The scan is the search; there is no separate pre-search count.
        plan.estimated_candidates = 0;
        plan.estimated_scanned = 0;
        plan.reason = "requested; the wordlist is scanned during the search";
        return plan;
    }

    if (multi_word && (automatic || wants_phrase || wants_phrase_v1)) {
        plan.solver = wants_phrase_v1 ? "phrase-v1" : "phrase";
        // The number of phrases is not known without running the search.
        // The phrase solver counts entries while it builds its index.
        plan.estimated_candidates = 0;
        plan.estimated_scanned = 0;
        plan.reason =
            (automatic ? "auto: " : "requested; ") +
            std::string(wants_phrase_v1
                            ? "first-version phrase DFS on the current SIMD hash batch"
                            : "multi-word phrase composed from the wordlist");
        return plan;
    }

    // automatic
    if (!problem.wordlist) {
        plan.solver = "permutations";
        plan.estimated_candidates = permutations;
        plan.estimated_scanned = permutations;
        plan.reason = "no wordlist; " + std::to_string(permutations) +
                      " distinct permutation(s) to try";
        return plan;
    }

    if (entries == 0) {
        plan.solver = "permutations";
        plan.estimated_candidates = permutations;
        plan.estimated_scanned = permutations;
        plan.reason = "the wordlist is empty; falling back to " + std::to_string(permutations) +
                      " distinct permutation(s)";
        return plan;
    }

    if (permutations <= entries) {
        plan.solver = "permutations";
        plan.estimated_candidates = permutations;
        plan.estimated_scanned = permutations;
        if (entries_exact) {
            plan.reason = "auto: " + std::to_string(entries) +
                          " wordlist entries are not cheaper than " +
                          std::to_string(permutations) + " permutation(s)";
        } else {
            plan.reason = "auto: the wordlist has at least " +
                          std::to_string(permutations) +
                          " entries, so the permutations are cheaper";
        }
        return plan;
    }

    plan.solver = "dictionary";
    plan.estimated_candidates = entries_exact ? entries : 0;
    plan.estimated_scanned = entries_exact ? entries : 0;
    if (entries_exact) {
        plan.reason = "auto: " + std::to_string(entries) +
                      " wordlist entries are cheaper than " +
                      std::to_string(permutations) + " permutation(s)";
    } else {
        plan.reason = "auto: the wordlist has fewer than " +
                      std::to_string(permutations) +
                      " entries, so scanning it is cheaper";
    }
    return plan;
}

}  // namespace solve
