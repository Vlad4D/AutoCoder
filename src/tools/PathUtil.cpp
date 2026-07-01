#include "PathUtil.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace pathutil {

namespace fs = std::filesystem;

namespace {

bool startsWithFileScheme(std::string_view s) {
    constexpr std::string_view scheme = "file://";
    if (s.size() < scheme.size()) return false;
    for (size_t i = 0; i < scheme.size(); ++i) {
        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        if (a != scheme[i]) return false;
    }
    return true;
}

bool isHex(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return 0;
}

std::string percentDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && isHex(s[i + 1]) && isHex(s[i + 2])) {
            out.push_back(static_cast<char>((hexValue(s[i + 1]) << 4) | hexValue(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

bool isLocalhost(std::string_view s) {
    constexpr std::string_view local = "localhost";
    if (s.size() != local.size()) return false;
    for (size_t i = 0; i < local.size(); ++i) {
        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        if (a != local[i]) return false;
    }
    return true;
}

std::string rewriteFileUrl(std::string_view s) {
    if (!startsWithFileScheme(s)) return std::string(s);

    std::string_view rest = s.substr(7);
    size_t slash = rest.find('/');
    std::string_view authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);
    std::string_view pathPart = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);

    std::string decodedAuthority = percentDecode(authority);
    std::string decodedPath = percentDecode(pathPart);

#if defined(_WIN32)
    if (decodedAuthority.empty() || isLocalhost(decodedAuthority)) {
        if (decodedPath.size() >= 3
            && decodedPath[0] == '/'
            && std::isalpha(static_cast<unsigned char>(decodedPath[1]))
            && decodedPath[2] == ':') {
            decodedPath.erase(decodedPath.begin());
        }
        if (decodedPath.size() >= 3
            && decodedPath[0] == '/'
            && std::isalpha(static_cast<unsigned char>(decodedPath[1]))
            && decodedPath[2] == '|') {
            decodedPath[0] = decodedPath[1];
            decodedPath[1] = ':';
            decodedPath.erase(decodedPath.begin() + 2);
        }
        return decodedPath;
    }
    if (decodedAuthority.size() == 2
        && std::isalpha(static_cast<unsigned char>(decodedAuthority[0]))
        && (decodedAuthority[1] == ':' || decodedAuthority[1] == '|')) {
        decodedAuthority[1] = ':';
        return decodedAuthority + decodedPath;
    }
    return "//" + decodedAuthority + decodedPath;
#else
    if (decodedAuthority.empty() || isLocalhost(decodedAuthority)) return decodedPath;
    return "//" + decodedAuthority + decodedPath;
#endif
}

#if defined(_WIN32)
bool isDriveLetter(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

std::string rewriteUnixDriveAlias(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');

    // WSL: /mnt/c/path -> C:/path
    if (s.size() >= 6
        && s[0] == '/'
        && s.substr(1, 4) == "mnt/"
        && isDriveLetter(s[5])
        && (s.size() == 6 || s[6] == '/')) {
        std::string out;
        out.reserve(s.size() - 3);
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(s[5]))));
        out += ':';
        out += s.substr(6);
        if (out.size() == 2) out += '/';
        return out;
    }

    // Cygwin: /cygdrive/c/path -> C:/path
    constexpr std::string_view cygdrive = "/cygdrive/";
    if (s.size() >= cygdrive.size() + 1
        && s.compare(0, cygdrive.size(), cygdrive) == 0
        && isDriveLetter(s[cygdrive.size()])
        && (s.size() == cygdrive.size() + 1 || s[cygdrive.size() + 1] == '/')) {
        std::string out;
        out.reserve(s.size() - cygdrive.size() + 2);
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(s[cygdrive.size()]))));
        out += ':';
        out += s.substr(cygdrive.size() + 1);
        if (out.size() == 2) out += '/';
        return out;
    }

    // MSYS/Git Bash: /c/path -> C:/path
    if (s.size() >= 2
        && s[0] == '/'
        && isDriveLetter(s[1])
        && (s.size() == 2 || s[2] == '/')) {
        std::string out;
        out.reserve(s.size() + 1);
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(s[1]))));
        out += ':';
        out += s.substr(2);
        if (out.size() == 2) out += '/';
        return out;
    }

    return s;
}
#endif

}  // namespace

fs::path resolve(const std::string& arg, const ToolContext& ctx) {
    fs::path p = fromUtf8(arg);
    if (p.is_absolute()) return p.lexically_normal();
    return (ctx.projectRoot / p).lexically_normal();
}

bool containedIn(const fs::path& p, const fs::path& root) {
    std::error_code ec;
    fs::path canonicalP = fs::weakly_canonical(p, ec);
    if (ec || canonicalP.empty()) return false;
    fs::path canonicalRoot = fs::weakly_canonical(root, ec);
    if (ec || canonicalRoot.empty()) return false;

    // Component-wise comparison: walk the root components, then verify the
    // remaining path doesn't escape upward via ".." (weakly_canonical already
    // resolves those, so a simple prefix of the path's component sequence works).
    auto rootIt  = canonicalRoot.begin();
    auto rootEnd = canonicalRoot.end();
    auto pIt     = canonicalP.begin();
    auto pEnd    = canonicalP.end();

    for (; rootIt != rootEnd; ++rootIt, ++pIt) {
        if (pIt == pEnd) return false;         // p is shorter = not inside
        if (*rootIt != *pIt) return false;     // component mismatch
    }
    // All root components matched -- p is inside or equal to root
    return true;
}

// resolve() + containment check. Returns empty path if the resolved path
// falls outside ctx.projectRoot.
fs::path resolveSafely(const std::string& arg, const ToolContext& ctx) {
    fs::path p = resolve(arg, ctx);
    if (!containedIn(p, ctx.projectRoot)) return {};
    return p;
}

fs::path fromUtf8(const std::string& s) {
    std::string fileUrlNormalized = rewriteFileUrl(s);
#if defined(_WIN32)
    std::string normalized = rewriteUnixDriveAlias(fileUrlNormalized);
    if (normalized.empty()) return fs::path();
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                  normalized.data(), static_cast<int>(normalized.size()),
                                  nullptr, 0);
    if (len <= 0) {
        len = MultiByteToWideChar(CP_UTF8, 0,
                                  normalized.data(), static_cast<int>(normalized.size()),
                                  nullptr, 0);
    }
    if (len <= 0) return fs::path(normalized);

    std::wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
                        normalized.data(), static_cast<int>(normalized.size()),
                        wide.data(), len);
    return fs::path(wide);
#else
    return fs::path(fileUrlNormalized);
#endif
}

std::string toUtf8(const fs::path& path) {
#if defined(_WIN32)
    std::wstring wide = path.generic_wstring();
    if (wide.empty()) return {};

    int len = WideCharToMultiByte(CP_UTF8, 0,
                                  wide.data(), static_cast<int>(wide.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};

    std::string utf8(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0,
                        wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), len, nullptr, nullptr);
    return utf8;
#else
    return path.generic_string();
#endif
}

std::string globToRegex(const std::string& pattern) {
    std::string out;
    out.reserve(pattern.size() * 2 + 4);
    out += '^';

    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        switch (c) {
            case '*':
                if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                    // **  -> match across directories
                    out += ".*";
                    ++i;
                    // optional trailing slash: "**/foo" matches "foo"
                    if (i + 1 < pattern.size() && pattern[i + 1] == '/') {
                        ++i;
                    }
                } else {
                    out += "[^/]*";
                }
                break;
            case '?':  out += "[^/]"; break;
            case '.':  out += "\\."; break;
            case '+': case '(': case ')': case '|': case '^': case '$':
            case '{': case '}': case '[': case ']': case '\\':
                out += '\\';
                out += c;
                break;
            default:
                out += c;
        }
    }

    out += '$';
    return out;
}

void makeWritable(const fs::path& path, const fs::path& projectRoot) {
#if defined(_WIN32)
    // Only touch files inside the working project directory.
    if (!containedIn(path, projectRoot)) return;

    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(path, ec);
    if (ec) return;

    DWORD attrs = GetFileAttributesW(canonical.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if (!(attrs & FILE_ATTRIBUTE_READONLY)) return;

    SetFileAttributesW(canonical.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
#else
    (void)path;
    (void)projectRoot;
#endif
}

bool isExcludedDir(const std::string& dirName) {
    static const std::array<std::string_view, 12> excluded = {
        ".git", ".svn", ".hg",
        "node_modules", "vendor", "third_party",
        "build", "out", "dist", "target",
        ".vs", ".idea"
    };
    for (auto& e : excluded) {
        if (dirName == e) return true;
    }
    return false;
}

}  // namespace pathutil
