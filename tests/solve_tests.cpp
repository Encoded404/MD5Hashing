// gtest pulls standard headers in textually. Include it before `import std;`
// for the same reason as hashlib_tests.cpp: the reverse order collides with the
// declarations owned by the std module.
#include <gtest/gtest.h>

import std;

import hashlib;
import solve;

namespace {

hashlib::Digest DigestOf(std::string_view text) {
    return hashlib::md5_fast(std::as_bytes(std::span{text.data(), text.size()}));
}

// A wordlist file that removes itself. The tests only need a handful of lines.
class TempWordlist {
public:
    explicit TempWordlist(std::string_view contents) {
        static std::atomic<std::uint64_t> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("hashsolve_test_" + std::to_string(stamp) + "_" + std::to_string(counter++));
        std::ofstream out(path_, std::ios::binary);
        out << contents;
    }

    ~TempWordlist() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    TempWordlist(const TempWordlist&) = delete;
    TempWordlist& operator=(const TempWordlist&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

solve::Outcome SolveWithWordlist(std::string_view contents, std::string_view target,
                                 std::string_view letters,
                                 std::string_view solver = "dictionary") {
    const TempWordlist words{contents};

    solve::SearchProblem problem;
    problem.target.digest = DigestOf(target);
    problem.anagram = solve::Anagram{std::string{letters}};
    problem.wordlist = solve::Wordlist{words.path()};
    return solve::Solve(problem, solver);
}

}  // namespace

TEST(SolveTest, ParsesDigest) {
    const std::optional<hashlib::Digest> parsed =
        solve::ParseDigest("839D7228b5ccc60e54455093a966b8c2");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(solve::ToHex(*parsed), "839d7228b5ccc60e54455093a966b8c2");

    EXPECT_FALSE(solve::ParseDigest("").has_value());
    EXPECT_FALSE(solve::ParseDigest("839d72").has_value());
    EXPECT_FALSE(solve::ParseDigest("839d7228b5ccc60e54455093a966b8c").has_value());
    EXPECT_FALSE(solve::ParseDigest("839d7228b5ccc60e54455093a966b8cg").has_value());
}

TEST(SolveTest, CountsDistinctPermutations) {
    EXPECT_EQ(solve::CountDistinctPermutations(""), 1u);
    EXPECT_EQ(solve::CountDistinctPermutations("a"), 1u);
    EXPECT_EQ(solve::CountDistinctPermutations("ab"), 2u);
    EXPECT_EQ(solve::CountDistinctPermutations("abc"), 6u);
    EXPECT_EQ(solve::CountDistinctPermutations("aab"), 3u);
    EXPECT_EQ(solve::CountDistinctPermutations("mississippi"), 34650u);
}

TEST(SolveTest, RecognizesAnagrams) {
    EXPECT_TRUE(solve::IsAnagram("listen", "silent"));
    EXPECT_TRUE(solve::IsAnagram("", ""));
    EXPECT_FALSE(solve::IsAnagram("listen", "liste"));
    EXPECT_FALSE(solve::IsAnagram("listen", "listem"));
}

TEST(SolveTest, PermutationsRecoverTheWord) {
    solve::SearchProblem problem;
    problem.target.digest = DigestOf("listen");
    problem.anagram = solve::Anagram{"silent"};

    const solve::Outcome outcome = solve::Solve(problem, "permutations");
    ASSERT_EQ(outcome.status, solve::Status::found);
    ASSERT_TRUE(outcome.solution.has_value());
    EXPECT_EQ(outcome.solution->candidate, "listen");
    EXPECT_EQ(outcome.solution->digest, DigestOf("listen"));
    EXPECT_GT(outcome.stats.candidates, 0u);
}

TEST(SolveTest, PermutationsOnlyEmitDistinctArrangements) {
    solve::SearchProblem problem;
    problem.target.digest = DigestOf("aab");
    problem.anagram = solve::Anagram{"aba"};

    const solve::Outcome outcome = solve::Solve(problem, "permutations");
    ASSERT_EQ(outcome.status, solve::Status::found);
    ASSERT_TRUE(outcome.solution.has_value());
    EXPECT_EQ(outcome.solution->candidate, "aab");
    // "aba" has three distinct arrangements, not 3! = 6.
    EXPECT_EQ(outcome.stats.candidates, 3u);
}

TEST(SolveTest, DictionaryRecoversTheWord) {
    const TempWordlist words{"banana\nlisten\nsilent\n# a comment\n\n"};

    solve::SearchProblem problem;
    problem.target.digest = DigestOf("listen");
    problem.anagram = solve::Anagram{"silent"};
    problem.wordlist = solve::Wordlist{words.path()};

    const solve::Outcome outcome = solve::Solve(problem, "dictionary");
    ASSERT_EQ(outcome.status, solve::Status::found);
    ASSERT_TRUE(outcome.solution.has_value());
    EXPECT_EQ(outcome.solution->candidate, "listen");
    EXPECT_GT(outcome.stats.scanned, 0u);
}

TEST(SolveTest, DictionaryRejectsNonAnagrams) {
    // "banana" is in the list but is not an anagram of "silent", so a hash-only scan would still be wrong here.
    const TempWordlist words{"banana\n"};

    solve::SearchProblem problem;
    problem.target.digest = DigestOf("banana");
    problem.anagram = solve::Anagram{"silent"};
    problem.wordlist = solve::Wordlist{words.path()};

    const solve::Outcome outcome = solve::Solve(problem, "dictionary");
    EXPECT_EQ(outcome.status, solve::Status::exhausted);
}

TEST(SolveTest, DictionaryAcceptsCommaSeparated) {
    const solve::Outcome outcome =
        SolveWithWordlist("banana,listen,silent\n", "listen", "silent");
    ASSERT_EQ(outcome.status, solve::Status::found);
    EXPECT_EQ(outcome.solution->candidate, "listen");
}

TEST(SolveTest, DictionaryAcceptsCsvWithHeaderAndQuotes) {
    const solve::Outcome outcome = SolveWithWordlist(
        "word,frequency\n\"listen\",42\n\"silent\",7\n\"banana\",1\n", "listen", "silent");
    ASSERT_EQ(outcome.status, solve::Status::found);
    EXPECT_EQ(outcome.solution->candidate, "listen");
}

TEST(SolveTest, DictionaryAcceptsHunspellStyleEntries) {
    // Hunspell .dic files append affix flags after a slash.
    const solve::Outcome outcome =
        SolveWithWordlist("2\nlisten/AB\nsilent/XY\n", "listen", "silent");
    ASSERT_EQ(outcome.status, solve::Status::found);
    EXPECT_EQ(outcome.solution->candidate, "listen");
}

TEST(SolveTest, PhraseComposesWords) {
    const solve::Outcome outcome =
        SolveWithWordlist("listen\nsilent\nto\nme\n", "listen to me", "silent to me", "phrase");
    ASSERT_EQ(outcome.status, solve::Status::found);
    ASSERT_TRUE(outcome.solution.has_value());
    EXPECT_EQ(outcome.solution->candidate, "listen to me");
}

TEST(SolveTest, PhraseHandlesTwoWords) {
    const solve::Outcome outcome =
        SolveWithWordlist("listen\nto\n", "listen to", "silent to", "phrase");
    ASSERT_EQ(outcome.status, solve::Status::found);
    EXPECT_EQ(outcome.solution->candidate, "listen to");
}

TEST(SolveTest, PhraseCanReuseAWord) {
    const solve::Outcome outcome = SolveWithWordlist("me\n", "me me", "em em", "phrase");
    ASSERT_EQ(outcome.status, solve::Status::found);
    EXPECT_EQ(outcome.solution->candidate, "me me");
}

TEST(SolveTest, PhraseHandlesFourWords) {
    const solve::Outcome outcome =
        SolveWithWordlist("a\nb\nc\nd\n", "a b c d", "a b c d", "phrase");
    ASSERT_EQ(outcome.status, solve::Status::found);
    EXPECT_EQ(outcome.solution->candidate, "a b c d");
}

TEST(SolveTest, PhraseDoesNotUseClueWordCount) {
    // The clue has two words, the target has three.
    // The phrase solver must strip the clue's spaces and try several word counts.
    const solve::Outcome outcome =
        SolveWithWordlist("listen\nsilent\nto\nme\n", "listen to me", "silent tome", "phrase");
    ASSERT_EQ(outcome.status, solve::Status::found);
    EXPECT_EQ(outcome.solution->candidate, "listen to me");
}

TEST(SolveTest, AutoChoosesPhraseForMultiWordAnagram) {
    const TempWordlist words{"listen\nsilent\nto\nme\n"};

    solve::SearchProblem problem;
    problem.target.digest = DigestOf("listen to me");
    problem.anagram = solve::Anagram{"silent to me"};
    problem.wordlist = solve::Wordlist{words.path()};

    std::string error;
    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, "auto", error);
    ASSERT_TRUE(plan.has_value()) << error;
    EXPECT_EQ(plan->solver, "phrase");
}

TEST(SolveTest, MultiWordAutoRequiresAWordlist) {
    solve::SearchProblem problem;
    problem.target.digest = DigestOf("listen to me");
    problem.anagram = solve::Anagram{"silent to me"};

    std::string error;
    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, "auto", error);
    EXPECT_FALSE(plan.has_value());
    EXPECT_NE(error.find("wordlist"), std::string::npos);
}

TEST(SolveTest, DictionaryRejectsMultiWordAnagram) {
    const TempWordlist words{"listen\nsilent\nto\nme\n"};

    solve::SearchProblem problem;
    problem.target.digest = DigestOf("listen to me");
    problem.anagram = solve::Anagram{"silent to me"};
    problem.wordlist = solve::Wordlist{words.path()};

    std::string error;
    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, "dictionary", error);
    EXPECT_FALSE(plan.has_value());
    EXPECT_NE(error.find("phrase"), std::string::npos);
}

TEST(SolveTest, PhraseRejectsSingleWordAnagram) {
    const TempWordlist words{"listen\nsilent\n"};

    solve::SearchProblem problem;
    problem.target.digest = DigestOf("listen");
    problem.anagram = solve::Anagram{"silent"};
    problem.wordlist = solve::Wordlist{words.path()};

    std::string error;
    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, "phrase", error);
    EXPECT_FALSE(plan.has_value());
    EXPECT_NE(error.find("multi-word"), std::string::npos);
}

TEST(SolveTest, DictionaryAcceptsJsonArray) {
    const solve::Outcome outcome =
        SolveWithWordlist("[\"banana\", \"listen\", \"silent\"]", "listen", "silent");
    ASSERT_EQ(outcome.status, solve::Status::found);
    EXPECT_EQ(outcome.solution->candidate, "listen");
}

TEST(SolveTest, DictionaryAcceptsBomAndCrlf) {
    const solve::Outcome outcome = SolveWithWordlist(
        "\xEF\xBB\xBF" "banana\r\nlisten\r\nsilent\r\n", "listen", "silent");
    ASSERT_EQ(outcome.status, solve::Status::found);
    EXPECT_EQ(outcome.solution->candidate, "listen");
}

TEST(SolveTest, AutoCountsWordlistTokensNotLines) {
    // One line, three entries. P("ab") = 2, so permutations should win.
    // If the selector counted lines it would see one entry and pick the dictionary.
    const TempWordlist words{"[\"a\",\"b\",\"c\"]"};

    solve::SearchProblem problem;
    problem.target.digest = DigestOf("ab");
    problem.anagram = solve::Anagram{"ab"};
    problem.wordlist = solve::Wordlist{words.path()};

    std::string error;
    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, "auto", error);
    ASSERT_TRUE(plan.has_value()) << error;
    EXPECT_EQ(plan->solver, "permutations");
}

TEST(SolveTest, RejectsGzipWordlist) {
    const TempWordlist words{std::string_view{"\x1F\x8B", 2}};

    solve::SearchProblem problem;
    problem.target.digest = DigestOf("listen");
    problem.anagram = solve::Anagram{"silent"};
    problem.wordlist = solve::Wordlist{words.path()};

    std::string error;
    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, "auto", error);
    EXPECT_FALSE(plan.has_value());
    EXPECT_NE(error.find("gzip"), std::string::npos);
}

TEST(SolveTest, RejectsUtf16Wordlist) {
    const TempWordlist words{std::string_view{"\xFF\xFE", 2}};

    solve::SearchProblem problem;
    problem.target.digest = DigestOf("listen");
    problem.anagram = solve::Anagram{"silent"};
    problem.wordlist = solve::Wordlist{words.path()};

    std::string error;
    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, "auto", error);
    EXPECT_FALSE(plan.has_value());
    EXPECT_NE(error.find("UTF-16"), std::string::npos);
}

TEST(SolveTest, AutoPrefersPermutationsForSmallSpaces) {
    const TempWordlist words{"a\nb\nc\n"};

    solve::SearchProblem problem;
    problem.target.digest = DigestOf("ab");
    problem.anagram = solve::Anagram{"ab"};
    problem.wordlist = solve::Wordlist{words.path()};

    std::string error;
    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, "auto", error);
    ASSERT_TRUE(plan.has_value()) << error;
    EXPECT_EQ(plan->solver, "permutations");
}

TEST(SolveTest, AutoPrefersDictionaryForLargeSpaces) {
    const TempWordlist words{"a\nb\nc\n"};

    solve::SearchProblem problem;
    problem.target.digest = DigestOf("abcdefghij");
    problem.anagram = solve::Anagram{"abcdefghij"};  // 10! = 3628800 permutations
    problem.wordlist = solve::Wordlist{words.path()};

    std::string error;
    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, "auto", error);
    ASSERT_TRUE(plan.has_value()) << error;
    EXPECT_EQ(plan->solver, "dictionary");
}

TEST(SolveTest, AutoUsesRepeatedLetterCount) {
    // "mississippi" has 11 letters but only 34650 distinct arrangements, so a 40000-line wordlist is still the more expensive strategy.
    std::string contents;
    for (int i = 0; i < 40000; ++i) {
        contents += "x\n";
    }
    const TempWordlist words{contents};

    solve::SearchProblem problem;
    problem.target.digest = DigestOf("mississippi");
    problem.anagram = solve::Anagram{"mississippi"};
    problem.wordlist = solve::Wordlist{words.path()};

    std::string error;
    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, "auto", error);
    ASSERT_TRUE(plan.has_value()) << error;
    EXPECT_EQ(plan->solver, "permutations");
}

TEST(SolveTest, WithoutAWordlistAutoUsesPermutations) {
    solve::SearchProblem problem;
    problem.target.digest = DigestOf("listen");
    problem.anagram = solve::Anagram{"silent"};

    std::string error;
    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, "auto", error);
    ASSERT_TRUE(plan.has_value()) << error;
    EXPECT_EQ(plan->solver, "permutations");
}

TEST(SolveTest, DictionaryRequiresAWordlist) {
    solve::SearchProblem problem;
    problem.target.digest = DigestOf("listen");
    problem.anagram = solve::Anagram{"silent"};

    std::string error;
    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, "dictionary", error);
    EXPECT_FALSE(plan.has_value());
    EXPECT_NE(error.find("wordlist"), std::string::npos);
}

TEST(SolveTest, RejectsUnknownSolver) {
    solve::SearchProblem problem;
    problem.target.digest = DigestOf("listen");
    problem.anagram = solve::Anagram{"silent"};

    std::string error;
    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, "nonsense", error);
    EXPECT_FALSE(plan.has_value());
    EXPECT_NE(error.find("unknown solver"), std::string::npos);
}

TEST(SolveTest, MissingWordlistIsReported) {
    solve::SearchProblem problem;
    problem.target.digest = DigestOf("listen");
    problem.anagram = solve::Anagram{"silent"};
    problem.wordlist = solve::Wordlist{std::filesystem::temp_directory_path() /
                                       "hashsolve_definitely_missing_wordlist"};

    std::string error;
    const std::optional<solve::Plan> plan = solve::PlanSearch(problem, "auto", error);
    EXPECT_FALSE(plan.has_value());
    EXPECT_NE(error.find("cannot open wordlist"), std::string::npos);
}

TEST(SolveTest, HonorsCandidateBudget) {
    solve::SearchProblem problem;
    problem.target.digest = DigestOf("zzzzzzzzzz");
    problem.anagram = solve::Anagram{"abcdefghij"};
    problem.max_candidates = 10;

    const solve::Outcome outcome = solve::Solve(problem, "permutations");
    EXPECT_EQ(outcome.status, solve::Status::exhausted);
    EXPECT_NE(outcome.message.find("budget"), std::string::npos);
    // The budget is checked between batches, so a batch of at most 64 can overshoot it.
    // It must never be unbounded.
    EXPECT_LE(outcome.stats.candidates, 64u + 10u);
}
