#include "FindCallersTool.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>
#include <vector>

#include "PathUtil.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxOutputBytes = 100 * 1024;
constexpr size_t kBinarySniffBytes = 4096;

bool looksBinary(const std::string& sample) {
    for (char c : sample) if (c == '\0') return true;
    return false;
}

// Build a regex that matches function/method calls to `funcName`.
// This matches: funcName( or funcName< or funcName ( or funcName\n(
// Also handles qualified calls: obj.funcName( or ns::funcName(
// We match word-boundary before the name and require a non-word char
// (like paren, bracket, space, newline, semicolon, operator) after it
// to avoid matching definitions or substrings.
std::string callerRegex(const std::string& funcName) {
    // Escape special regex chars in funcName.
    std::string escaped;
    escaped.reserve(funcName.size());
    for (char c : funcName) {
        switch (c) {
            case '\\': case '.': case '*': case '+': case '?':
            case '(': case ')': case '[': case ']': case '{': case '}':
            case '^': case '$': case '|': case '-':
                escaped += '\\';
                escaped += c;
                break;
            default:
                escaped += c;
        }
    }
    // Match at word boundary, allow optional qualifiers (e.g. obj.funcName, ns::funcName)
    // but NOT definition patterns like "void funcName(" or "funcName(...) {" at line start
    // with preceding type keyword. We'll filter definitions out post-match.
    return "\\b" + escaped + "\\s*\\(";
}

// Heuristic: check if a matched line looks like a *definition* rather than a call.
// This is not perfect but catches common patterns.
bool looksLikeDefinition(const std::string& line, const std::string& funcName) {
    // Trim leading whitespace
    size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) return false;

    // Check for common definition patterns at the start of the significant content.
    // We search for the funcName preceded by a type keyword.
    std::string trimmed = line.substr(first);
    static const std::vector<std::string> defKeywords = {
        "void ", "int ", "bool ", "char ", "float ", "double ", "long ",
        "unsigned ", "signed ", "short ", "auto ", "const ",
        "static ", "virtual ", "inline ", "explicit ",
        "std::", "size_t ", "string ", "vector<", "map<",
        "unique_ptr<", "shared_ptr<", "optional<",
        // Class/struct definitions
        "class ", "struct ", "enum ",
        // Templates
        "template<", "template <"
    };

    for (const auto& kw : defKeywords) {
        if (trimmed.find(kw) == 0) {
            // Verify the funcName appears soon after the keyword
            return trimmed.find(funcName) < trimmed.find('(');
        }
    }

    // Also check for "MyType funcName(" or "auto funcName(" patterns
    // by looking for a space before the function name and then a type-like word.
    size_t namePos = trimmed.find(funcName);
    if (namePos != std::string::npos && namePos > 0) {
        // If preceded by something that looks like a type (word, then space),
        // and followed by ( with more stuff before {, it might be a definition.
        if (namePos > 0 && std::isalpha(static_cast<unsigned char>(trimmed[namePos - 1]))) {
            // There's a word character right before the function name --
            // this means it's part of a larger identifier (e.g. "init_funcName")
            // which is a call, not a definition. Don't classify as definition.
            return false;
        }
        // Check if after the ) there's a { on the same line -- strong def signal
        size_t openParen = trimmed.find('(', namePos);
        if (openParen != std::string::npos) {
            size_t closeParen = trimmed.find(')', openParen);
            if (closeParen != std::string::npos) {
                // Check for trailing { or override/final keywords
                std::string after = trimmed.substr(closeParen + 1);
                if (after.find('{') != std::string::npos ||
                    after.find("override") != std::string::npos ||
                    after.find("final") != std::string::npos ||
                    after.find("=") != std::string::npos ||
                    after.find("const") != std::string::npos) {
                    return true;
                }
                // Check for trailing noexcept
                if (after.find("noexcept") != std::string::npos) return true;
            }
        }
    }

    return false;
}

}  // namespace

json FindCallersTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "find_callers"},
            {"description",
             "Find likely call sites for a function or method. Returns file:line "
             "matches and skips common generated/VCS directories."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"function", {{"type", "string"}, {"description", "Function or method name to search for."}}},
                    {"path",     {{"type", "string"}, {"description", "Optional root directory to search. Defaults to project root."}}},
                    {"glob",     {{"type", "string"}, {"description", "Optional file-name glob filter."}}}
                }},
                {"required", json::array({"function"})}
            }}
        }}
    };
}

ToolResult FindCallersTool::execute(const json& args, ToolContext& ctx) {
    if (!args.contains("function")) return ToolResult::error("missing required arg: function");
    const std::string funcName = args["function"].get<std::string>();

    if (funcName.empty()) return ToolResult::error("function name must not be empty");

    fs::path root = ctx.projectRoot;
    if (args.contains("path")) {
        fs::path p = pathutil::resolveSafely(args["path"].get<std::string>(), ctx);
        if (!p.empty()) root = p;
    }

    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return ToolResult::error("not a directory: " + pathutil::toUtf8(root));
    }

    std::optional<std::string> globPat;
    if (args.contains("glob")) globPat = args["glob"].get<std::string>();

    std::optional<std::regex> globRe;
    if (globPat) {
        try {
            globRe = std::regex(pathutil::globToRegex(*globPat), std::regex::ECMAScript);
        } catch (const std::regex_error& e) {
            return ToolResult::error(std::string("invalid glob: ") + e.what());
        }
    }

    std::regex re;
    try {
        re = std::regex(callerRegex(funcName), std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
        return ToolResult::error(std::string("invalid function name: ") + e.what());
    }

    std::ostringstream out;
    size_t bytesEmitted = 0;
    size_t totalCallSites = 0;
    bool truncated = false;

    auto emit = [&](const std::string& s) {
        if (truncated) return;
        if (bytesEmitted + s.size() > kMaxOutputBytes) {
            out << "\n[... truncated at " << kMaxOutputBytes / 1024 << " KB output]\n";
            truncated = true;
            return;
        }
        out << s;
        bytesEmitted += s.size();
    };

    // Collect files
    std::vector<fs::path> files;
    {
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        while (it != end) {
            if (ctx.cancelled && ctx.cancelled->load()) break;
            const fs::directory_entry& entry = *it;
            std::error_code ec2;
            if (entry.is_directory(ec2)) {
                if (pathutil::isExcludedDir(pathutil::toUtf8(entry.path().filename())))
                    it.disable_recursion_pending();
            } else if (entry.is_regular_file(ec2)) {
                files.push_back(entry.path());
            }
            (void)it.increment(ec);
        }
    }

    for (const auto& filePath : files) {
        if (ctx.cancelled && ctx.cancelled->load()) break;
        if (truncated) break;

        std::error_code ecRel;
        const std::string rel = pathutil::toUtf8(fs::relative(filePath, root, ecRel));
        const std::string displayPath = ecRel ? pathutil::toUtf8(filePath) : rel;

        if (globRe && !std::regex_match(displayPath, *globRe))
            continue;

        std::ifstream in(filePath, std::ios::binary);
        if (!in) continue;

        std::string sniff(kBinarySniffBytes, '\0');
        in.read(sniff.data(), kBinarySniffBytes);
        sniff.resize(in.gcount());
        if (looksBinary(sniff)) continue;
        in.clear(); in.seekg(0);

        std::string line;
        int lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            // Strip \r if present
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (std::regex_search(line, re)) {
                // Filter out definition patterns.
                if (looksLikeDefinition(line, funcName))
                    continue;

                ++totalCallSites;
                std::string entry = displayPath + ":" + std::to_string(lineno) + ":" + line + "\n";
                emit(entry);
            }
        }
    }

    if (totalCallSites == 0) {
        return ToolResult::success("[no call sites found for '" + funcName + "']");
    }

    std::string summary = "\n" + std::to_string(totalCallSites) + " call site(s) found for '" + funcName + "'.\n";
    out << summary;
    return ToolResult::success(out.str());
}
