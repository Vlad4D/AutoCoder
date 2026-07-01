#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>

namespace diagnostics {

/// Thread-safe high-resolution timer that appends a trace line to a log file
/// on destruction.  Use like:
///   auto _ = TraceTimer("my label");
/// The log file is created at the first TraceTimer instantiation.
class TraceTimer {
public:
    explicit TraceTimer(std::string_view label)
        : label_(label)
        , start_(clock::now()) {}

    ~TraceTimer() {
        auto elapsed = clock::now() - start_;
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
        log(label_, us);
    }

    // Allow move only (no copy).
    TraceTimer(const TraceTimer&) = delete;
    TraceTimer& operator=(const TraceTimer&) = delete;
    TraceTimer(TraceTimer&&) = default;
    TraceTimer& operator=(TraceTimer&&) = default;

private:
    using clock = std::chrono::steady_clock;

    static void log(const std::string& label, long long us);

    std::string label_;
    clock::time_point start_;
};

}  // namespace diagnostics
