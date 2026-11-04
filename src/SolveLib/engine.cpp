module solve.detail;

import std;
import solve.types;
import solve;
import Timer;
import hashlib;

namespace solve::detail {

namespace {

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

}  // namespace

solve::Outcome RunPermutations(const solve::SearchProblem& problem,
                               Timer::Timer& setup_timer, solve::Timing& timing) {
    PermutationSource source(problem.anagram->letters, setup_timer);
    return RunEngine(problem, source, "permutations", timing);
}

solve::Outcome RunDictionary(const solve::SearchProblem& problem,
                             Timer::Timer& setup_timer, solve::Timing& timing) {
    solve::Outcome outcome;
    if (!problem.wordlist) {
        outcome.status = solve::Status::invalid;
        outcome.message = "solver 'dictionary' requires a wordlist (--wordlist)";
        return outcome;
    }
    DictionarySource source(problem.wordlist->path, problem.anagram->letters, setup_timer);
    if (!source.is_open()) {
        outcome.status = solve::Status::invalid;
        outcome.message = source.error().empty()
                              ? "cannot open wordlist: " + problem.wordlist->path.string()
                              : source.error();
        return outcome;
    }
    return RunEngine(problem, source, "dictionary", timing);
}

}  // namespace solve::detail
