module solve;

import std;
import hashlib;
import Timer;
import solve.types;
import solve.detail;

namespace {

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

// Shared body of both Solve overloads. `total_timer` starts here, so a caller
// that planned itself can add its planning time to the result if it wants the
// whole pipeline.
solve::Outcome SolveWithPlan(const solve::SearchProblem& problem, const solve::Plan& plan) {
    solve::Timing timing;
    solve::Counters counters;
    Timer::Timer setup_timer;
    Timer::Timer total_timer(solve::detail::kTimingEnabled);

    solve::Outcome outcome;
    if (plan.solver == "permutations") {
        outcome = solve::detail::RunPermutations(problem, setup_timer, timing);
    } else if (plan.solver == "phrase") {
        outcome = solve::detail::RunPhrase(problem, setup_timer, timing, counters);
    } else if (plan.solver == "phrase-v1") {
        outcome = solve::detail::RunPhraseV1(problem, setup_timer, timing, counters);
    } else if (plan.solver == "dictionary") {
        outcome = solve::detail::RunDictionary(problem, setup_timer, timing);
    } else {
        // A Plan normally comes from PlanSearch; tolerate a hand-built one.
        outcome.status = solve::Status::invalid;
        outcome.message = "unknown solver '" + std::string(plan.solver) + "'";
    }

    // The run entry points filled generate/hash/match; add the phases that
    // wrap them here.
    timing.setup = setup_timer.ElapsedDuration();
    timing.total = total_timer.ElapsedDuration();
    outcome.stats.timing = timing;
    outcome.stats.counters = counters;
    return outcome;
}

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


Outcome Solve(const SearchProblem& problem, std::string_view requested) {
    std::string error;
    Timer::Timer planning_timer(solve::detail::kTimingEnabled);
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
