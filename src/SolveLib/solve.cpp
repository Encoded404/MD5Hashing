module solve;

import std;
import hashlib;

namespace {

constexpr std::size_t kBatchSize = 64;
constexpr std::size_t kMaxPhraseUnique = 32;
constexpr std::size_t kMaxPhraseWords = 8;
constexpr std::uint8_t kNoIndex = 0xFF;
constexpr std::uint64_t kPhraseNodeBudget = 100'000'000;
constexpr std::uint64_t kMaxU64 = std::numeric_limits<std::uint64_t>::max();

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
    /// Zero means the source is exhausted.
    virtual std::size_t next_batch(std::span<std::string> out) = 0;

    /// Work performed so far: permutations generated or wordlist lines read.
    [[nodiscard]] virtual std::uint64_t scanned() const { return 0; }
};

class PermutationSource final : public CandidateSource {
public:
    explicit PermutationSource(std::string letters) : current_(std::move(letters)) {
        std::ranges::sort(current_);
    }

    std::size_t next_batch(std::span<std::string> out) override {
        std::size_t count = 0;
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

// Reads a wordlist as a stream of tokens, making a best effort at the shapes wordlists come in:
// one entry per line, CSV/TSV, comma/whitespace separated, and JSON arrays/objects (quoted strings and punctuation are stripped by the same tokenizer).
// UTF-8 with or without BOM and LF/CRLF/CR endings all work.
// Compressed and UTF-16 files are rejected with an actionable message, rather than being silently read as mojibake.
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
        file_.seekg(0);

        if (got >= 2 && probe[0] == 0x1F && probe[1] == 0x8B) {
            error_ = "wordlist is gzip-compressed; decompress it first";
            return;
        }
        if (got >= 2 && ((probe[0] == 0xFF && probe[1] == 0xFE) ||
                         (probe[0] == 0xFE && probe[1] == 0xFF))) {
            error_ = "wordlist is UTF-16; convert it to UTF-8";
            return;
        }
        if (got >= 3 && probe[0] == 0xEF && probe[1] == 0xBB && probe[2] == 0xBF) {
            // UTF-8 BOM: start reading after it.
            file_.clear();
            file_.seekg(3);
            return;
        }
        for (std::streamsize i = 0; i < got; ++i) {
            if (probe[i] == 0) {
                error_ = "wordlist looks binary or UTF-16; convert it to UTF-8";
                return;
            }
        }
    }

    [[nodiscard]] bool is_open() const { return file_.is_open(); }
    [[nodiscard]] const std::string& error() const { return error_; }

    /// Returns the next non-empty token, or false at the end of the file.
    bool next(std::string& token) {
        while (true) {
            while (pos_ < line_.size() && IsDelimiter(line_[pos_])) {
                ++pos_;
            }
            if (pos_ < line_.size()) {
                const std::size_t begin = pos_;
                while (pos_ < line_.size() && !IsDelimiter(line_[pos_])) {
                    ++pos_;
                }
                token.assign(line_, begin, pos_ - begin);
                return true;
            }

            if (!std::getline(file_, line_)) {
                return false;
            }
            pos_ = 0;
            if (!line_.empty() && line_.back() == '\r') {
                line_.pop_back();
            }

            const std::size_t first = line_.find_first_not_of(" \t\r");
            if (first == std::string::npos) {
                continue;
            }
            if (line_[first] == '#' ||
                (line_[first] == '/' && first + 1 < line_.size() && line_[first + 1] == '/')) {
                line_.clear();
                continue;
            }
        }
    }

private:
    static bool IsDelimiter(char ch) {
        switch (ch) {
            case ' ':
            case '\t':
            case '\r':
            case '\n':
            case ',':
            case ';':
            case '|':
            case '/':
            case '[':
            case ']':
            case '{':
            case '}':
            case ':':
            case '"':
            case '(':
            case ')':
                return true;
            default:
                return false;
        }
    }

    std::ifstream file_;
    std::string line_;
    std::size_t pos_ = 0;
    std::string error_;
};

class DictionarySource final : public CandidateSource {
public:
    DictionarySource(std::filesystem::path path, std::string_view letters)
        : reader_(std::move(path)), target_length_(letters.size()) {
        for (const unsigned char ch : letters) {
            ++counts_[ch];
        }
    }

    [[nodiscard]] bool is_open() const { return reader_.is_open() && reader_.error().empty(); }
    [[nodiscard]] const std::string& error() const { return reader_.error(); }

    std::size_t next_batch(std::span<std::string> out) override {
        std::size_t count = 0;
        std::string token;
        while (count < out.size() && reader_.next(token)) {
            ++scanned_;
            if (token.size() != target_length_ || !SameMultiset(token)) {
                continue;
            }
            out[count++] = token;
        }
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

std::optional<std::uint64_t> CountWordlistEntries(const std::filesystem::path& path,
                                                  std::string& error) {
    WordlistReader reader(path);
    if (!reader.error().empty()) {
        error = reader.error();
        return std::nullopt;
    }
    if (!reader.is_open()) {
        error = "cannot open wordlist: " + path.string();
        return std::nullopt;
    }

    std::uint64_t entries = 0;
    std::string token;
    while (reader.next(token)) {
        ++entries;
    }
    return entries;
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
};

class PhraseSearch {
public:
    PhraseSearch(const solve::SearchProblem& problem, std::string_view phrase)
        : target_(problem.target.digest), max_candidates_(problem.max_candidates) {
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

        std::unordered_set<std::string> seen;
        std::string token;
        while (reader.next(token)) {
            ++wordlist_entries_;
            if (token.empty() || token.size() > total_letters_) {
                continue;
            }

            PhraseWord word;
            bool fits = true;
            for (const unsigned char ch : token) {
                const std::size_t index = index_of_[ch];
                if (index == kNoIndex) {
                    fits = false;
                    break;
                }
                ++word.counts[index];
                if (word.counts[index] > target_counts_[index]) {
                    fits = false;
                    break;
                }
            }
            if (!fits || !seen.insert(token).second) {
                continue;
            }

            word.text = token;
            for (std::size_t i = 0; i < unique_count_; ++i) {
                word.key += static_cast<std::uint64_t>(word.counts[i]) * strides_[i];
            }
            words_.push_back(std::move(word));
        }

        if (words_.empty()) {
            return;
        }

        // Longest first: it consumes the most letters and prunes the deepest levels of the search earlier.
        std::ranges::sort(words_, [](const PhraseWord& left, const PhraseWord& right) {
            if (left.text.size() != right.text.size()) {
                return left.text.size() > right.text.size();
            }
            return left.text < right.text;
        });

        max_word_length_ = words_.front().text.size();
        min_word_length_ = words_.back().text.size();
        for (std::size_t i = 0; i < words_.size(); ++i) {
            by_key_[words_[i].key].push_back(static_cast<std::uint32_t>(i));
        }

        remaining_counts_ = target_counts_;
        remaining_length_ = total_letters_;
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

        prefix_.reserve(total_letters_ + max_words_ - 1);
        const auto start = std::chrono::steady_clock::now();
        DfsResult result = DfsResult::exhausted;
        for (std::size_t words = 2; words <= max_words_; ++words) {
            remaining_counts_ = target_counts_;
            remaining_length_ = total_letters_;
            prefix_.clear();
            result = dfs(words, target_key_);
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
        outcome.stats = solve::Stats{
            hashed_,
            nodes_,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start),
        };

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
        for (std::size_t i = 0; i < unique_count_; ++i) {
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
        for (std::size_t i = 0; i < unique_count_; ++i) {
            remaining_counts_[i] -= word.counts[i];
        }
        remaining_length_ -= word.text.size();
    }

    void Add(const PhraseWord& word) {
        for (std::size_t i = 0; i < unique_count_; ++i) {
            remaining_counts_[i] += word.counts[i];
        }
        remaining_length_ += word.text.size();
    }

    DfsResult add_phrase() {
        if (max_candidates_ != 0 && hashed_ >= max_candidates_) {
            return DfsResult::candidate_budget;
        }
        batch_[batch_count_++] = prefix_;
        if (batch_count_ == batch_.size()) {
            return flush();
        }
        return DfsResult::exhausted;
    }

    DfsResult flush() {
        if (batch_count_ == 0) {
            return DfsResult::exhausted;
        }

        std::array<std::span<const std::byte>, kBatchSize> inputs{};
        std::array<solve::Digest, kBatchSize> digests{};
        for (std::size_t i = 0; i < batch_count_; ++i) {
            inputs[i] = std::as_bytes(std::span{batch_[i].data(), batch_[i].size()});
        }
        hashlib::md5_batch(std::span<const std::span<const std::byte>>{inputs.data(), batch_count_},
                           std::span<solve::Digest>{digests.data(), batch_count_});

        for (std::size_t i = 0; i < batch_count_; ++i) {
            if (digests[i] == target_) {
                phrase_ = batch_[i];
                hashed_ += batch_count_;
                batch_count_ = 0;
                return DfsResult::found;
            }
        }
        hashed_ += batch_count_;
        batch_count_ = 0;
        return DfsResult::exhausted;
    }

    DfsResult dfs(std::size_t words_left, std::uint64_t remaining_key) {
        if (++nodes_ > kPhraseNodeBudget) {
            return DfsResult::node_budget;
        }

        if (words_left == 1) {
            const auto bucket = by_key_.find(remaining_key);
            if (bucket == by_key_.end()) {
                return DfsResult::exhausted;
            }
            for (const std::uint32_t index : bucket->second) {
                const PhraseWord& word = words_[index];
                if (!SameCounts(word)) {
                    continue;
                }
                const std::size_t old_size = prefix_.size();
                prefix_ += word.text;
                const DfsResult result = add_phrase();
                prefix_.resize(old_size);
                if (result != DfsResult::exhausted) {
                    return result;
                }
            }
            return DfsResult::exhausted;
        }

        for (const PhraseWord& word : words_) {
            if (!Fits(word)) {
                continue;
            }
            const std::size_t remaining_after = remaining_length_ - word.text.size();
            if (remaining_after < (words_left - 1) * min_word_length_) {
                continue;
            }
            if (remaining_after > (words_left - 1) * max_word_length_) {
                continue;
            }

            Subtract(word);
            const std::size_t old_size = prefix_.size();
            prefix_ += word.text;
            prefix_ += ' ';
            const DfsResult result = dfs(words_left - 1, remaining_key - word.key);
            prefix_.resize(old_size);
            Add(word);
            if (result != DfsResult::exhausted) {
                return result;
            }
        }
        return DfsResult::exhausted;
    }

    solve::Digest target_{};
    std::uint64_t max_candidates_ = 0;
    std::string error_;
    std::size_t max_words_ = 0;
    std::size_t total_letters_ = 0;
    std::size_t unique_count_ = 0;
    std::array<std::uint8_t, 256> index_of_{};
    std::array<std::uint16_t, kMaxPhraseUnique> target_counts_{};
    std::array<std::uint16_t, kMaxPhraseUnique> remaining_counts_{};
    std::array<std::uint64_t, kMaxPhraseUnique> strides_{};
    std::uint64_t state_count_ = 1;
    std::uint64_t target_key_ = 0;
    std::size_t remaining_length_ = 0;
    std::size_t max_word_length_ = 0;
    std::size_t min_word_length_ = 0;
    std::vector<PhraseWord> words_;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> by_key_;
    std::array<std::string, kBatchSize> batch_{};
    std::size_t batch_count_ = 0;
    std::string prefix_;
    std::string phrase_;
    std::uint64_t hashed_ = 0;
    std::uint64_t nodes_ = 0;
    std::uint64_t wordlist_entries_ = 0;
};

solve::Outcome RunEngine(const solve::SearchProblem& problem, CandidateSource& source,
                         std::string_view solver_id) {
    std::array<std::string, kBatchSize> candidates;
    std::array<std::span<const std::byte>, kBatchSize> inputs{};
    std::array<solve::Digest, kBatchSize> digests{};

    std::uint64_t hashed = 0;
    const auto start = std::chrono::steady_clock::now();
    const auto make_stats = [&]() {
        return solve::Stats{
            hashed,
            source.scanned(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start),
        };
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

        const std::size_t count = source.next_batch(candidates);
        if (count == 0) {
            break;
        }

        for (std::size_t i = 0; i < count; ++i) {
            inputs[i] = std::as_bytes(std::span{candidates[i].data(), candidates[i].size()});
        }
        hashlib::md5_batch(std::span<const std::span<const std::byte>>{inputs.data(), count},
                           std::span<solve::Digest>{digests.data(), count});
        hashed += count;

        for (std::size_t i = 0; i < count; ++i) {
            if (digests[i] != problem.target.digest) {
                continue;
            }
            // The source is supposed to enforce the anagram constraint already.
            // Checking it again on a hit keeps a buggy source from returning a hash collision that is not an anagram.
            if (problem.anagram && !solve::IsAnagram(candidates[i], problem.anagram->letters)) {
                continue;
            }
            solve::Outcome outcome;
            outcome.status = solve::Status::found;
            outcome.solution =
                solve::Solution{candidates[i], digests[i], std::string(solver_id)};
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

    std::uint64_t entries = 0;
    // An explicit permutations request does not consult the wordlist, so do not
    // pay for counting it.
    if (problem.wordlist && !wants_permutations) {
        const auto counted = CountWordlistEntries(problem.wordlist->path, error);
        if (!counted) {
            return std::nullopt;
        }
        entries = *counted;
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
        plan.estimated_candidates = entries;
        plan.estimated_scanned = entries;
        plan.reason = "requested; " + std::to_string(entries) + " wordlist entries";
        return plan;
    }

    if (multi_word && (automatic || wants_phrase)) {
        plan.solver = "phrase";
        // The number of phrases is not known without running the search, so the
        // wordlist count is the only useful pre-search estimate.
        plan.estimated_candidates = 0;
        plan.estimated_scanned = entries;
        plan.reason = (automatic ? "auto: " : "requested; ") +
                      std::string("multi-word phrase from ") + std::to_string(entries) +
                      " wordlist entries";
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
        plan.reason = "auto: " + std::to_string(permutations) +
                      " permutation(s) are cheaper than " + std::to_string(entries) +
                      " wordlist entries";
        return plan;
    }

    plan.solver = "dictionary";
    plan.estimated_candidates = entries;
    plan.estimated_scanned = entries;
    plan.reason = "auto: " + std::to_string(entries) +
                  " wordlist entries are cheaper than " + std::to_string(permutations) +
                  " permutation(s)";
    return plan;
}

Outcome Solve(const SearchProblem& problem, std::string_view requested) {
    std::string error;
    const std::optional<Plan> plan = PlanSearch(problem, requested, error);
    if (!plan) {
        Outcome outcome;
        outcome.status = Status::invalid;
        outcome.message = std::move(error);
        return outcome;
    }

    if (!problem.anagram) {
        Outcome outcome;
        outcome.status = Status::invalid;
        outcome.message = "solving requires an anagram (--anagram-of)";
        return outcome;
    }

    if (plan->solver == "permutations") {
        PermutationSource source(problem.anagram->letters);
        return RunEngine(problem, source, "permutations");
    }

    if (plan->solver == "phrase") {
        PhraseSearch search(problem, problem.anagram->letters);
        return search.Run();
    }

    if (!problem.wordlist) {
        Outcome outcome;
        outcome.status = Status::invalid;
        outcome.message = "solver 'dictionary' requires a wordlist (--wordlist)";
        return outcome;
    }

    DictionarySource source(problem.wordlist->path, problem.anagram->letters);
    if (!source.is_open()) {
        Outcome outcome;
        outcome.status = Status::invalid;
        outcome.message = source.error().empty()
                              ? "cannot open wordlist: " + problem.wordlist->path.string()
                              : source.error();
        return outcome;
    }
    return RunEngine(problem, source, "dictionary");
}

}  // namespace solve
