module solve.detail;

import std;
import solve.types;
import Timer;
import hashlib;

namespace solve::detail {

namespace {

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

// The phrase solver's batch only feeds `md5_batch`; it is larger than the
// generic one so the per-batch bookkeeping (and its timing reads) is amortised
// over more candidates. The candidate budget is still enforced per phrase, so
// a batch can overshoot it by at most this many candidates.
constexpr std::size_t kPhraseBatchSize = 256;

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

// The composed prefix is copied the same way, and its fixed-size move has to
// stay inside the phrase so it cannot rewrite a block that is already padded.
// The first word's emit always covers the bytes past a short prefix.
constexpr std::size_t kPrefixCopyBytes = 16;
// Room past the end of the phrase buffer, which the fixed-size word copy needs.
constexpr std::size_t kSlotSlack = kWordCopyBytes;
// MD5 works on 512-bit blocks, and a slot holds the message with its padding so
// the hash phase never has to build a tail block.
constexpr std::size_t kMd5BlockBytes = 64;
// Bytes of a padded block for a message of `length` bytes: the message, the
// 0x80 marker, the zero fill, and the 64-bit length in the last eight bytes.
constexpr std::size_t PaddedBlockBytes(std::size_t length) {
    return (length + 9 + kMd5BlockBytes - 1) / kMd5BlockBytes * kMd5BlockBytes;
}
// Bytes between two batch slots: one padded block, and room for the fixed-size
// word copy, which may run up to kWordCopyBytes past the phrase.
constexpr std::size_t SlotStride(std::size_t maxLength) {
    const std::size_t padded = PaddedBlockBytes(maxLength);
    return padded > maxLength + kWordCopyBytes ? padded : maxLength + kWordCopyBytes;
}
// Marks a span cache that has not been built yet; no phrase can be this long.
constexpr std::size_t kNoSpanLength = std::numeric_limits<std::size_t>::max();
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

/// Two words that together use exactly the letters of one mixed-radix state.
/// The memo of these lists turns the bottom two levels of the DFS into a table
/// lookup: for a remaining key, every completion is already enumerated.
/// The indices fit in 16 bits because the memo is only built for wordlists up to `kPairMemoWordLimit`, and the emission loop reads this table once per phrase, so halving the record halves the traffic it brings through the cache.
struct PhrasePair {
    std::uint16_t first = 0;
    std::uint16_t second = 0;
};
static_assert(sizeof(PhrasePair) == 4);
static_assert(kPairMemoWordLimit <= std::numeric_limits<std::uint16_t>::max());

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

        std::uint16_t max_target_count = 0;
        for (std::size_t i = 0; i < unique_count_; ++i) {
            max_target_count = std::max(max_target_count, target_counts_[i]);
        }
        // Five bits per distinct letter leave room for twelve letters.
        // A count of at most seven cannot carry into the neighbouring field, which is what makes add and subtract exact.
        packed_counts_ = unique_count_ <= kPackedPhraseLetters && max_target_count <= 7;
        if (packed_counts_) {
            for (std::size_t i = 0; i < unique_count_; ++i) {
                target_packed_ |= static_cast<std::uint64_t>(target_counts_[i]) << (5 * i);
            }
            remaining_packed_ = target_packed_;
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
            word.size = static_cast<std::uint32_t>(token.size());
            word.counts = counts;
            word.mask = mask;
            const std::size_t head_bytes = std::min(token.size(), kWordCopyBytes);
            std::memcpy(word.head.data(), token.data(), head_bytes);
            for (std::size_t i = 0; i < unique_count_; ++i) {
                word.key += static_cast<std::uint64_t>(word.counts[i]) * strides_[i];
            }
            if (packed_counts_) {
                for (std::size_t i = 0; i < unique_count_; ++i) {
                    word.packed |= static_cast<std::uint64_t>(word.counts[i]) << (5 * i);
                }
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
        lazy_pair_possible_ = flat_keys_ && words_.size() <= kPairMemoWordLimit &&
                              unique_count_ <= kPackedPhraseLetters &&
                              max_target_count <= 7;

        remaining_counts_ = target_counts_;
        remaining_length_ = total_letters_;
        remaining_mask_ = full_mask_;
        remaining_packed_ = target_packed_;
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
        batch_stride_ = SlotStride(total_letters_ + max_words_ - 1);
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
            remaining_packed_ = target_packed_;
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

    /// The count-scan fit test, used for inputs the packed layout cannot represent.
    [[nodiscard]] bool FitsByCounts(const PhraseWord& word) const {
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

    [[nodiscard]] bool Fits(const PhraseWord& word) const {
        if (!packed_counts_) {
            return FitsByCounts(word);
        }
        // The high bit of each field acts as a guard: a field whose budget is
        // short borrows through it, so one clear guard bit rejects the word.
        // A borrow also clears the guard of the field that produced it, so a
        // later field cannot hide the rejection.
        const bool fits = (((remaining_packed_ | kPhraseFieldHigh) - word.packed) &
                           kPhraseFieldHigh) == kPhraseFieldHigh;
#ifndef NDEBUG
        if (fits != FitsByCounts(word)) {
            std::abort();
        }
#endif
        return fits;
    }

    [[nodiscard]] bool SameCountsByCounts(const PhraseWord& word) const {
        for (std::size_t i = 0; i < unique_count_; ++i) {
            if (word.counts[i] != remaining_counts_[i]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool SameCounts(const PhraseWord& word) const {
        if (!packed_counts_) {
            return SameCountsByCounts(word);
        }
        const bool same = word.packed == remaining_packed_;
#ifndef NDEBUG
        if (same != SameCountsByCounts(word)) {
            std::abort();
        }
#endif
        return same;
    }

    /// Packs a count array into the five-bit layout.
    /// Debug builds use it to check the packed arithmetic against the count form.
    [[nodiscard]] std::uint64_t PackCounts(
        const std::array<std::uint16_t, kMaxPhraseUnique>& counts) const {
        std::uint64_t packed = 0;
        for (std::size_t i = 0; i < unique_count_; ++i) {
            packed |= static_cast<std::uint64_t>(counts[i]) << (5 * i);
        }
        return packed;
    }

    void SubtractByCounts(const PhraseWord& word) {
        for (std::uint64_t bits = word.mask; bits != 0; bits &= bits - 1) {
            const std::size_t i = static_cast<std::size_t>(std::countr_zero(bits));
            remaining_counts_[i] -= word.counts[i];
            if (remaining_counts_[i] == 0) {
                remaining_mask_ &= ~(std::uint64_t{1} << i);
            }
        }
    }

    void Subtract(const PhraseWord& word) {
        remaining_length_ -= word.size;
        if (!packed_counts_) {
            SubtractByCounts(word);
            return;
        }
        remaining_packed_ -= word.packed;
#ifndef NDEBUG
        // Keep the count form current as well so the two can be compared.
        SubtractByCounts(word);
        if (PackCounts(remaining_counts_) != remaining_packed_) {
            std::abort();
        }
#endif
    }

    void AddByCounts(const PhraseWord& word) {
        for (std::uint64_t bits = word.mask; bits != 0; bits &= bits - 1) {
            const std::size_t i = static_cast<std::size_t>(std::countr_zero(bits));
            remaining_counts_[i] += word.counts[i];
        }
        // Every letter in the word is present again after adding it back.
        remaining_mask_ |= word.mask;
    }

    void Add(const PhraseWord& word) {
        remaining_length_ += word.size;
        if (!packed_counts_) {
            AddByCounts(word);
            return;
        }
        remaining_packed_ += word.packed;
#ifndef NDEBUG
        AddByCounts(word);
        if (PackCounts(remaining_counts_) != remaining_packed_) {
            std::abort();
        }
#endif
    }

    /// Copies `word` into `dst` with a constant-size move; `dst` must be
    /// followed by at least kWordCopyBytes of writable space.
    static void copy_word(char* dst, const PhraseWord& word) {
        if (word.size <= kWordCopyBytes) {
            std::memcpy(dst, word.head.data(), kWordCopyBytes);
        } else {
            std::memcpy(dst, word.text.data(), word.size);
        }
    }

    /// Same idea for the composed prefix, which can be a few words long.
    /// The move is unconditional because the two branches were identical calls differing only in length, so the optimizer merged them into one run-time-length `memcpy` that the backend cannot expand into fixed-size moves.
    /// The leading fixed-size copy keeps the common case inline; only a prefix longer than that reaches the library.
    static void copy_prefix(char* dst, const char* source, std::size_t size) {
        std::memcpy(dst, source, kPrefixCopyBytes);
        if (size > kPrefixCopyBytes) {
            std::memcpy(dst + kPrefixCopyBytes, source + kPrefixCopyBytes, size - kPrefixCopyBytes);
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

    /// Writes the constant part of one slot's padded block: the 0x80 marker at the
    /// end of the message, the zero fill, and the 64-bit message length.
    static void pad_slot(char* slot, std::size_t length) {
        const std::size_t padded = PaddedBlockBytes(length);
        std::memset(slot + length + 1, 0, padded - length - 1 - sizeof(std::uint64_t));
        std::uint64_t bits = static_cast<std::uint64_t>(length) * 8;
        if constexpr (std::endian::native == std::endian::big) {
            bits = std::byteswap(bits);
        }
        std::memcpy(slot + padded - sizeof(bits), &bits, sizeof(bits));
        slot[length] = static_cast<char>(0x80);
    }

    /// Pads every slot for the batch length that starts now.
    /// A phrase length only changes with the word count, so this runs a handful of times per search; every batch of that length then reuses the blocks.
    void pad_slots(std::size_t length) {
        const std::size_t padded = PaddedBlockBytes(length);
        for (std::size_t i = 0; i < kPhraseBatchSize; ++i) {
            pad_slot(batch_storage_.data() + i * batch_stride_, length);
        }
        // The final word's fixed-size copy writes zeros past the phrase, and for a length whose padding is shorter than that overrun it would land in the bit-length field.
        // The condition depends on the length alone, so the check in commit_phrase is the same for every phrase in the batch.
        tail_needs_rewrite_ = padded - length < kWordCopyBytes + sizeof(std::uint64_t);
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
        if (length != batch_length_) {
            pad_slots(length);
            batch_length_ = length;
        }
        return DfsResult::exhausted;
    }

    [[nodiscard]] char* current_slot() {
        return batch_storage_.data() + batch_count_ * batch_stride_;
    }

    /// Publishes the phrase written through `slot`, which the caller already holds: recomputing it would cost a multiply and a reload per phrase.
    DfsResult commit_phrase(char* slot) {
        if (tail_needs_rewrite_) {
            pad_slot(slot, batch_length_);
        } else {
            slot[batch_length_] = static_cast<char>(0x80);
        }
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

        std::size_t length = last.size;
        for (std::size_t i = 0; i < depth; ++i) {
            length += chosen_[i]->size + 1;
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
        return commit_phrase(slot);
    }

    /// Writes `chosen_[0 .. depth-1]`, each followed by a space, into
    /// `phrase_buffer_`; returns the number of characters written.
    std::size_t write_prefix(std::size_t depth) {
        std::size_t length = 0;
        for (std::size_t i = 0; i < depth; ++i) {
            const PhraseWord& word = *chosen_[i];
            copy_word(phrase_buffer_.data() + length, word);
            length += word.size;
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
        return std::span<const PhrasePair>{pair_entries_.get() + begin, count};
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
                lazy_pairs_.push_back(PhrasePair{static_cast<std::uint16_t>(i),
                                                 static_cast<std::uint16_t>(key_entries_[at])});
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
        // A pair can only fit when its two keys sum to at most the target key, and the mixed-radix key is monotone under the letterwise order.
        // Visiting the second word in ascending key order turns that bound into one cut point per first word, so the inner scan stops where no later word can fit.
        // The pair tests on the remaining span still decide the exact letterwise fit, because a key sum below the target does not prove that no letter overflows on its own.
        std::vector<std::uint16_t> second_order(word_count);
        std::vector<std::uint64_t> second_keys(word_count);
        std::vector<std::uint64_t> second_packed(word_count);
        for (std::size_t at = 0; at < word_count; ++at) {
            const std::size_t j = key_entries_[at];
            second_order[at] = static_cast<std::uint16_t>(j);
            second_keys[at] = keys[j];
            if (packed_layout) {
                second_packed[at] = packed[j];
            }
        }
        const auto visit_pairs = [&](auto&& sink) {
            for (std::size_t i = 0; i < word_count; ++i) {
                const std::uint64_t first_key = keys[i];
                // Every word fits the anagram, so its key never exceeds the target key.
                const std::uint64_t bound = target_key_ - first_key;
                const std::size_t limit =
                    static_cast<std::size_t>(std::upper_bound(second_keys.begin(),
                                                              second_keys.end(), bound) -
                                             second_keys.begin());
                if (packed_layout) {
                    const std::uint64_t first = packed[i];
                    for (std::size_t at = 0; at < limit; ++at) {
                        const std::uint64_t difference = check - (first + second_packed[at]);
                        if ((difference & kPhraseFieldHigh) != kPhraseFieldHigh) {
                            continue;
                        }
                        if (!sink(i, second_order[at], first_key + second_keys[at])) {
                            return false;
                        }
                    }
                } else {
                    for (std::size_t at = 0; at < limit; ++at) {
                        const std::size_t j = second_order[at];
                        if (!PairFits(words_[i], words_[j])) {
                            continue;
                        }
                        if (!sink(i, j, first_key + second_keys[at])) {
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
        // The fill writes every entry below, so this storage must not be value-initialised: a memset of the whole table only warms pages the fill is about to dirty one store at a time anyway.
        pair_entries_ = std::make_unique_for_overwrite<PhrasePair[]>(
            static_cast<std::size_t>(total));
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
                        static_cast<std::uint16_t>(i),
                        static_cast<std::uint16_t>(j),
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
            const DfsResult result = commit_phrase(slot);
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

        // One running timer and two peeks instead of three scoped timers: the
        // phases are measured with four clock reads per batch, not six.
        Timer::Timer flush_timer(kTimingEnabled);
        if (inputs_length_ != batch_length_) {
            for (std::size_t i = 0; i < kPhraseBatchSize; ++i) {
                inputs_[i] = std::as_bytes(
                    std::span{batch_storage_.data() + i * batch_stride_, PaddedBlockBytes(batch_length_)});
            }
            inputs_length_ = batch_length_;
        }
        [[maybe_unused]] const std::chrono::nanoseconds prepared =
            flush_timer.ElapsedDuration();
        // Every slot already carries its padded block, so the hash phase is the compression and nothing else.
        // The comparison comes back from the same state the kernel finished in, so no digest is ever written out.
        const std::size_t matched = hashlib::md5_batch_padded_find(
            std::span<const std::span<const std::byte>>{inputs_.data(), batch_count_}, target_);
        flush_timer.Stop();

        if constexpr (kTimingEnabled) {
            const std::chrono::nanoseconds elapsed = flush_timer.ElapsedDuration();
            timing_->generate.prepare += prepared;
            timing_->hash += elapsed - prepared;
        }

        hashed_ += batch_count_;
        if (matched != hashlib::kNoDigestMatch) {
            phrase_.assign(batch_storage_.data() + matched * batch_stride_, batch_length_);
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
    /// True when every distinct letter fits in one five-bit field and every target count is at most seven.
    bool packed_counts_ = false;
    /// The target counts packed five bits per letter; only meaningful when `packed_counts_`.
    std::uint64_t target_packed_ = 0;
    /// The letters still available in the same packed layout, maintained by Subtract and Add.
    std::uint64_t remaining_packed_ = 0;
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
    std::unique_ptr<PhrasePair[]> pair_entries_;
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
    /// True when a length's padding is short enough for the final word's fixed-size copy to land in the bit-length field.
    /// Such a batch pads each slot again in `commit_phrase`, which is the only case that pays per phrase.
    bool tail_needs_rewrite_ = false;
    /// The batch's message spans, kept across flushes because they only depend on `batch_stride_` and the batch length.
    /// A local array would be zeroed on every flush just to be overwritten.
    std::array<std::span<const std::byte>, kPhraseBatchSize> inputs_{};
    /// Length `inputs_` was built for, or `kNoSpanLength` when it still has to be built.
    std::size_t inputs_length_ = kNoSpanLength;
    std::array<const PhraseWord*, kMaxPhraseWords> chosen_{};
    std::vector<char> phrase_buffer_;
    std::string phrase_;
    std::uint64_t hashed_ = 0;
    std::uint64_t nodes_ = 0;
    std::uint64_t wordlist_entries_ = 0;
};

}  // namespace

solve::Outcome RunPhrase(const solve::SearchProblem& problem, Timer::Timer& setup_timer,
                         solve::Timing& timing, solve::Counters& counters) {
    PhraseSearch search(problem, problem.anagram->letters, setup_timer, timing, counters);
    return search.Run();
}

}  // namespace solve::detail
