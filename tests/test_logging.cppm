module;

// GTest is a preprocessor-based framework, so its headers must stay in the
// global module fragment. They are included before the module declaration and
// before `import std;`; that ordering is also required in normal test TUs.
#include <gtest/gtest.h>

export module test_logging;

import std;
import logiface;
import logiface.ConsoleLogger;

export namespace TestLogging {

// GTest listener that routes per-test logs to TEST_LOG_DIR/[suite]_[test].txt.
class PerTestFileLogger final : public ::testing::EmptyTestEventListener {
public:
    void OnTestStart(const ::testing::TestInfo& info) override {
        const char* env = std::getenv("TEST_LOG_DIR");
        if (!env || *env == '\0') {
            return;  // Logging disabled when directory is not provided.
        }

        const std::filesystem::path dir(env);
        active_ = EnsureDirectory(dir);
        if (!active_) {
            return;
        }

        const std::string filename = std::string(info.test_suite_name()) + "_" + info.name() + ".txt";
        const auto filepath = dir / filename;

        stream_ = std::make_unique<std::ofstream>(filepath);
        if (!stream_->is_open()) {
            stream_.reset();
            active_ = false;
            return;
        }

        logger_.emplace(::Logiface::Level::debug, *stream_, *stream_);
        previous_logger_ = ::Logiface::GetLogger();
        ::Logiface::SetLogger(&logger_.value());
    }

    void OnTestEnd(const ::testing::TestInfo&) override {
        if (active_) {
            ::Logiface::SetLogger(previous_logger_);
        }
        logger_.reset();
        stream_.reset();
        active_ = false;
        previous_logger_ = nullptr;
    }

private:
    static bool EnsureDirectory(const std::filesystem::path& dir) {
        std::error_code ec;
        if (std::filesystem::exists(dir, ec)) {
            return !ec;
        }
        std::filesystem::create_directories(dir, ec);
        return !ec;
    }

    std::unique_ptr<std::ofstream> stream_{};
    std::optional<::Logiface::ConsoleLogger> logger_{};
    ::Logiface::Logger* previous_logger_{};
    bool active_{false};
};

inline void InstallPerTestFileLogger() {
    const static bool installed = [] {
        ::testing::UnitTest::GetInstance()->listeners().Append(new PerTestFileLogger{});
        return true;
    }();
    (void)installed;
}

}  // namespace TestLogging
