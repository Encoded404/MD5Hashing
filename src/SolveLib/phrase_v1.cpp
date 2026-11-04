module solve.detail;

import std;
import solve.types;
import Timer;
import hashlib;

namespace solve::detail {

namespace {

// ---------------------------------------------------------------------------
// phrase solver, first version
//
// The phrase solver from the commit that introduced it (`2fdd97e`), kept as a
// baseline for measuring what the later search optimizations bought.
// The traversal is the original one: a recursive walk over phrase lengths, a
// count-array fit test at internal nodes, a mixed-radix key lookup at leaves.
// Hashing is not the original: `flush` calls the current `md5_batch`, so this
// solver runs on the same SIMD kernels as the optimized phrase solver.
// The batch shape is still the original one (mixed lengths, 64 candidates at a
// time), so the two `hash` phases are not directly comparable, but the kernel
// underneath them is.
//
// Everything the optimized solver added around the traversal is absent:
// packed letter counts, the flat leaf index, the two-word completion table,
// the fixed-size phrase slots, and the prefix copy helpers.
// A difference between the two solvers is therefore mostly a difference in the
// search, which is the point of keeping this version around.
// ---------------------------------------------------------------------------
class PhraseSearchV1 {
public:
    PhraseSearchV1(const solve::SearchProblem& problem, std::string_view phrase,
                   Timer::Timer& setup_timer, solve::Timing& timing,
                   solve::Counters& counters)
        : target_(problem.target.digest), max_candidates_(problem.max_candidates),
          timing_(&timing), counters_(&counters) {
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

        // Longest first, because it consumes the most letters and prunes the deepest levels of the search earlier.
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
        // Always timed: `Stats::elapsed` is user-facing even when the per-phase diagnostics are compiled out.
        Timer::Timer search_timer(true);
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
        search_timer.Stop();
        const std::chrono::nanoseconds elapsed = search_timer.ElapsedDuration();
        if constexpr (kTimingEnabled) {
            // Hashing, comparison, and span preparation happen inside the DFS (flush), so the traversal is the search duration minus those nested phases.
            const std::chrono::nanoseconds nested =
                timing_->generate.prepare + timing_->hash + timing_->match;
            timing_->generate.dfs =
                elapsed > nested ? elapsed - nested : std::chrono::nanoseconds{0};
        }
        outcome.stats = solve::Stats{hashed_, nodes_, elapsed};

        switch (result) {
            case DfsResult::found:
                outcome.status = solve::Status::found;
                outcome.solution = solve::Solution{phrase_, target_, "phrase-v1"};
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
        if constexpr (kTimingEnabled) {
            ++counters_->phrases_emitted;
        }
        if (batch_count_ == batch_.size()) {
            return flush();
        }
        return DfsResult::exhausted;
    }

    DfsResult flush() {
        if (batch_count_ == 0) {
            return DfsResult::exhausted;
        }

        Timer::Timer flush_timer(kTimingEnabled);
        std::array<std::span<const std::byte>, kBatchSize> inputs{};
        std::array<solve::Digest, kBatchSize> digests{};
        for (std::size_t i = 0; i < batch_count_; ++i) {
            inputs[i] = std::as_bytes(std::span{batch_[i].data(), batch_[i].size()});
        }
        [[maybe_unused]] const std::chrono::nanoseconds prepared =
            flush_timer.ElapsedDuration();
        hashlib::md5_batch(std::span<const std::span<const std::byte>>{inputs.data(),
                                                                      batch_count_},
                           std::span<solve::Digest>{digests.data(), batch_count_});
        flush_timer.Stop();
        if constexpr (kTimingEnabled) {
            const std::chrono::nanoseconds elapsed = flush_timer.ElapsedDuration();
            timing_->generate.prepare += prepared;
            timing_->hash += elapsed - prepared;
        }

        std::optional<std::size_t> found;
        {
            [[maybe_unused]] const Timer::ScopedTimer phase(timing_->match, kTimingEnabled);
            for (std::size_t i = 0; i < batch_count_; ++i) {
                if (digests[i] == target_) {
                    found = i;
                    break;
                }
            }
        }

        hashed_ += batch_count_;
        if (found) {
            phrase_ = batch_[*found];
            batch_count_ = 0;
            return DfsResult::found;
        }
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
            if constexpr (kTimingEnabled) {
                ++counters_->leaf_visits;
            }
            for (const std::uint32_t index : bucket->second) {
                const PhraseWord& word = words_[index];
                if constexpr (kTimingEnabled) {
                    ++counters_->leaf_candidates;
                }
                if (!SameCounts(word)) {
                    continue;
                }
                if constexpr (kTimingEnabled) {
                    ++counters_->same_counts_hits;
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
            if constexpr (kTimingEnabled) {
                ++counters_->fits_calls;
            }
            if (!Fits(word)) {
                continue;
            }
            if constexpr (kTimingEnabled) {
                ++counters_->fits_passes;
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
            if constexpr (kTimingEnabled) {
                ++counters_->edges;
            }
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
    solve::Timing* timing_ = nullptr;
    solve::Counters* counters_ = nullptr;
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

}  // namespace

solve::Outcome RunPhraseV1(const solve::SearchProblem& problem, Timer::Timer& setup_timer,
                           solve::Timing& timing, solve::Counters& counters) {
    PhraseSearchV1 search(problem, problem.anagram->letters, setup_timer, timing, counters);
    return search.Run();
}

}  // namespace solve::detail
