export module solve.detail;

import std;
import solve.types;
import Timer;

/// Shared internals of the solve library.
///
/// This module is not part of the public API: the app and the tests import `solve`, never this.
/// It exists so the wordlist reader, the phrase word record, and the entry points a plan can name cross a translation unit boundary without appearing in solve.cppm.
export namespace solve::detail {

/// Candidates per generic hash batch.
inline constexpr std::size_t kBatchSize = 64;
/// The phrase solver's distinct-character limit.
inline constexpr std::size_t kMaxPhraseUnique = 32;
/// Longest phrase, in words, that the phrase solvers compose.
inline constexpr std::size_t kMaxPhraseWords = solve::kMaxPhraseWords;
/// Value of `index_of_` for a character outside the anagram.
inline constexpr std::uint8_t kNoIndex = 0xFF;
/// Prefix budget shared by both phrase searches.
inline constexpr std::uint64_t kPhraseNodeBudget = 100'000'000;
inline constexpr std::uint64_t kMaxU64 = std::numeric_limits<std::uint64_t>::max();
/// Bytes of a word copied by the phrase writer's fixed-size move.
/// The store may run past the phrase; SlotStride in phrase.cpp reserves the room.
inline constexpr std::size_t kWordCopyBytes = 16;

#if defined(SOLVE_TIMING) && SOLVE_TIMING
inline constexpr bool kTimingEnabled = true;
#else
inline constexpr bool kTimingEnabled = false;
#endif

/// One word from the phrase solver's index.
struct PhraseWord {
    std::string text;
    std::array<std::uint16_t, kMaxPhraseUnique> counts{};
    std::uint64_t key = 0;
    /// Bits of the letters present in `text`, for one-AND exhaustion checks.
    std::uint64_t mask = 0;
    /// The counts packed five bits per letter, so a fit test is one subtraction.
    /// Only filled in when `PhraseSearch::packed_counts_` says the layout is safe.
    std::uint64_t packed = 0;
    /// `text.size()`, kept as a plain word: asking the string for its length costs a short/long-mode branch on every use.
    std::uint32_t size = 0;
    /// The first kWordCopyBytes characters of `text`, zero-padded, so emitting a
    /// phrase is a constant-size store rather than a memcpy call.
    std::array<char, kWordCopyBytes> head{};
};

/// Reads a wordlist as a stream of tokens, making a best effort at the shapes wordlists come in:
/// one entry per line.
/// CSV/TSV files.
/// comma/whitespace separated lists.
/// JSON arrays/objects (quoted strings and punctuation are stripped by the same tokenizer).
/// UTF-8 with or without BOM and LF/CRLF/CR endings all work.
/// Compressed and UTF-16 files are rejected with an actionable message, rather than being silently read as mojibake.
///
/// The file is read in chunks and tokenized in place.
/// A std::getline loop calls through the stream buffer once per line, copies the line, then scans it again.
/// For a dictionary-sized list, that loop was several times the cost of the setup work it feeds.
/// One pass over a 256 KiB buffer removes both copies and the per-line calls while keeping memory bounded for very large wordlists.
class WordlistReader {
public:
    explicit WordlistReader(const std::filesystem::path& path);

    [[nodiscard]] bool is_open() const { return file_.is_open(); }
    [[nodiscard]] const std::string& error() const { return error_; }

    /// Returns the next non-empty token, or false at the end of the file.
    bool next(std::string& token);

private:
    static constexpr std::size_t kReadChunk = 1 << 18;

    static bool IsDelimiter(char ch);

    /// Reads more bytes, moving any unread tail to the front.
    /// Returns false only when no unread byte is available any more.
    bool fill();

    void skip_comment();

    std::ifstream file_;
    std::vector<char> buffer_;
    std::size_t pos_ = 0;
    std::size_t end_ = 0;
    bool eof_ = false;
    /// True between the start of a line and its first token character.
    /// Comments are recognised only here, exactly as before.
    bool at_line_start_ = true;
    std::string error_;
};

struct WordlistCount {
    std::uint64_t entries = 0;
    /// True when counting stopped at `limit`.
    /// The real count is at least `limit` but otherwise unknown.
    /// The selector only needs the comparison, so a large wordlist does not have to be read to the end.
    bool reached_limit = false;
};

/// Counts the tokens behind `reader`, stopping at `limit` (which must be non-zero).
/// The reader has already been validated by the caller.
std::optional<WordlistCount> CountWordlistEntries(WordlistReader& reader,
                                                  std::uint64_t limit,
                                                  std::string& error);

/// Runs the permutation solver.
/// `setup_timer` covers sorting the seed before the search starts.
solve::Outcome RunPermutations(const solve::SearchProblem& problem,
                               Timer::Timer& setup_timer, solve::Timing& timing);

/// Runs the single-word dictionary solver.
/// The caller's plan has already established that a wordlist is present.
solve::Outcome RunDictionary(const solve::SearchProblem& problem,
                             Timer::Timer& setup_timer, solve::Timing& timing);

/// Runs the optimized phrase solver, filling `timing` and `counters`.
solve::Outcome RunPhrase(const solve::SearchProblem& problem, Timer::Timer& setup_timer,
                         solve::Timing& timing, solve::Counters& counters);

/// Runs the first-version phrase solver, kept as a benchmark baseline.
solve::Outcome RunPhraseV1(const solve::SearchProblem& problem, Timer::Timer& setup_timer,
                           solve::Timing& timing, solve::Counters& counters);

}  // namespace solve::detail
