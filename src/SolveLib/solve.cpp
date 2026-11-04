module solve;

import std;
import hashlib;
import Timer;

namespace {

constexpr std::size_t kBatchSize = 64;
// The phrase solver's batch only feeds `md5_batch`; it is larger than the
// generic one so the per-batch bookkeeping (and its timing reads) is amortised
// over more candidates. The candidate budget is still enforced per phrase, so
// a batch can overshoot it by at most this many candidates.
constexpr std::size_t kPhraseBatchSize = 256;
constexpr std::size_t kMaxPhraseUnique = 32;
constexpr std::size_t kMaxPhraseWords = solve::kMaxPhraseWords;
constexpr std::uint8_t kNoIndex = 0xFF;
constexpr std::uint64_t kPhraseNodeBudget = 100'000'000;
constexpr std::uint64_t kMaxU64 = std::numeric_limits<std::uint64_t>::max();
// Above this many mixed-radix states the flat leaf index costs too much memory;
// the unordered_map fallback is used instead.
constexpr std::uint64_t kFlatKeyLimit = 1'000'000;
// The pair memo trades an O(words^2) setup pass for removing the last two DFS
// levels. These caps keep that pass (and its memory) bounded on pathological
// wordlists; past them the plain DFS runs instead.
constexpr std::size_t kPairMemoWordLimit = 4096;
constexpr std::size_t kPairMemoEntryLimit = 4'000'000;
// A search that finds its answer early reaches only a handful of two-word states, so building the whole O(words^2) table up front can cost far more than the search.
// Answer the first states on demand, and build the full table once this many distinct states have been asked for.
constexpr std::size_t kPairMemoLazyKeyLimit = 256;
// Phrase emission copies words with a constant-size move so the compiler emits
// a load/store pair instead of calling memcpy for every word. The store may run
// past the phrase; the batch slot reserves kSlotSlack bytes for that.
constexpr std::size_t kWordCopyBytes = 16;
constexpr std::size_t kPrefixCopyBytes = 32;
constexpr std::size_t kSlotSlack = kPrefixCopyBytes;
// The pair memo packs one letter count into five bits of a 64-bit word. Twelve
// letters fit, and counts up to seven keep the packed addition carry-free.
constexpr std::size_t kPackedPhraseLetters = 12;
constexpr std::uint64_t kPhraseFieldHigh = [] {
    std::uint64_t mask = 0;
    for (std::size_t i = 0; i < kPackedPhraseLetters; ++i) {
        mask |= std::uint64_t{1} << (5 * i + 4);
    }
    return mask;
}();

#if defined(SOLVE_TIMING) && SOLVE_TIMING
constexpr bool kTimingEnabled = true;
#else
constexpr bool kTimingEnabled = false;
#endif

std::uint64_t SaturatingMultiply(std::uint64_t left, std::uint64_t right) {
    if (left != 0 && right > kMaxU64 / left) {
        return kMaxU64;
    }
    return left * right;
}

// C(n, k), saturating instead of overflowing.
// For the string lengths a person can type this is exact.
// The saturation only keeps absurd inputs well-defined.
std::uint64_t Binomial(std::uint64_t n, std::uint64_t k) {
    if (k > n) {
        return 0;
    }
    k = std::min(k, n - k);
    std::uint64_t result = 1;
    for (std::uint64_t i = 1; i <= k; ++i) {
        const std::uint64_t factor = n - k + i;
        if (result > kMaxU64 / factor) {
            return kMaxU64;
        }
        // The intermediate product is always divisible by i: it equals
        // C(n - k + i, i) * i.
        result = result * factor / i;
    }
    return result;
}

std::string LowerAscii(std::string_view text) {
    std::string lower;
    lower.reserve(text.size());
    for (const char ch : text) {
        lower.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    return lower;
}

class CandidateSource {
public:
    virtual ~CandidateSource() = default;

    /// Fills up to out.size() candidates; returns how many were written.
    /// Zero means the source is exhausted. Sub-phase timings go into `generate`.
    virtual std::size_t next_batch(std::span<std::string> out,
                                   solve::Timing::Generate& generate) = 0;

    /// Work performed so far: permutations generated or wordlist lines read.
    [[nodiscard]] virtual std::uint64_t scanned() const { return 0; }
};

class PermutationSource final : public CandidateSource {
public:
    PermutationSource(std::string letters, Timer::Timer& setup_timer)
        : current_(std::move(letters)) {
        [[maybe_unused]] const Timer::ScopedTimer setup(setup_timer, kTimingEnabled);
        std::ranges::sort(current_);
    }

    std::size_t next_batch(std::span<std::string> out,
                           solve::Timing::Generate& generate) override {
        std::size_t count = 0;
        [[maybe_unused]] const Timer::ScopedTimer phase(generate.permute, kTimingEnabled);
        while (count < out.size() && !done_) {
            out[count++] = current_;
            ++generated_;
            if (!std::next_permutation(current_.begin(), current_.end())) {
                done_ = true;
            }
        }
        return count;
    }

    [[nodiscard]] std::uint64_t scanned() const override { return generated_; }

private:
    std::string current_;
    bool done_ = false;
    std::uint64_t generated_ = 0;
};

constexpr std::array<std::uint8_t, 256> MakeDelimiterTable() {
    std::array<std::uint8_t, 256> table{};
    for (const char ch : {' ', '\t', '\r', '\n', ',', ';', '|', '/',
                          '[', ']', '{', '}', ':', '"', '(', ')'}) {
        table[static_cast<unsigned char>(ch)] = 1;
    }
    return table;
}

constexpr auto kDelimiterTable = MakeDelimiterTable();

// Reads a wordlist as a stream of tokens, making a best effort at the shapes wordlists come in:
// one entry per line.
// CSV/TSV files.
// comma/whitespace separated lists.
// JSON arrays/objects (quoted strings and punctuation are stripped by the same tokenizer).
// UTF-8 with or without BOM and LF/CRLF/CR endings all work.
// Compressed and UTF-16 files are rejected with an actionable message, rather than being silently read as mojibake.
//
// The file is read in chunks and tokenized in place.
// A std::getline loop calls through the stream buffer once per line, copies the line, then scans it again.
// For a dictionary-sized list, that loop was several times the cost of the setup work it feeds.
// One pass over a 256 KiB buffer removes both copies and the per-line calls while keeping memory bounded for very large wordlists.
class WordlistReader {
public:
    explicit WordlistReader(const std::filesystem::path& path)
        : file_(path, std::ios::binary) {
        if (!file_) {
            error_ = "cannot open wordlist: " + path.string();
            return;
        }

        std::array<unsigned char, 4> probe{};
        file_.read(reinterpret_cast<char*>(probe.data()),
                   static_cast<std::streamsize>(probe.size()));
        const std::streamsize got = file_.gcount();
        file_.clear();

        if (got >= 2 && probe[0] == 0x1F && probe[1] == 0x8B) {
            error_ = "wordlist is gzip-compressed; decompress it first";
            return;
        }
        if (got >= 2 && ((probe[0] == 0xFF && probe[1] == 0xFE) ||
                         (probe[0] == 0xFE && probe[1] == 0xFF))) {
            error_ = "wordlist is UTF-16; convert it to UTF-8";
            return;
        }

        std::streamoff offset = 0;
        if (got >= 3 && probe[0] == 0xEF && probe[1] == 0xBB && probe[2] == 0xBF) {
            // UTF-8 BOM: start reading after it.
            offset = 3;
        } else {
            for (std::streamsize i = 0; i < got; ++i) {
                if (probe[i] == 0) {
                    error_ = "wordlist looks binary or UTF-16; convert it to UTF-8";
                    return;
                }
            }
        }

        file_.seekg(offset);
        if (!file_) {
            error_ = "cannot read wordlist: " + path.string();
        }
    }

    [[nodiscard]] bool is_open() const { return file_.is_open(); }
    [[nodiscard]] const std::string& error() const { return error_; }

    /// Returns the next non-empty token, or false at the end of the file.
    bool next(std::string& token) {
        token.clear();
        while (true) {
            if (pos_ == end_ && !fill()) {
                // A final token need not be followed by a delimiter.
                return !token.empty();
            }

            if (at_line_start_) {
                const char ch = buffer_[pos_];
                if (ch == ' ' || ch == '\t' || ch == '\r') {
                    ++pos_;
                    continue;
                }
                if (ch == '\n') {
                    ++pos_;
                    continue;
                }
                if (ch == '#') {
                    skip_comment();
                    continue;
                }
                if (ch == '/') {
                    // "//" starts a comment only at the start of a line.
                    // The second byte may live in the next chunk.
                    if (pos_ + 1 == end_) {
                        if (!eof_ && fill()) {
                            continue;
                        }
                        // A lone '/' at end of file is just a delimiter.
                        at_line_start_ = false;
                        ++pos_;
                        continue;
                    }
                    if (buffer_[pos_ + 1] == '/') {
                        skip_comment();
                        continue;
                    }
                }
                at_line_start_ = false;
            }

            const char ch = buffer_[pos_];
            if (IsDelimiter(ch)) {
                ++pos_;
                if (ch == '\n') {
                    at_line_start_ = true;
                }
                continue;
            }

            const std::size_t begin = pos_;
            while (pos_ < end_ && !IsDelimiter(buffer_[pos_])) {
                ++pos_;
            }
            token.append(buffer_.data() + begin, pos_ - begin);
            if (pos_ == end_) {
                // The token may continue in the next chunk.
                // What has been copied is already in `token`, and `fill` compacts the unread tail to the front.
                if (!fill()) {
                    return true;
                }
                // The token ended exactly at the old chunk boundary if the first new byte is a delimiter.
                // Consume it and finish the token instead of merging the next entry into this one.
                if (IsDelimiter(buffer_[pos_])) {
                    const char delimiter = buffer_[pos_];
                    ++pos_;
                    if (delimiter == '\n') {
                        at_line_start_ = true;
                    }
                    return true;
                }
                continue;
            }
            const char delimiter = buffer_[pos_];
            ++pos_;
            if (delimiter == '\n') {
                at_line_start_ = true;
            }
            return true;
        }
    }

private:
    static constexpr std::size_t kReadChunk = 1 << 18;

    static bool IsDelimiter(char ch) {
        return kDelimiterTable[static_cast<unsigned char>(ch)] != 0;
    }

    /// Reads more bytes, moving any unread tail to the front.
    /// Returns false only when no unread byte is available any more.
    bool fill() {
        if (pos_ > 0) {
            const std::size_t remaining = end_ - pos_;
            if (remaining != 0) {
                std::memmove(buffer_.data(), buffer_.data() + pos_, remaining);
            }
            end_ = remaining;
            pos_ = 0;
        }
        if (eof_) {
            return end_ != 0;
        }
        if (buffer_.empty()) {
            // Header-only validation (PlanSearch) never calls this, so a 256 KiB buffer is allocated only when the file is actually read.
            buffer_.resize(kReadChunk);
        }
        if (end_ == buffer_.size()) {
            buffer_.resize(buffer_.size() * 2);
        }
        const std::size_t want = buffer_.size() - end_;
        file_.read(buffer_.data() + end_, static_cast<std::streamsize>(want));
        const std::streamsize got = file_.gcount();
        if (got <= 0) {
            eof_ = true;
            if (file_.bad()) {
                error_ = "cannot read wordlist";
            }
            return end_ != 0;
        }
        end_ += static_cast<std::size_t>(got);
        if (static_cast<std::size_t>(got) < want) {
            eof_ = true;
            if (file_.bad()) {
                error_ = "cannot read wordlist";
            }
        }
        return true;
    }

    void skip_comment() {
        while (true) {
            if (pos_ == end_ && !fill()) {
                at_line_start_ = true;
                return;
            }
            const void* newline = std::memchr(buffer_.data() + pos_, '\n', end_ - pos_);
            if (newline != nullptr) {
                pos_ = static_cast<std::size_t>(
                           static_cast<const char*>(newline) - buffer_.data()) +
                       1;
                at_line_start_ = true;
                return;
            }
            pos_ = end_;
        }
    }

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

class DictionarySource final : public CandidateSource {
public:
    DictionarySource(std::filesystem::path path, std::string_view letters,
                     Timer::Timer& setup_timer)
        : reader_(std::move(path)), target_length_(letters.size()) {
        [[maybe_unused]] const Timer::ScopedTimer setup(setup_timer, kTimingEnabled);
        for (const unsigned char ch : letters) {
            ++counts_[ch];
        }
    }

    [[nodiscard]] bool is_open() const { return reader_.is_open() && reader_.error().empty(); }
    [[nodiscard]] const std::string& error() const { return reader_.error(); }

    std::size_t next_batch(std::span<std::string> out,
                           solve::Timing::Generate& generate) override {
        std::size_t count = 0;
        std::string token;
        const std::chrono::nanoseconds filter_before = generate.filter;
        Timer::Timer batch_timer(kTimingEnabled);
        while (count < out.size() && reader_.next(token)) {
            ++scanned_;
            {
                [[maybe_unused]] const Timer::ScopedTimer phase(generate.filter, kTimingEnabled);
                if (token.size() != target_length_ || !SameMultiset(token)) {
                    continue;
                }
                out[count++] = token;
            }
        }
        batch_timer.Stop();
        // The batch timer covers the whole loop; what is left after the filter
        // scopes is reading/tokenizing the wordlist.
        const std::chrono::nanoseconds filter_elapsed = generate.filter - filter_before;
        const std::chrono::nanoseconds batch_elapsed = batch_timer.ElapsedDuration();
        generate.scan += batch_elapsed > filter_elapsed
                             ? batch_elapsed - filter_elapsed
                             : std::chrono::nanoseconds{0};
        return count;
    }

    [[nodiscard]] std::uint64_t scanned() const override { return scanned_; }

private:
    [[nodiscard]] bool SameMultiset(std::string_view word) const {
        std::array<std::uint16_t, 256> seen{};
        for (const unsigned char ch : word) {
            if (++seen[ch] > counts_[ch]) {
                return false;
            }
        }
        return true;
    }

    WordlistReader reader_;
    std::size_t target_length_ = 0;
    std::array<std::uint16_t, 256> counts_{};
    std::uint64_t scanned_ = 0;
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
                                                  std::string& error) {
    WordlistCount count;
    std::string token;
    while (reader.next(token)) {
        ++count.entries;
        if (count.entries == limit) {
            count.reached_limit = true;
            break;
        }
    }
    if (!reader.error().empty()) {
        error = reader.error();
        return std::nullopt;
    }
    return count;
}

// ---------------------------------------------------------------------------
// phrase solver
//
// A multi-word anagram is not a single token, so the dictionary solver cannot express it.
// The phrase solver composes the answer from wordlist words:
// strip the clue's spaces (they are separators, not anagram characters).
// try a range of word counts.
// find words whose letter counts sum to the target.
// hash the joined phrase.
// ---------------------------------------------------------------------------

struct PhraseWord {
    std::string text;
    std::array<std::uint16_t, kMaxPhraseUnique> counts{};
    std::uint64_t key = 0;
    /// Bits of the letters present in `text`, for one-AND exhaustion checks.
    std::uint64_t mask = 0;
    /// The first kWordCopyBytes characters of `text`, zero-padded, so emitting a
    /// phrase is a constant-size store rather than a memcpy call.
    std::array<char, kWordCopyBytes> head{};
};

/// Two words that together use exactly the letters of one mixed-radix state.
/// The memo of these lists turns the bottom two levels of the DFS into a table
/// lookup: for a remaining key, every completion is already enumerated.
struct PhrasePair {
    std::uint32_t first = 0;
    std::uint32_t second = 0;
};

/// Half-open range into the on-demand pair lists.
struct PairRange {
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
};

/// The part of a word that phrase writing needs, kept in a small array of its
/// own. The memo path picks words at random; walking these 20-byte records keeps
/// those accesses in L1 instead of touching the 120-byte `PhraseWord` records.
struct WordEmit {
    std::array<char, kWordCopyBytes> head{};
    std::uint32_t size = 0;
};

class PhraseSearch {
public:
    PhraseSearch(const solve::SearchProblem& problem, std::string_view phrase,
                 Timer::Timer& setup_timer, solve::Timing& timing, solve::Counters& counters)
        : target_(problem.target.digest), max_candidates_(problem.max_candidates),
          timing_(&timing), counters_(&counters), setup_timer_(&setup_timer) {
        [[maybe_unused]] const Timer::ScopedTimer setup(setup_timer, kTimingEnabled);
        if (!problem.wordlist) {
            error_ = "the phrase solver requires a wordlist (--wordlist)";
            return;
        }

        std::string letters;
        letters.reserve(phrase.size());
        for (const char ch : phrase) {
            if (ch != ' ') {
                letters.push_back(ch);
            }
        }
        if (letters.empty()) {
            error_ = "the anagram has no letters after removing spaces";
            return;
        }
        total_letters_ = letters.size();
        max_words_ = std::min<std::size_t>(total_letters_, kMaxPhraseWords);
        if (max_words_ < 2) {
            error_ = "a multi-word phrase needs at least two letters";
            return;
        }

        index_of_.fill(kNoIndex);
        for (const unsigned char ch : letters) {
            const std::size_t index = FindOrAddChar(ch);
            if (index == kMaxPhraseUnique) {
                error_ = "the anagram uses more than " +
                         std::to_string(kMaxPhraseUnique) + " distinct characters";
                return;
            }
            ++target_counts_[index];
        }

        full_mask_ = (std::uint64_t{1} << unique_count_) - 1;

        std::uint64_t stride = 1;
        for (std::size_t i = 0; i < unique_count_; ++i) {
            strides_[i] = stride;
            const std::uint64_t radix = static_cast<std::uint64_t>(target_counts_[i]) + 1;
            if (stride > kMaxU64 / radix) {
                error_ = "the anagram has too many letter combinations for the phrase solver";
                return;
            }
            stride *= radix;
        }
        state_count_ = stride;
        target_key_ = state_count_ - 1;

        WordlistReader reader(problem.wordlist->path);
        if (!reader.error().empty()) {
            error_ = reader.error();
            return;
        }
        if (!reader.is_open()) {
            error_ = "cannot open wordlist: " + problem.wordlist->path.string();
            return;
        }

        std::string token;
        std::array<std::uint16_t, kMaxPhraseUnique> counts{};
        while (reader.next(token)) {
            ++wordlist_entries_;
            if (token.empty() || token.size() > total_letters_) {
                continue;
            }

            // Count into a local record first.
            // The old loop constructed a whole PhraseWord (and zeroed its 64-byte count array) for every token, including the ~98% that do not fit the anagram.
            counts.fill(0);
            std::uint64_t mask = 0;
            bool fits = true;
            for (const unsigned char ch : token) {
                const std::size_t index = index_of_[ch];
                if (index == kNoIndex) {
                    fits = false;
                    break;
                }
                if (++counts[index] > target_counts_[index]) {
                    fits = false;
                    break;
                }
                mask |= std::uint64_t{1} << index;
            }
            if (!fits) {
                continue;
            }

            PhraseWord word;
            word.text = token;
            word.counts = counts;
            word.mask = mask;
            const std::size_t head_bytes = std::min(token.size(), kWordCopyBytes);
            std::memcpy(word.head.data(), token.data(), head_bytes);
            for (std::size_t i = 0; i < unique_count_; ++i) {
                word.key += static_cast<std::uint64_t>(word.counts[i]) * strides_[i];
            }
            words_.push_back(std::move(word));
        }

        if (words_.empty()) {
            return;
        }

        // Longest first, because it consumes the most letters and prunes the deepest levels of the search earlier.
        // Duplicates are adjacent under this ordering.
        // One `unique` pass replaces a hash set, which would allocate and hash a copy of every accepted token.
        std::ranges::sort(words_, [](const PhraseWord& left, const PhraseWord& right) {
            if (left.text.size() != right.text.size()) {
                return left.text.size() > right.text.size();
            }
            return left.text < right.text;
        });
        words_.erase(std::unique(words_.begin(), words_.end(),
                                 [](const PhraseWord& left, const PhraseWord& right) {
                                     return left.text == right.text;
                                 }),
                     words_.end());

        max_word_length_ = words_.front().text.size();
        min_word_length_ = words_.back().text.size();
        // Emission records in the same order as words_, so the pair memo can
        // name a word by its index and still write it from a compact array.
        emit_words_.resize(words_.size());
        for (std::size_t i = 0; i < words_.size(); ++i) {
            emit_words_[i].head = words_[i].head;
            emit_words_[i].size = static_cast<std::uint32_t>(words_[i].text.size());
        }
        if (state_count_ <= kFlatKeyLimit) {
            flat_keys_ = true;
            key_offsets_.assign(static_cast<std::size_t>(state_count_) + 1, 0);
            for (const PhraseWord& word : words_) {
                ++key_offsets_[static_cast<std::size_t>(word.key) + 1];
            }
            for (std::size_t i = 1; i < key_offsets_.size(); ++i) {
                key_offsets_[i] += key_offsets_[i - 1];
            }
            key_entries_.resize(words_.size());
            std::vector<std::uint32_t> cursor(key_offsets_.begin(), key_offsets_.end() - 1);
            for (std::size_t i = 0; i < words_.size(); ++i) {
                const std::size_t key = static_cast<std::size_t>(words_[i].key);
                key_entries_[cursor[key]++] = static_cast<std::uint32_t>(i);
            }
        } else {
            for (std::size_t i = 0; i < words_.size(); ++i) {
                by_key_[words_[i].key].push_back(static_cast<std::uint32_t>(i));
            }
        }

        // The on-demand memo needs the flat key index and the packed pair test.
        std::uint16_t max_target_count = 0;
        for (std::size_t i = 0; i < unique_count_; ++i) {
            max_target_count = std::max(max_target_count, target_counts_[i]);
        }
        lazy_pair_possible_ = flat_keys_ && words_.size() <= kPairMemoWordLimit &&
                              unique_count_ <= kPackedPhraseLetters &&
                              max_target_count <= 7;

        remaining_counts_ = target_counts_;
        remaining_length_ = total_letters_;
        remaining_mask_ = full_mask_;
    }

    solve::Outcome Run() {
        solve::Outcome outcome;
        if (!error_.empty()) {
            outcome.status = solve::Status::invalid;
            outcome.message = error_;
            return outcome;
        }
        if (words_.empty()) {
            outcome.status = solve::Status::exhausted;
            outcome.message = "no wordlist entry uses only the anagram's letters";
            return outcome;
        }

        phrase_buffer_.resize(total_letters_ + max_words_ - 1 + kSlotSlack);
        batch_stride_ = total_letters_ + max_words_ - 1 + kSlotSlack;
        batch_storage_.resize(kPhraseBatchSize * batch_stride_);
        Timer::Timer search_timer(true);
        // The lazy pair memo advances `setup_timer_` in the middle of a pass, so
        // its growth has to come out of the level timers exactly like the other
        // nested phases. `setup_at_start` also keeps the constructor's own setup
        // time out of the subtraction.
        [[maybe_unused]] const std::chrono::nanoseconds setup_at_start =
            kTimingEnabled ? setup_timer_->ElapsedDuration() : std::chrono::nanoseconds{0};
        // Everything measured inside the traversal: hashing, comparison, span
        // preparation, and any setup work performed on demand.
        [[maybe_unused]] const auto inner = [this, setup_at_start] {
            return timing_->generate.prepare + timing_->hash + timing_->match +
                   (setup_timer_->ElapsedDuration() - setup_at_start);
        };
        DfsResult result = DfsResult::exhausted;
        for (std::size_t words = 2; words <= max_words_; ++words) {
            remaining_counts_ = target_counts_;
            remaining_length_ = total_letters_;
            remaining_mask_ = full_mask_;
            [[maybe_unused]] const std::chrono::nanoseconds nested_before =
                kTimingEnabled ? inner() : std::chrono::nanoseconds{0};
            Timer::Timer level_timer(kTimingEnabled);
            result = dfs(words, target_key_, 0);
            level_timer.Stop();
            if constexpr (kTimingEnabled) {
                // The level timer includes hash/match/prepare and on-demand
                // setup; take those out so the per-depth values add up to
                // `generate.dfs`.
                const std::chrono::nanoseconds nested = inner() - nested_before;
                const std::chrono::nanoseconds level_total = level_timer.ElapsedDuration();
                timing_->generate.dfs_by_word_count[words - 2] +=
                    level_total > nested ? level_total - nested
                                         : std::chrono::nanoseconds{0};
            }
            if (result != DfsResult::exhausted) {
                break;
            }
        }
        if (result != DfsResult::found && batch_count_ > 0) {
            const DfsResult flushed = flush();
            if (flushed == DfsResult::found) {
                result = flushed;
            }
        }
        search_timer.Stop();
        // `Stats::elapsed` is documented as the search loop excluding setup, and
        // the other solvers get that for free because their setup finishes
        // before `search_timer` starts. The pair memo is setup performed inside
        // the loop, so take its measured cost back out.
        const std::chrono::nanoseconds search_elapsed = search_timer.ElapsedDuration();
        const std::chrono::nanoseconds elapsed =
            search_elapsed > lazy_setup_ ? search_elapsed - lazy_setup_
                                         : std::chrono::nanoseconds{0};
        outcome.stats = solve::Stats{hashed_, nodes_, elapsed};
        if constexpr (kTimingEnabled) {
            // Hashing, comparison and span preparation happen inside the DFS
            // (flush), so the traversal is the search duration minus those
            // nested phases. On-demand setup is already out of `elapsed`.
            const std::chrono::nanoseconds nested =
                timing_->hash + timing_->match + timing_->generate.prepare;
            timing_->generate.dfs =
                elapsed > nested ? elapsed - nested : std::chrono::nanoseconds{0};
        }

        switch (result) {
            case DfsResult::found:
                outcome.status = solve::Status::found;
                outcome.solution = solve::Solution{phrase_, target_, "phrase"};
                break;
            case DfsResult::node_budget:
                outcome.status = solve::Status::exhausted;
                outcome.message =
                    "phrase search stopped after " + std::to_string(nodes_) +
                    " prefixes (" + std::to_string(wordlist_entries_) +
                    " wordlist entries considered); the answer may need a larger budget, or "
                    "its words may not be in the wordlist";
                break;
            case DfsResult::candidate_budget:
                outcome.status = solve::Status::exhausted;
                outcome.message = "candidate budget exhausted after " +
                                  std::to_string(hashed_) + " phrase hash(es)";
                break;
            case DfsResult::exhausted:
                outcome.status = solve::Status::exhausted;
                outcome.message = "no matching phrase after " + std::to_string(hashed_) +
                                  " hash(es) from " + std::to_string(wordlist_entries_) +
                                  " wordlist entries (" + std::to_string(nodes_) + " prefixes)";
                break;
        }
        return outcome;
    }

private:
    enum class DfsResult {
        exhausted,
        found,
        node_budget,
        candidate_budget,
    };

    std::size_t FindOrAddChar(unsigned char ch) {
        if (index_of_[ch] != kNoIndex) {
            return index_of_[ch];
        }
        if (unique_count_ == kMaxPhraseUnique) {
            return kMaxPhraseUnique;
        }
        index_of_[ch] = static_cast<std::uint8_t>(unique_count_);
        return unique_count_++;
    }

    [[nodiscard]] bool Fits(const PhraseWord& word) const {
        // The caller's length window already guarantees the word is not too
        // long; this rejects exhausted letters before the count scan.
        if ((word.mask & ~remaining_mask_) != 0) {
            return false;
        }
        for (std::uint64_t bits = word.mask; bits != 0; bits &= bits - 1) {
            const std::size_t i = static_cast<std::size_t>(std::countr_zero(bits));
            if (word.counts[i] > remaining_counts_[i]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool SameCounts(const PhraseWord& word) const {
        for (std::size_t i = 0; i < unique_count_; ++i) {
            if (word.counts[i] != remaining_counts_[i]) {
                return false;
            }
        }
        return true;
    }

    void Subtract(const PhraseWord& word) {
        for (std::uint64_t bits = word.mask; bits != 0; bits &= bits - 1) {
            const std::size_t i = static_cast<std::size_t>(std::countr_zero(bits));
            remaining_counts_[i] -= word.counts[i];
            if (remaining_counts_[i] == 0) {
                remaining_mask_ &= ~(std::uint64_t{1} << i);
            }
        }
        remaining_length_ -= word.text.size();
    }

    void Add(const PhraseWord& word) {
        for (std::uint64_t bits = word.mask; bits != 0; bits &= bits - 1) {
            const std::size_t i = static_cast<std::size_t>(std::countr_zero(bits));
            remaining_counts_[i] += word.counts[i];
        }
        // Every letter in the word is present again after adding it back.
        remaining_mask_ |= word.mask;
        remaining_length_ += word.text.size();
    }

    /// Copies `word` into `dst` with a constant-size move; `dst` must be
    /// followed by at least kWordCopyBytes of writable space.
    static void copy_word(char* dst, const PhraseWord& word) {
        if (word.text.size() <= kWordCopyBytes) {
            std::memcpy(dst, word.head.data(), kWordCopyBytes);
        } else {
            std::memcpy(dst, word.text.data(), word.text.size());
        }
    }

    /// Same idea for the composed prefix, which can be a few words long.
    static void copy_prefix(char* dst, const char* source, std::size_t size) {
        if (size <= kPrefixCopyBytes) {
            std::memcpy(dst, source, kPrefixCopyBytes);
        } else {
            std::memcpy(dst, source, size);
        }
    }

    /// Writes a compact emission record; `full` is only touched for words longer
    /// than the inline head.
    static void copy_emit(char* dst, const WordEmit& word, const PhraseWord& full) {
        if (word.size <= kWordCopyBytes) {
            std::memcpy(dst, word.head.data(), kWordCopyBytes);
        } else {
            std::memcpy(dst, full.text.data(), word.size);
        }
    }

    /// Makes the next batch slot writable for a phrase of `length` bytes. A
    /// batch only ever holds phrases of equal length, so `flush` can hash them
    /// all with one length and emission can use fixed-size moves.
    DfsResult begin_phrase(std::size_t length) {
        if (batch_count_ == kPhraseBatchSize ||
            (batch_count_ != 0 && length != batch_length_)) {
            const DfsResult result = flush();
            if (result != DfsResult::exhausted) {
                return result;
            }
        }
        batch_length_ = length;
        return DfsResult::exhausted;
    }

    [[nodiscard]] char* current_slot() {
        return batch_storage_.data() + batch_count_ * batch_stride_;
    }

    /// Publishes the phrase written through `current_slot()`.
    DfsResult commit_phrase() {
        ++batch_count_;
        if (batch_count_ == kPhraseBatchSize) {
            return flush();
        }
        return DfsResult::exhausted;
    }

    DfsResult add_phrase(const PhraseWord& last, std::size_t depth) {
        if (max_candidates_ != 0 && hashed_ >= max_candidates_) {
            return DfsResult::candidate_budget;
        }
        if constexpr (kTimingEnabled) {
            ++counters_->phrases_emitted;
        }

        std::size_t length = last.text.size();
        for (std::size_t i = 0; i < depth; ++i) {
            length += chosen_[i]->text.size() + 1;
        }
        const DfsResult ready = begin_phrase(length);
        if (ready != DfsResult::exhausted) {
            return ready;
        }

        char* slot = current_slot();
        std::size_t at = 0;
        for (std::size_t i = 0; i < depth; ++i) {
            copy_word(slot + at, *chosen_[i]);
            at += chosen_[i]->text.size();
            slot[at++] = ' ';
        }
        copy_word(slot + at, last);
        return commit_phrase();
    }

    /// Writes `chosen_[0 .. depth-1]`, each followed by a space, into
    /// `phrase_buffer_`; returns the number of characters written.
    std::size_t write_prefix(std::size_t depth) {
        std::size_t length = 0;
        for (std::size_t i = 0; i < depth; ++i) {
            const PhraseWord& word = *chosen_[i];
            copy_word(phrase_buffer_.data() + length, word);
            length += word.text.size();
            phrase_buffer_[length++] = ' ';
        }
        return length;
    }

    /// Packs each word's letter counts into the 5-bit-per-letter layout used by the pair tests.
    void PackWords(std::vector<std::uint64_t>& packed) const {
        packed.resize(words_.size());
        for (std::size_t i = 0; i < words_.size(); ++i) {
            std::uint64_t value = 0;
            for (std::size_t k = 0; k < unique_count_; ++k) {
                value |= static_cast<std::uint64_t>(words_[i].counts[k]) << (5 * k);
            }
            packed[i] = value;
        }
    }

    /// The full memo's list for one state.
    [[nodiscard]] std::span<const PhrasePair> FullPairsFor(std::uint64_t key) const {
        const std::size_t bucket = static_cast<std::size_t>(key);
        const std::size_t begin = pair_offsets_[bucket];
        const std::size_t count = pair_offsets_[bucket + 1] - begin;
        return std::span<const PhrasePair>{pair_entries_}.subspan(begin, count);
    }

    /// Drops the partial on-demand lists.
    void ClearLazyCaches() {
        lazy_pairs_.clear();
        lazy_pairs_.shrink_to_fit();
        lazy_pair_index_.clear();
        lazy_packed_words_.clear();
        lazy_packed_words_.shrink_to_fit();
    }

    /// The list for one state, built on first use while the full memo has not been built yet.
    /// After `kPairMemoLazyKeyLimit` distinct states, this falls back to the full table, which is cheaper once the search is going to visit most of the state space.
    std::optional<std::span<const PhrasePair>> LazyPairsFor(std::uint64_t key) {
        if (pair_memo_ready_) {
            return FullPairsFor(key);
        }
        if (pair_memo_built_ || lazy_pair_rejected_) {
            // The full table was rejected, or an on-demand list would have exceeded the same cap.
            // Drop back to the constant-memory plain two-word expansion instead of growing lists without bound.
            return std::nullopt;
        }
        if (const auto found = lazy_pair_index_.find(key); found != lazy_pair_index_.end()) {
            return std::span<const PhrasePair>{lazy_pairs_}.subspan(found->second.offset,
                                                                    found->second.count);
        }
        if (lazy_pair_index_.size() >= kPairMemoLazyKeyLimit) {
            BuildPairMemo();
            if (pair_memo_ready_) {
                ClearLazyCaches();
                return FullPairsFor(key);
            }
            // The full table is too large, so discard the partial caches and let the rest of the search run with constant memory.
            lazy_pair_rejected_ = true;
            ClearLazyCaches();
            return std::nullopt;
        }

        [[maybe_unused]] const Timer::ScopedTimer phase(*setup_timer_, kTimingEnabled);
        [[maybe_unused]] const Timer::ScopedTimer measured(lazy_setup_, true);
        if (lazy_packed_words_.empty()) {
            PackWords(lazy_packed_words_);
        }

        std::uint64_t state_packed = 0;
        std::uint64_t rest = key;
        for (std::size_t k = 0; k < unique_count_; ++k) {
            const std::uint64_t radix = static_cast<std::uint64_t>(target_counts_[k]) + 1;
            state_packed |= (rest % radix) << (5 * k);
            rest /= radix;
        }
        const std::uint64_t check = state_packed | kPhraseFieldHigh;
        const std::uint32_t offset = static_cast<std::uint32_t>(lazy_pairs_.size());
        for (std::size_t i = 0; i < words_.size(); ++i) {
            if (((check - lazy_packed_words_[i]) & kPhraseFieldHigh) != kPhraseFieldHigh) {
                continue;
            }
            const std::size_t bucket = static_cast<std::size_t>(key - words_[i].key);
            const std::uint32_t begin = key_offsets_[bucket];
            const std::uint32_t end = key_offsets_[bucket + 1];
            const std::size_t bucket_count = static_cast<std::size_t>(end - begin);
            if (lazy_pairs_.size() + bucket_count > kPairMemoEntryLimit) {
                // One state alone would exceed the pair table's cap, so use the plain expansion, which needs no table at all.
                lazy_pair_rejected_ = true;
                ClearLazyCaches();
                return std::nullopt;
            }
            for (std::uint32_t at = begin; at < end; ++at) {
                lazy_pairs_.push_back(PhrasePair{static_cast<std::uint32_t>(i), key_entries_[at]});
            }
        }
        const std::uint32_t count =
            static_cast<std::uint32_t>(lazy_pairs_.size() - offset);
        lazy_pair_index_.emplace(key, PairRange{offset, count});
        return std::span<const PhrasePair>{lazy_pairs_}.subspan(offset, count);
    }

    /// Enumerates the ordered word pairs behind every mixed-radix state, so the
    /// last two levels of the DFS become one table lookup. Only the states that
    /// a prefix can leave behind are ever asked for, and the lists preserve the
    /// (first, second) index order the plain DFS would visit, so the search
    /// finds the same phrase and reports the same candidates.
    void BuildPairMemo() {
        pair_memo_built_ = true;
        if (!flat_keys_ || words_.size() > kPairMemoWordLimit) {
            return;
        }

        // Index work, so it is reported as setup even though it happens lazily
        // on the first node with two words left. `setup_timer_` is read after
        // the search returns, so the accumulating timer picks this up without
        // the caller having to know it happened late.
        [[maybe_unused]] const Timer::ScopedTimer phase(*setup_timer_, kTimingEnabled);
        // Measured in every build, not just the instrumented ones: `Stats` has
        // to exclude setup even where `Timing` itself is compiled out.
        [[maybe_unused]] const Timer::ScopedTimer measured(lazy_setup_, true);
        const std::size_t word_count = words_.size();

        // Five bits per distinct letter: with at most seven of any letter, two
        // packed words add without carrying between fields, and one subtraction
        // then reports per letter whether the pair stays within the target.
        // That turns the O(words^2) validity test into a few instructions.
        std::uint64_t target_packed = 0;
        std::uint32_t max_count = 0;
        for (std::size_t k = 0; k < unique_count_; ++k) {
            target_packed |= static_cast<std::uint64_t>(target_counts_[k]) << (5 * k);
            max_count = std::max<std::uint32_t>(max_count, target_counts_[k]);
        }
        const bool packed_layout = unique_count_ <= kPackedPhraseLetters && max_count <= 7;
        std::vector<std::uint64_t> packed;
        if (packed_layout) {
            PackWords(packed);
        }
        const std::uint64_t check = target_packed | kPhraseFieldHigh;

        // The pair tests below run O(words^2) times, so every byte they touch matters.
        // `PhraseWord` is ~128 bytes, and reading its key for each valid pair would stride through the whole array once per outer iteration.
        // A compact key array makes the inner loop sequential over 8-byte entries instead.
        std::vector<std::uint64_t> keys(word_count);
        for (std::size_t i = 0; i < word_count; ++i) {
            keys[i] = words_[i].key;
        }

        // Count the valid pairs per remaining key and remember which pairs they are.
        // A bit per (first, second) pair is a few hundred KiB for a dictionary-sized list, and it lets the fill pass skip the O(words^2) validity test entirely.
        const std::size_t mask_stride = (word_count + 63) / 64;
        std::vector<std::uint64_t> valid_pairs(word_count * mask_stride, 0);
        pair_offsets_.assign(static_cast<std::size_t>(state_count_) + 1, 0);
        std::uint64_t total = 0;
        const auto count_pair = [&](std::size_t i, std::size_t j, std::uint64_t key) {
            valid_pairs[i * mask_stride + (j >> 6)] |= std::uint64_t{1} << (j & 63);
            ++pair_offsets_[static_cast<std::size_t>(key) + 1];
            return ++total <= kPairMemoEntryLimit;
        };
        const auto visit_pairs = [&](auto&& sink) {
            for (std::size_t i = 0; i < word_count; ++i) {
                const std::uint64_t first_key = keys[i];
                if (packed_layout) {
                    const std::uint64_t first = packed[i];
                    for (std::size_t j = 0; j < word_count; ++j) {
                        const std::uint64_t difference = check - (first + packed[j]);
                        if ((difference & kPhraseFieldHigh) != kPhraseFieldHigh) {
                            continue;
                        }
                        if (!sink(i, j, first_key + keys[j])) {
                            return false;
                        }
                    }
                } else {
                    for (std::size_t j = 0; j < word_count; ++j) {
                        if (!PairFits(words_[i], words_[j])) {
                            continue;
                        }
                        if (!sink(i, j, first_key + keys[j])) {
                            return false;
                        }
                    }
                }
            }
            return true;
        };

        if (!visit_pairs(count_pair)) {
            // Too many completions to be worth the memory; keep the plain DFS instead of a partial table.
            return;
        }

        for (std::size_t i = 1; i < pair_offsets_.size(); ++i) {
            pair_offsets_[i] += pair_offsets_[i - 1];
        }
        pair_entries_.resize(static_cast<std::size_t>(total));
        std::vector<std::uint32_t> cursor(pair_offsets_.begin(), pair_offsets_.end() - 1);
        for (std::size_t i = 0; i < word_count; ++i) {
            const std::uint64_t* row = valid_pairs.data() + i * mask_stride;
            const std::uint64_t first_key = keys[i];
            for (std::size_t word = 0; word < mask_stride; ++word) {
                std::uint64_t bits = row[word];
                while (bits != 0) {
                    const std::size_t j = word * 64 + std::countr_zero(bits);
                    bits &= bits - 1;
                    const std::size_t key =
                        static_cast<std::size_t>(first_key + keys[j]);
                    pair_entries_[cursor[key]++] = PhrasePair{
                        static_cast<std::uint32_t>(i),
                        static_cast<std::uint32_t>(j),
                    };
                }
            }
        }
        pair_memo_ready_ = true;
    }

    /// Fallback pair test for a key space too wide for the packed layout.
    [[nodiscard]] bool PairFits(const PhraseWord& first, const PhraseWord& second) const {
        for (std::size_t k = 0; k < unique_count_; ++k) {
            if (first.counts[k] + second.counts[k] > target_counts_[k]) {
                return false;
            }
        }
        return true;
    }

    /// Bottom two levels of the DFS, resolved from the pair memo: the remaining
    /// letters are known to split into two wordlist words, so there is nothing
    /// left to prune.
    DfsResult finish_with_pairs(std::span<const PhrasePair> pairs, std::size_t depth) {
        if constexpr (kTimingEnabled) {
            ++counters_->leaf_visits;
            counters_->leaf_candidates += pairs.size();
            counters_->same_counts_hits += pairs.size();
        }
        if (pairs.empty()) {
            return DfsResult::exhausted;
        }

        const std::size_t prefix = write_prefix(depth);
        // Every completion uses exactly the letters still remaining, so the
        // phrase length is the same for the whole memo list.
        const DfsResult ready = begin_phrase(prefix + remaining_length_ + 1);
        if (ready != DfsResult::exhausted) {
            return ready;
        }
        const WordEmit* emits = emit_words_.data();
        for (const PhrasePair& pair : pairs) {
            if (max_candidates_ != 0 && hashed_ >= max_candidates_) {
                return DfsResult::candidate_budget;
            }
            if constexpr (kTimingEnabled) {
                ++counters_->phrases_emitted;
            }
            const WordEmit& first = emits[pair.first];
            const WordEmit& last = emits[pair.second];
            char* slot = current_slot();
            copy_prefix(slot, phrase_buffer_.data(), prefix);
            copy_emit(slot + prefix, first, words_[pair.first]);
            std::size_t at = prefix + first.size;
            slot[at++] = ' ';
            copy_emit(slot + at, last, words_[pair.second]);
            const DfsResult result = commit_phrase();
            if (result != DfsResult::exhausted) {
                return result;
            }
        }
        return DfsResult::exhausted;
    }

    DfsResult flush() {
        if (batch_count_ == 0) {
            return DfsResult::exhausted;
        }

        // Uninitialised on purpose: only the first batch_count_ entries are
        // written and read.
        std::array<std::span<const std::byte>, kPhraseBatchSize> inputs;
        std::array<solve::Digest, kPhraseBatchSize> digests;
        // One running timer and two peeks instead of three scoped timers: the
        // phases are measured with four clock reads per batch, not six.
        Timer::Timer flush_timer(kTimingEnabled);
        for (std::size_t i = 0; i < batch_count_; ++i) {
            inputs[i] = std::as_bytes(
                std::span{batch_storage_.data() + i * batch_stride_, batch_length_});
        }
        [[maybe_unused]] const std::chrono::nanoseconds prepared =
            flush_timer.ElapsedDuration();
        hashlib::md5_batch(
            std::span<const std::span<const std::byte>>{inputs.data(), batch_count_},
            std::span<solve::Digest>{digests.data(), batch_count_});
        [[maybe_unused]] const std::chrono::nanoseconds hashed =
            flush_timer.ElapsedDuration();

        std::optional<std::size_t> matched;
        for (std::size_t i = 0; i < batch_count_; ++i) {
            if (digests[i] == target_) {
                matched = i;
                break;
            }
        }
        flush_timer.Stop();

        if constexpr (kTimingEnabled) {
            const std::chrono::nanoseconds elapsed = flush_timer.ElapsedDuration();
            timing_->generate.prepare += prepared;
            timing_->hash += hashed - prepared;
            timing_->match += elapsed - hashed;
        }

        hashed_ += batch_count_;
        if (matched) {
            phrase_.assign(batch_storage_.data() + *matched * batch_stride_, batch_length_);
            batch_count_ = 0;
            return DfsResult::found;
        }
        batch_count_ = 0;
        return DfsResult::exhausted;
    }

    DfsResult dfs(std::size_t words_left, std::uint64_t remaining_key, std::size_t depth) {
        if (++nodes_ > kPhraseNodeBudget) {
            return DfsResult::node_budget;
        }

        if (words_left == 1) {
            if constexpr (kTimingEnabled) {
                ++counters_->leaf_visits;
            }
            std::span<const std::uint32_t> bucket;
            if (flat_keys_) {
                const std::size_t key = static_cast<std::size_t>(remaining_key);
                const std::size_t begin = key_offsets_[key];
                const std::size_t end = key_offsets_[key + 1];
                bucket = std::span<const std::uint32_t>{key_entries_.data() + begin, end - begin};
            } else {
                const auto found = by_key_.find(remaining_key);
                if (found == by_key_.end()) {
                    return DfsResult::exhausted;
                }
                bucket = std::span<const std::uint32_t>{found->second.data(),
                                                        found->second.size()};
            }
            for (const std::uint32_t index : bucket) {
                if constexpr (kTimingEnabled) {
                    ++counters_->leaf_candidates;
                }
                const PhraseWord& word = words_[index];
                if (!SameCounts(word)) {
                    continue;
                }
                if constexpr (kTimingEnabled) {
                    ++counters_->same_counts_hits;
                }
                const DfsResult result = add_phrase(word, depth);
                if (result != DfsResult::exhausted) {
                    return result;
                }
            }
            return DfsResult::exhausted;
        }

        // With two words left, the remaining letters either split into two wordlist words or they do not.
        // The memo answers the whole subtree without touching the letter counts.
        // An early answer usually reaches only a few states, so those are served on demand.
        // The full table is built once the search asks for enough of them to pay for it.
        if (words_left == 2) {
            if (pair_memo_ready_) {
                return finish_with_pairs(FullPairsFor(remaining_key), depth);
            }
            if (lazy_pair_possible_) {
                if (const auto pairs = LazyPairsFor(remaining_key)) {
                    return finish_with_pairs(*pairs, depth);
                }
                // The full table was rejected, so use the plain expansion below.
            } else {
                if (!pair_memo_built_) {
                    BuildPairMemo();
                }
                if (pair_memo_ready_) {
                    return finish_with_pairs(FullPairsFor(remaining_key), depth);
                }
            }
        }

        // Skip words whose length cannot be part of any remaining phrase.
        // `words_` is sorted longest-first, so the feasible lengths form one
        // contiguous iterator range.
        const std::size_t remaining_words = words_left - 1;
        const std::size_t min_remaining = remaining_words * min_word_length_;
        const std::size_t max_remaining = remaining_words * max_word_length_;
        if (remaining_length_ < min_remaining) {
            return DfsResult::exhausted;
        }
        const std::size_t max_len = remaining_length_ - min_remaining;
        const std::size_t min_len =
            remaining_length_ > max_remaining ? remaining_length_ - max_remaining : 1;
        if (max_len < min_len) {
            return DfsResult::exhausted;
        }

        const auto length_above = [](const PhraseWord& word, std::size_t length) {
            return word.text.size() > length;
        };
        const auto begin =
            std::lower_bound(words_.begin(), words_.end(), max_len, length_above);
        const auto end =
            std::lower_bound(words_.begin(), words_.end(), min_len - 1, length_above);

        for (auto it = begin; it != end; ++it) {
            const PhraseWord& word = *it;
            if constexpr (kTimingEnabled) {
                ++counters_->fits_calls;
            }
            if (!Fits(word)) {
                continue;
            }
            if constexpr (kTimingEnabled) {
                ++counters_->fits_passes;
            }

            Subtract(word);
            chosen_[depth] = &word;
            if constexpr (kTimingEnabled) {
                ++counters_->edges;
            }
            const DfsResult result = dfs(words_left - 1, remaining_key - word.key, depth + 1);
            Add(word);
            if (result != DfsResult::exhausted) {
                return result;
            }
        }
        return DfsResult::exhausted;
    }

    solve::Digest target_{};
    std::uint64_t max_candidates_ = 0;
    solve::Timing* timing_ = nullptr;
    solve::Counters* counters_ = nullptr;
    /// The caller's setup timer. Kept so lazily built index work (the pair memo)
    /// is charged to `setup` where it belongs, even though it runs mid-search.
    Timer::Timer* setup_timer_ = nullptr;
    /// Cost of that lazily built work, so `Stats::elapsed` can leave setup out
    /// of the reported search duration.
    std::chrono::nanoseconds lazy_setup_{0};
    std::string error_;
    std::size_t max_words_ = 0;
    std::size_t total_letters_ = 0;
    std::size_t unique_count_ = 0;
    std::array<std::uint8_t, 256> index_of_{};
    std::array<std::uint16_t, kMaxPhraseUnique> target_counts_{};
    std::array<std::uint16_t, kMaxPhraseUnique> remaining_counts_{};
    std::uint64_t full_mask_ = 0;
    std::uint64_t remaining_mask_ = 0;
    std::array<std::uint64_t, kMaxPhraseUnique> strides_{};
    std::uint64_t state_count_ = 1;
    std::uint64_t target_key_ = 0;
    std::size_t remaining_length_ = 0;
    std::size_t max_word_length_ = 0;
    std::size_t min_word_length_ = 0;
    std::vector<PhraseWord> words_;
    std::vector<WordEmit> emit_words_;
    bool flat_keys_ = false;
    std::vector<std::uint32_t> key_offsets_;
    std::vector<std::uint32_t> key_entries_;
    bool pair_memo_built_ = false;
    bool pair_memo_ready_ = false;
    std::vector<std::uint32_t> pair_offsets_;
    std::vector<PhrasePair> pair_entries_;
    /// True when a pair list can be built for one state from the flat key index, which is what lets an early answer avoid the full table.
    bool lazy_pair_possible_ = false;
    /// True once on-demand lists have been abandoned for this search.
    bool lazy_pair_rejected_ = false;
    std::unordered_map<std::uint64_t, PairRange> lazy_pair_index_;
    std::vector<PhrasePair> lazy_pairs_;
    std::vector<std::uint64_t> lazy_packed_words_;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> by_key_;
    std::vector<char> batch_storage_;
    std::size_t batch_stride_ = 0;
    std::size_t batch_length_ = 0;
    std::size_t batch_count_ = 0;
    std::array<const PhraseWord*, kMaxPhraseWords> chosen_{};
    std::vector<char> phrase_buffer_;
    std::string phrase_;
    std::uint64_t hashed_ = 0;
    std::uint64_t nodes_ = 0;
    std::uint64_t wordlist_entries_ = 0;
};

solve::Outcome RunEngine(const solve::SearchProblem& problem, CandidateSource& source,
                         std::string_view solver_id, solve::Timing& timing) {
    std::array<std::string, kBatchSize> candidates;
    std::array<std::span<const std::byte>, kBatchSize> inputs{};
    std::array<solve::Digest, kBatchSize> digests{};

    Timer::Timer hash_timer;
    Timer::Timer match_timer;
    Timer::Timer search_timer(true);
    std::uint64_t hashed = 0;
    const auto make_stats = [&]() {
        timing.hash = hash_timer.ElapsedDuration();
        timing.match = match_timer.ElapsedDuration();
        solve::Stats stats{
            hashed,
            source.scanned(),
            search_timer.ElapsedDuration(),
            timing,
        };
        return stats;
    };

    while (true) {
        if (problem.max_candidates != 0 && hashed >= problem.max_candidates) {
            solve::Outcome outcome;
            outcome.status = solve::Status::exhausted;
            outcome.stats = make_stats();
            outcome.message =
                "candidate budget exhausted after " + std::to_string(hashed) + " hash(es)";
            return outcome;
        }

        std::size_t count = source.next_batch(candidates, timing.generate);
        if (count == 0) {
            break;
        }
        {
            [[maybe_unused]] const Timer::ScopedTimer phase(timing.generate.prepare,
                                                            kTimingEnabled);
            for (std::size_t i = 0; i < count; ++i) {
                inputs[i] = std::as_bytes(std::span{candidates[i].data(), candidates[i].size()});
            }
        }
        {
            [[maybe_unused]] const Timer::ScopedTimer phase(hash_timer, kTimingEnabled);
            hashlib::md5_batch(std::span<const std::span<const std::byte>>{inputs.data(), count},
                               std::span<solve::Digest>{digests.data(), count});
        }
        hashed += count;

        std::optional<std::size_t> found;
        {
            [[maybe_unused]] const Timer::ScopedTimer phase(match_timer, kTimingEnabled);
            for (std::size_t i = 0; i < count; ++i) {
                if (digests[i] != problem.target.digest) {
                    continue;
                }
                // The source is supposed to enforce the anagram constraint already.
                // Checking it again on a hit keeps a buggy source from returning a hash collision that is not an anagram.
                if (problem.anagram &&
                    !solve::IsAnagram(candidates[i], problem.anagram->letters)) {
                    continue;
                }
                found = i;
                break;
            }
        }
        if (found) {
            solve::Outcome outcome;
            outcome.status = solve::Status::found;
            outcome.solution =
                solve::Solution{candidates[*found], digests[*found], std::string(solver_id)};
            outcome.stats = make_stats();
            return outcome;
        }
    }

    solve::Outcome outcome;
    outcome.status = solve::Status::exhausted;
    outcome.stats = make_stats();
    outcome.message = "exhausted the search space after " + std::to_string(hashed) +
                      " hash(es) without a match";
    return outcome;
}

// Shared body of both Solve overloads. `total_timer` starts here, so a caller
// that planned itself can add its planning time to the result if it wants the
// whole pipeline.
solve::Outcome SolveWithPlan(const solve::SearchProblem& problem, const solve::Plan& plan) {
    solve::Timing timing;
    solve::Counters counters;
    Timer::Timer setup_timer;
    Timer::Timer total_timer(kTimingEnabled);

    solve::Outcome outcome;
    if (plan.solver == "permutations") {
        PermutationSource source(problem.anagram->letters, setup_timer);
        outcome = RunEngine(problem, source, "permutations", timing);
    } else if (plan.solver == "phrase") {
        PhraseSearch search(problem, problem.anagram->letters, setup_timer, timing, counters);
        outcome = search.Run();
    } else if (plan.solver == "dictionary") {
        if (!problem.wordlist) {
            outcome.status = solve::Status::invalid;
            outcome.message = "solver 'dictionary' requires a wordlist (--wordlist)";
        } else {
            DictionarySource source(problem.wordlist->path, problem.anagram->letters,
                                    setup_timer);
            if (!source.is_open()) {
                outcome.status = solve::Status::invalid;
                outcome.message = source.error().empty()
                                      ? "cannot open wordlist: " +
                                            problem.wordlist->path.string()
                                      : source.error();
            } else {
                outcome = RunEngine(problem, source, "dictionary", timing);
            }
        }
    } else {
        // A Plan normally comes from PlanSearch; tolerate a hand-built one.
        outcome.status = solve::Status::invalid;
        outcome.message = "unknown solver '" + std::string(plan.solver) + "'";
    }

    // RunEngine / PhraseSearch filled generate/hash/match; add the phases that
    // wrap them here.
    timing.setup = setup_timer.ElapsedDuration();
    timing.total = total_timer.ElapsedDuration();
    outcome.stats.timing = timing;
    outcome.stats.counters = counters;
    return outcome;
}

// The table the selector, the CLI help and --list-solvers derive from.
// Adding a strategy means adding a row here, a CandidateSource above, and a branch in Solve() below.
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
};

}  // namespace

namespace solve {

std::optional<Digest> ParseDigest(std::string_view hex) {
    if (hex.size() != 32) {
        return std::nullopt;
    }

    const auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        return -1;
    };

    Digest digest{};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const int high = nibble(hex[2 * i]);
        const int low = nibble(hex[2 * i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        digest[i] = static_cast<std::byte>((high << 4) | low);
    }
    return digest;
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

std::uint64_t CountDistinctPermutations(std::string_view letters) {
    std::array<std::uint64_t, 256> counts{};
    for (const unsigned char ch : letters) {
        ++counts[ch];
    }

    std::uint64_t remaining = letters.size();
    std::uint64_t result = 1;
    for (const std::uint64_t count : counts) {
        if (count == 0) {
            continue;
        }
        result = SaturatingMultiply(result, Binomial(remaining, count));
        remaining -= count;
    }
    return result;
}

bool IsAnagram(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }

    std::array<std::uint32_t, 256> counts{};
    for (const unsigned char ch : left) {
        ++counts[ch];
    }
    for (const unsigned char ch : right) {
        if (counts[ch] == 0) {
            return false;
        }
        --counts[ch];
    }
    return true;
}

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
    if (!automatic && !wants_permutations && !wants_dictionary && !wants_phrase) {
        error = "unknown solver '" + std::string(requested) +
                "'; available: permutations, dictionary, phrase, auto";
        return std::nullopt;
    }

    if (wants_dictionary && !problem.wordlist) {
        error = "solver 'dictionary' requires a wordlist (--wordlist)";
        return std::nullopt;
    }
    if (wants_phrase && !problem.wordlist) {
        error = "solver 'phrase' requires a wordlist (--wordlist)";
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
    if (!multi_word && wants_phrase) {
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
        WordlistReader reader(problem.wordlist->path);
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
                const auto counted = CountWordlistEntries(reader, permutations, error);
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

    if (multi_word && (automatic || wants_phrase)) {
        plan.solver = "phrase";
        // The number of phrases is not known without running the search.
        // The phrase solver counts entries while it builds its index.
        plan.estimated_candidates = 0;
        plan.estimated_scanned = 0;
        plan.reason = (automatic ? "auto: " : "requested; ") +
                      std::string("multi-word phrase composed from the wordlist");
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

Outcome Solve(const SearchProblem& problem, std::string_view requested) {
    std::string error;
    Timer::Timer planning_timer(kTimingEnabled);
    const std::optional<Plan> plan = PlanSearch(problem, requested, error);
    planning_timer.Stop();
    if (!plan) {
        Outcome outcome;
        outcome.status = Status::invalid;
        outcome.message = std::move(error);
        return outcome;
    }

    Outcome outcome = SolveWithPlan(problem, *plan);
    // SolveWithPlan measured setup and the search; fold in the planning that
    // happened just above so `total` covers the whole call.
    const std::chrono::nanoseconds planning = planning_timer.ElapsedDuration();
    outcome.stats.timing.planning = planning;
    outcome.stats.timing.total += planning;
    return outcome;
}

Outcome Solve(const SearchProblem& problem, const Plan& plan) {
    if (!problem.anagram) {
        Outcome outcome;
        outcome.status = Status::invalid;
        outcome.message = "solving requires an anagram (--anagram-of)";
        return outcome;
    }
    return SolveWithPlan(problem, plan);
}

}  // namespace solve
