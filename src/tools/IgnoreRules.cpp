#include "tools/IgnoreRules.h"

#include <fstream>

#include "tools/PathUtil.h"

namespace fs = std::filesystem;

namespace pathutil {

namespace {

// Built-in defaults: secrets/credentials that must never reach the provider,
// even when the project ships no ignore files.
const char* kDefaultPatterns[] = {
    ".env", ".env.*", "*.pem", "*.key", "*.pfx", "*.p12", "id_rsa*", "id_ed25519*",
    "*.keystore", "credentials", "credentials.json", ".npmrc", ".netrc",
    ".aws/", ".ssh/", "secrets.*", "*.secret",
};

std::string rtrim(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(0, e);
}

// Convert one gitignore glob (already stripped of !, leading/trailing /) into a
// regex that matches a single relative path string with forward slashes.
std::string globToRegexLocal(const std::string& glob) {
    std::string re;
    re.reserve(glob.size() * 2);
    for (size_t i = 0; i < glob.size(); ++i) {
        char c = glob[i];
        switch (c) {
            case '*':
                if (i + 1 < glob.size() && glob[i + 1] == '*') {
                    re += ".*";  // ** crosses path separators
                    ++i;
                    if (i + 1 < glob.size() && glob[i + 1] == '/') ++i;  // **/ -> .*
                } else {
                    re += "[^/]*";
                }
                break;
            case '?': re += "[^/]"; break;
            case '.': re += "\\."; break;
            case '+': case '(': case ')': case '{': case '}':
            case '[': case ']': case '^': case '$': case '|': case '\\':
                re += '\\';
                re += c;
                break;
            default: re += c; break;
        }
    }
    return re;
}

std::vector<std::string> splitSegments(const std::string& path) {
    std::vector<std::string> segs;
    std::string cur;
    for (char c : path) {
        if (c == '/') {
            if (!cur.empty()) { segs.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) segs.push_back(cur);
    return segs;
}

}  // namespace

void IgnoreRules::addLine(const std::string& rawLine, bool collectRgGlob) {
    std::string line = rtrim(rawLine);
    if (line.empty() || line[0] == '#') return;

    if (collectRgGlob) {
        // ripgrep --glob uses gitignore glob syntax: "!P" excludes P, "P" includes.
        // A gitignore ignore line P maps to rg exclude "!P"; a gitignore "!P"
        // (un-ignore) maps to rg include "P".
        if (line[0] == '!') rgGlobs_.push_back(line.substr(1));
        else                rgGlobs_.push_back("!" + line);
    }

    Pattern p;
    if (line[0] == '!') { p.negate = true; line = line.substr(1); }
    if (!line.empty() && line.back() == '/') { p.dirOnly = true; line.pop_back(); }
    if (line.empty()) return;

    // A leading slash, or any internal slash, anchors the pattern to the root.
    bool leadingSlash = line[0] == '/';
    if (leadingSlash) line = line.substr(1);
    bool internalSlash = line.find('/') != std::string::npos;
    p.anchored = leadingSlash || internalSlash;
    if (line.empty()) return;

    try {
        p.re = std::regex(globToRegexLocal(line), std::regex::ECMAScript);
    } catch (const std::regex_error&) {
        return;  // skip malformed pattern
    }
    patterns_.push_back(std::move(p));
}

void IgnoreRules::addFromFile(const fs::path& file, bool collectRgGlob) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) addLine(line, collectRgGlob);
}

IgnoreRules IgnoreRules::load(const fs::path& projectRoot) {
    IgnoreRules r;
    std::error_code ec;
    r.root_ = fs::weakly_canonical(projectRoot, ec);
    if (r.root_.empty()) r.root_ = projectRoot;

    // Built-in secret defaults and .autocoderignore are collected as rg globs
    // (ripgrep doesn't know them); .gitignore is honored by ripgrep natively.
    for (const char* pat : kDefaultPatterns) r.addLine(pat, /*collectRgGlob=*/true);
    r.addFromFile(projectRoot / ".gitignore", /*collectRgGlob=*/false);
    r.addFromFile(projectRoot / ".autocoderignore", /*collectRgGlob=*/true);  // highest priority
    return r;
}

bool IgnoreRules::isIgnored(const fs::path& absPath, bool isDir) const {
    if (patterns_.empty()) return false;

    std::error_code ec;
    fs::path canon = fs::weakly_canonical(absPath, ec);
    if (canon.empty()) canon = absPath;
    fs::path rel = fs::relative(canon, root_, ec);
    if (ec || rel.empty()) return false;

    std::string relStr = toUtf8(rel);
    for (char& c : relStr) if (c == '\\') c = '/';
    if (relStr.rfind("..", 0) == 0) return false;  // outside root

    const std::vector<std::string> segs = splitSegments(relStr);
    if (segs.empty()) return false;

    bool ignored = false;
    for (const Pattern& p : patterns_) {
        bool matched = false;
        if (p.anchored) {
            // Match the full relative path or any ancestor directory prefix.
            std::string prefix;
            for (size_t k = 0; k < segs.size() && !matched; ++k) {
                if (k) prefix += '/';
                prefix += segs[k];
                const bool prefixIsDir = (k + 1 < segs.size()) || isDir;
                if (p.dirOnly && !prefixIsDir) continue;
                if (std::regex_match(prefix, p.re)) matched = true;
            }
        } else {
            // Non-anchored: match any contiguous run of segments at any depth.
            for (size_t start = 0; start < segs.size() && !matched; ++start) {
                std::string sub;
                for (size_t k = start; k < segs.size() && !matched; ++k) {
                    if (k > start) sub += '/';
                    sub += segs[k];
                    const bool subIsDir = (k + 1 < segs.size()) || isDir;
                    if (p.dirOnly && !subIsDir) continue;
                    if (std::regex_match(sub, p.re)) matched = true;
                }
            }
        }
        if (matched) ignored = !p.negate;
    }
    return ignored;
}

}  // namespace pathutil
