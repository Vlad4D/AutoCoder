#pragma once

#include <filesystem>
#include <regex>
#include <string>
#include <vector>

namespace pathutil {

// gitignore-style ignore matching for the read/context boundary. Loaded from
// `.autocoderignore` and `.gitignore` at the project root (plus a small set of
// built-in secret/credential defaults). Used so files the user does not want
// shared are never read into context and sent to the LLM provider.
//
// Supports the common subset of gitignore syntax: comments (#), blank lines,
// negation (!), directory-only (trailing /), anchoring (leading / or an
// internal slash), and `*`, `?`, `**` globs. Order matters; the last matching
// pattern wins.
class IgnoreRules {
public:
    // Build rules for `projectRoot`, reading its ignore files if present.
    static IgnoreRules load(const std::filesystem::path& projectRoot);

    // True if `absPath` (a file or directory inside the project) is ignored.
    bool isIgnored(const std::filesystem::path& absPath, bool isDir = false) const;

    // ripgrep `--glob` arguments for the patterns ripgrep does NOT already honor
    // on its own (built-in secret defaults + .autocoderignore). ripgrep respects
    // .gitignore natively, so those are omitted to keep the arg list short.
    const std::vector<std::string>& ripgrepExcludeGlobs() const { return rgGlobs_; }

private:
    struct Pattern {
        std::regex re;
        bool negate = false;
        bool dirOnly = false;
        bool anchored = false;
    };

    void addLine(const std::string& rawLine, bool collectRgGlob);
    void addFromFile(const std::filesystem::path& file, bool collectRgGlob);

    std::filesystem::path root_;
    std::vector<Pattern> patterns_;
    std::vector<std::string> rgGlobs_;
};

}  // namespace pathutil
