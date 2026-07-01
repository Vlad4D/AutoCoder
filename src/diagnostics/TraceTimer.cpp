#include "TraceTimer.h"

#include <QtCore/QStandardPaths>
#include <QtCore/QString>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <filesystem>

namespace diagnostics {

namespace {

std::once_flag g_initFlag;
std::filesystem::path g_logPath;

void initLogPath() {
    std::call_once(g_initFlag, []() {
        QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (base.isEmpty()) {
            base = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.autocoder";
        }
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(base.toStdString()), ec);
        g_logPath = std::filesystem::path(base.toStdString()) / "trace.log";
    });
}

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    struct tm local;
#if defined(_WIN32)
    localtime_s(&local, &tt);
#else
    localtime_r(&tt, &local);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &local);
    return std::string(buf) + "." + std::to_string(ms.count());
}

std::mutex& logMutex() {
    static std::mutex mtx;
    return mtx;
}

}  // anonymous namespace

void TraceTimer::log(const std::string& label, long long us) {
    initLogPath();
    if (g_logPath.empty()) return;

    std::string line;
    if (us >= 10000) {
        line = "[" + timestamp() + "] [trace] " + label + " = " + std::to_string(us / 1000) + " ms\n";
    } else {
        line = "[" + timestamp() + "] [trace] " + label + " = " + std::to_string(us) + " us\n";
    }

    std::lock_guard<std::mutex> lock(logMutex());
    std::ofstream out(g_logPath, std::ios::app);
    if (out) {
        out << line;
    }
}

}  // namespace diagnostics
