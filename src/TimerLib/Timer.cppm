module;

export module Timer;

import std;

export namespace Timer {

class Timer {
public:
    Timer() noexcept;
    explicit Timer(bool start) noexcept;

    void Start() noexcept;
    void Stop() noexcept;
    void Restart() noexcept;

    [[nodiscard]] double ElapsedMs() const noexcept;
    [[nodiscard]] double ElapsedUs() const noexcept;
    [[nodiscard]] double ElapsedNs() const noexcept;
    [[nodiscard]] double ElapsedS() const noexcept;
    [[nodiscard]] std::int64_t ElapsedNsInt() const noexcept;
    [[nodiscard]] std::chrono::nanoseconds ElapsedDuration() const noexcept;

    [[nodiscard]] bool IsRunning() const noexcept;

private:
    std::int64_t accumulated_;
    std::int64_t start_time_;
    bool running_;

    [[nodiscard]] std::int64_t ElapsedRaw() const noexcept;
    static std::int64_t Now() noexcept;
};

/// Starts `timer` for the lifetime of the object and stops it again, so timing
/// a scope stays correct when the scope has early returns. `enabled == false`
/// makes the whole object a no-op, which lets a caller compile its measurements
/// out while keeping the same code shape:
///
///     {
///         ScopedTimer phase(hash_timer, timing_enabled);
///         md5_batch(...);
///     }
///
/// The duration-sink form is for a breakdown whose fields are plain durations
/// instead of whole timers:
///
///     ScopedTimer phase(timing.generate.prepare, timing_enabled);
class ScopedTimer {
public:
    ScopedTimer(Timer& timer, bool enabled = true) noexcept
        : timer_(&timer), sink_(nullptr), enabled_(enabled) {
        if (enabled_) {
            timer_->Start();
        }
    }

    ScopedTimer(std::chrono::nanoseconds& sink, bool enabled = true) noexcept
        : timer_(&owned_timer_), sink_(&sink), enabled_(enabled) {
        if (enabled_) {
            timer_->Start();
        }
    }

    ~ScopedTimer() noexcept {
        if (!enabled_) {
            return;
        }
        timer_->Stop();
        if (sink_ != nullptr) {
            *sink_ += timer_->ElapsedDuration();
        }
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    Timer owned_timer_;
    Timer* timer_;
    std::chrono::nanoseconds* sink_;
    bool enabled_;
};

/// Formats a duration with three significant digits and an auto-scaled unit (ns, us, ms, s, min, h).
/// Zero prints as "0 ns".
[[nodiscard]] std::string FormatDuration(std::chrono::nanoseconds duration);

} // namespace Timer
