module;

module Timer;

import std;
import std.compat;

namespace Timer {

Timer::Timer() noexcept
    : accumulated_(0)
    , start_time_(0)
    , running_(false)
{
}

Timer::Timer(const bool start) noexcept
    : accumulated_(0)
    , start_time_(start ? Now() : 0)
    , running_(start)
{
}

void Timer::Start() noexcept {
    if (!running_) {
        start_time_ = Now();
        running_ = true;
    }
}

void Timer::Stop() noexcept {
    if (running_) {
        accumulated_ += Now() - start_time_;
        running_ = false;
    }
}

void Timer::Restart() noexcept {
    accumulated_ = 0;
    start_time_ = Now();
    running_ = true;
}

double Timer::ElapsedMs() const noexcept {
    return static_cast<double>(ElapsedRaw()) / 1000000.0;
}

double Timer::ElapsedUs() const noexcept {
    return static_cast<double>(ElapsedRaw()) / 1000.0;
}

double Timer::ElapsedNs() const noexcept {
    return static_cast<double>(ElapsedRaw());
}

double Timer::ElapsedS() const noexcept {
    return static_cast<double>(ElapsedRaw()) / 1000000000.0;
}

std::int64_t Timer::ElapsedNsInt() const noexcept {
    return ElapsedRaw();
}

std::chrono::nanoseconds Timer::ElapsedDuration() const noexcept {
    return std::chrono::nanoseconds{ElapsedRaw()};
}

bool Timer::IsRunning() const noexcept {
    return running_;
}

std::int64_t Timer::ElapsedRaw() const noexcept {
    if (running_) {
        return accumulated_ + (Now() - start_time_);
    }
    return accumulated_;
}

std::int64_t Timer::Now() noexcept {
    using namespace std::chrono;

    return duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

std::string FormatDuration(std::chrono::nanoseconds duration) {
    using namespace std::chrono;

    const std::int64_t raw = duration.count();
    if (raw == 0) {
        return "0 ns";
    }

    const bool negative = raw < 0;
    const std::uint64_t magnitude = negative
        ? static_cast<std::uint64_t>(-(raw + 1)) + 1
        : static_cast<std::uint64_t>(raw);

    struct Unit {
        std::uint64_t nanoseconds;
        std::string_view suffix;
    };
    constexpr std::array<Unit, 6> kUnits{{
        {1ULL, "ns"},
        {1'000ULL, "us"},
        {1'000'000ULL, "ms"},
        {1'000'000'000ULL, "s"},
        {60'000'000'000ULL, "min"},
        {3'600'000'000'000ULL, "h"},
    }};

    // Largest unit that keeps the value at or above one.
    std::size_t unit = 0;
    while (unit + 1 < kUnits.size() && magnitude >= kUnits[unit + 1].nanoseconds) {
        ++unit;
    }

    const auto decimals_for = [](double value) {
        if (value >= 100.0) {
            return 0;
        }
        if (value >= 10.0) {
            return 1;
        }
        return 2;
    };
    const auto round_to = [](double value, int decimals) {
        const double factor = decimals == 0 ? 1.0 : (decimals == 1 ? 10.0 : 100.0);
        return std::round(value * factor) / factor;
    };

    // Rounding can push a value onto the next unit's boundary (999.6 ms ->
    // 1.00 s, 59.96 s -> 1.00 min). Promote so the result keeps three
    // significant digits and stays below the 1000/60 boundary.
    double value = static_cast<double>(magnitude) / static_cast<double>(kUnits[unit].nanoseconds);
    while (unit + 1 < kUnits.size()) {
        const int decimals = decimals_for(value);
        const double rounded = round_to(value, decimals);
        const double boundary = unit >= 3 ? 60.0 : 1000.0;
        if (rounded < boundary) {
            break;
        }
        ++unit;
        value = static_cast<double>(magnitude) / static_cast<double>(kUnits[unit].nanoseconds);
    }

    const int decimals = decimals_for(value);
    const std::int64_t scale = decimals == 0 ? 1 : (decimals == 1 ? 10 : 100);
    const std::int64_t scaled =
        static_cast<std::int64_t>(std::llround(value * static_cast<double>(scale)));

    std::string text;
    if (negative) {
        text.push_back('-');
    }
    text += std::to_string(scaled / scale);
    if (decimals > 0) {
        const std::string fraction = std::to_string(scaled % scale);
        text.push_back('.');
        text.append(static_cast<std::size_t>(decimals) - fraction.size(), '0');
        text += fraction;
    }
    text.push_back(' ');
    text.append(kUnits[unit].suffix);
    return text;
}

} // namespace Timer
