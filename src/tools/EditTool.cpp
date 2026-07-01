#include "EditTool.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#include "PathUtil.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Replace all occurrences of `needle` in `hay` with `repl`. Returns count.
size_t replaceAll(std::string& hay, const std::string& needle, const std::string& repl) {
    if (needle.empty()) return 0;
    size_t pos = 0, count = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        hay.replace(pos, needle.size(), repl);
        pos += repl.size();
        ++count;
    }
    return count;
}

// Detect the dominant line-ending style: true if CRLF, false if LF-only.
// Examines the full content for \r\n sequences.
bool hasCrlfStyle(const std::string& content) {
    for (size_t i = 1; i < content.size(); ++i) {
        if (content[i] == '\n' && content[i - 1] == '\r')
            return true;
    }
    return false;
}

// Normalise line endings in `s` from CRLF -> LF in-place.
void normaliseLf(std::string& s) {
    // Remove all \r that are followed by \n (CRLF -> LF).
    // Walk the string and keep characters that are not \r (or are \r not followed by \n).
    // The latter is extremely rare but we preserve it.
    size_t w = 0;
    for (size_t r = 0; r < s.size(); ++r) {
        if (s[r] == '\r' && (r + 1 < s.size() && s[r + 1] == '\n'))
            continue;   // skip CR, the LF will be copied on the next iteration
        s[w++] = s[r];
    }
    s.resize(w);
}

// Normalise line endings in `s` from LF -> CRLF in-place.
// This is the inverse of normaliseLf.
void normaliseCrlf(std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 2);  // worst-case every char is LF
    for (char c : s) {
        if (c == '\n')
            out.push_back('\r');
        out.push_back(c);
    }
    s = std::move(out);
}

// Numbered snippet of the (LF-normalised) post-edit content around the
// replacement at [replaceStart, replaceStart+replaceLen), with a few context
// lines, so the model can verify the result without re-reading the file.
std::string resultSnippet(const std::string& content, size_t replaceStart,
                          size_t replaceLen) {
    constexpr size_t kContextLines = 3;
    constexpr size_t kMaxEchoLines = 40;

    std::vector<size_t> starts{0};
    for (size_t i = 0; i + 1 < content.size(); ++i)
        if (content[i] == '\n') starts.push_back(i + 1);

    auto lineOf = [&](size_t off) {
        if (!content.empty() && off >= content.size()) off = content.size() - 1;
        auto it = std::upper_bound(starts.begin(), starts.end(), off);
        return static_cast<size_t>(it - starts.begin()) - 1;
    };

    const size_t firstLine = lineOf(replaceStart);
    const size_t lastLine  = lineOf(replaceLen ? replaceStart + replaceLen - 1
                                               : replaceStart);
    const size_t begin = firstLine >= kContextLines ? firstLine - kContextLines : 0;
    size_t endL = std::min(lastLine + kContextLines, starts.size() - 1);
    bool elided = false;
    if (endL - begin + 1 > kMaxEchoLines) {
        endL = begin + kMaxEchoLines - 1;
        elided = true;
    }

    std::ostringstream o;
    o << "Result (lines " << begin + 1 << "-" << endL + 1 << "):\n";
    for (size_t ln = begin; ln <= endL; ++ln) {
        const size_t s = starts[ln];
        size_t e = (ln + 1 < starts.size()) ? starts[ln + 1] : content.size();
        while (e > s && (content[e - 1] == '\n' || content[e - 1] == '\r')) --e;
        o << ln + 1 << "\t" << content.substr(s, e - s) << "\n";
    }
    if (elided) o << "[... rest of the changed region not shown]\n";
    return o.str();
}

}  // namespace

json EditTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "edit"},
            {"description",
             "Replace exact text in a file that was read earlier in this conversation. "
             "`old_string` must occur once unless replace_all is true. Preserve "
             "indentation exactly; line endings are handled automatically."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"path",        {{"type", "string"}, {"description", "Absolute or project-relative path."}}},
                    {"old_string",  {{"type", "string"}, {"description", "Exact text to replace, including whitespace."}}},
                    {"new_string",  {{"type", "string"}, {"description", "Replacement text."}}},
                    {"replace_all", {{"type", "boolean"},{"description", "Replace every occurrence; default false (unique-match required)."}}}
                }},
                {"required", json::array({"path", "old_string", "new_string"})}
            }}
        }}
    };
}

ToolResult EditTool::execute(const json& args, ToolContext& ctx) {
    if (!args.contains("path"))       return ToolResult::error("missing required arg: path");
    if (!args.contains("old_string")) return ToolResult::error("missing required arg: old_string");
    if (!args.contains("new_string")) return ToolResult::error("missing required arg: new_string");

    const std::string pathArg   = args["path"].get<std::string>();
    const std::string oldStr    = args["old_string"].get<std::string>();
    const std::string newStr    = args["new_string"].get<std::string>();
    const bool replaceAllFlag   = args.value("replace_all", false);

    if (oldStr.empty()) return ToolResult::error("old_string must not be empty");

    fs::path path = pathutil::resolveSafely(pathArg, ctx);
    if (path.empty()) return ToolResult::error(
        "refusing to edit outside project root: " + pathArg);
    if (!fs::exists(path)) return ToolResult::error("file does not exist: " + pathutil::toUtf8(path));

    if (ctx.readSet) {
        std::error_code ec2;
        fs::path canonical = fs::weakly_canonical(path, ec2);
        const fs::path& key = canonical.empty() ? path : canonical;
        if (ctx.readSet->find(key) == ctx.readSet->end()) {
            return ToolResult::error(
                "refusing to edit a file you have not read in this conversation: "
                + pathutil::toUtf8(path) + ". Call `read` on it first.");
        }
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) return ToolResult::error("could not open for reading: " + pathutil::toUtf8(path));
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string content = buf.str();
    in.close();

    // Snapshot old content for checkpoint before modifying.
    if (ctx.fileBeforeSnapshots) {
        std::error_code ec3;
        fs::path canonical2 = fs::weakly_canonical(path, ec3);
        const fs::path& key2 = canonical2.empty() ? path : canonical2;
        if (ctx.fileBeforeSnapshots->find(key2) == ctx.fileBeforeSnapshots->end()) {
            (*ctx.fileBeforeSnapshots)[key2] = content;
        }
    }

    // Normalise the file content and the old_string to LF-only so that CRLF
    // mismatches between what the LLM sees (ReadTool strips \r) and what the
    // file contains do not prevent matching.  We preserve the original
    // line-ending style in the final output.
    const bool originalIsCrlf = hasCrlfStyle(content);
    const std::string oldStrNormalised = [&]() {
        std::string s = oldStr;
        normaliseLf(s);
        return s;
    }();
    normaliseLf(content);

    size_t first = content.find(oldStrNormalised);
    if (first == std::string::npos) {
        return ToolResult::error("old_string not found in " + pathutil::toUtf8(path));
    }

    size_t replacements = 0;
    std::string newStrNormalised = newStr;
    normaliseLf(newStrNormalised);

    if (replaceAllFlag) {
        replacements = replaceAll(content, oldStrNormalised, newStrNormalised);
    } else {
        size_t second = content.find(oldStrNormalised, first + oldStrNormalised.size());
        if (second != std::string::npos) {
            return ToolResult::error(
                "old_string occurs more than once in " + pathutil::toUtf8(path)
                + "; pass replace_all=true or supply more surrounding context.");
        }
        content.replace(first, oldStrNormalised.size(), newStrNormalised);
        replacements = 1;
    }

    // The first replacement site stays at `first` even with replace_all
    // (later sites shift, the first cannot). Capture the echo snippet while
    // the content is still LF-normalised.
    std::string snippet = resultSnippet(content, first, newStrNormalised.size());

    // Restore original line-ending style so we don't accidentally convert
    // the whole file.
    if (originalIsCrlf)
        normaliseCrlf(content);

    pathutil::makeWritable(path, ctx.projectRoot);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return ToolResult::error("could not open for writing: " + pathutil::toUtf8(path));
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) return ToolResult::error("write failed: " + pathutil::toUtf8(path));

    // Mark the file as "read" so future edits in the same conversation work.
    if (ctx.readSet) {
        std::error_code ec4;
        fs::path canonical = fs::weakly_canonical(path, ec4);
        ctx.readSet->insert(canonical.empty() ? path : canonical);
    }

    std::string msg = "applied " + std::to_string(replacements)
        + " replacement(s) in " + pathutil::toUtf8(path);
    if (replacements > 1) msg += " (snippet shows the first site)";
    msg += ". " + snippet;
    return ToolResult::success(std::move(msg));
}
