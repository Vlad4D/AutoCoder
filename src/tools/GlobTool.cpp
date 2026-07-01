#include "GlobTool.h"

#include <algorithm>
#include <regex>
#include <sstream>
#include <vector>

#include "IgnoreRules.h"
#include "PathUtil.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {
constexpr size_t kMaxResults = 500;
}

json GlobTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "glob"},
            {"description",
             "Find files by glob pattern. Returns matching paths sorted by "
             "modification time and skips common generated/VCS directories."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"pattern", {{"type", "string"}, {"description", "Glob pattern."}}},
                    {"path",    {{"type", "string"}, {"description", "Optional root directory. Defaults to project root."}}}
                }},
                {"required", json::array({"pattern"})}
            }}
        }}
    };
}

ToolResult GlobTool::execute(const json& args, ToolContext& ctx) {
    if (!args.contains("pattern")) return ToolResult::error("missing required arg: pattern");
    const std::string pattern = args["pattern"].get<std::string>();

    fs::path root = ctx.projectRoot;
    if (args.contains("path")) {
        fs::path p = pathutil::resolveSafely(args["path"].get<std::string>(), ctx);
        if (!p.empty()) root = p;
    }

    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return ToolResult::error("not a directory: " + pathutil::toUtf8(root));
    }

    std::regex re;
    try {
        re = std::regex(pathutil::globToRegex(pattern), std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
        return ToolResult::error(std::string("invalid pattern: ") + e.what());
    }

    const pathutil::IgnoreRules ignore = pathutil::IgnoreRules::load(ctx.projectRoot);

    std::vector<std::pair<fs::file_time_type, fs::path>> matches;
    matches.reserve(64);

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    while (it != end) {
        if (ctx.cancelled && ctx.cancelled->load()) break;

        const fs::directory_entry& entry = *it;
        std::error_code ec2;
        if (entry.is_directory(ec2)) {
            const std::string dirName = pathutil::toUtf8(entry.path().filename());
            if (pathutil::isExcludedDir(dirName) || ignore.isIgnored(entry.path(), true)) {
                it.disable_recursion_pending();
            }
        } else if (entry.is_regular_file(ec2)) {
            std::string rel = pathutil::toUtf8(fs::relative(entry.path(), root, ec2));
            if (std::regex_match(rel, re) && !ignore.isIgnored(entry.path())) {
                matches.emplace_back(entry.last_write_time(ec2), entry.path());
                if (matches.size() >= kMaxResults) break;
            }
        }
        (void)it.increment(ec);
    }

    std::sort(matches.begin(), matches.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::ostringstream out;
    for (const auto& [_, p] : matches) {
        out << pathutil::toUtf8(p) << "\n";
    }

    std::string s = out.str();
    if (s.empty()) {
        s = "[no files matched pattern: " + pattern + "]";
    } else if (matches.size() >= kMaxResults) {
        s += "[... truncated at " + std::to_string(kMaxResults) + " results]\n";
    }
    return ToolResult::success(std::move(s));
}
