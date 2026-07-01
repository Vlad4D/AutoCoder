#pragma once

// Tiny no-framework test harness: a CHECK macro, a global failure counter, and
// filesystem helpers. Each suite is a free function declared in Suites.h.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace test {

inline int g_failures = 0;
inline int g_checks = 0;

inline void report(bool ok, const char* expr, const char* file, int line,
                   const std::string& extra) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s:%d: %s%s%s\n", file, line, expr,
                    extra.empty() ? "" : "  -- ", extra.c_str());
        std::fflush(stdout);
    }
}

namespace fs = std::filesystem;

inline std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

inline void writeFile(const fs::path& p, const std::string& s) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

// A clean, canonical temp directory unique to `name` (removed if it exists).
inline fs::path freshDir(const std::string& name) {
    fs::path d = fs::temp_directory_path() / "autocoder_tests" / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return fs::weakly_canonical(d, ec);
}

}  // namespace test

#define CHECK(cond)          ::test::report((cond), #cond, __FILE__, __LINE__, "")
#define CHECK_MSG(cond, msg) ::test::report((cond), #cond, __FILE__, __LINE__, (msg))
