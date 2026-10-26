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

} // namespace VulkanShared
