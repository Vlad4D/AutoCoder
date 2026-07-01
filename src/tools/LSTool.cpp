#include "LSTool.h"

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

json LSTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "ls"},
            {"description",
             "List directory contents with type, name, and size. Returns up to "
             "500 entries and skips common generated/VCS directories when recursing."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "Directory to list. Defaults to project root."}}},
                    {"recursive", {{"type", "boolean"}, {"description", "Recurse into subdirectories. Default false."}, {"default", false}}},
                    {"pattern", {{"type", "string"}, {"description", "Optional glob filter."}}}
                }},
                {"required", json::array()}
            }}
        }}
    };
}

ToolResult LSTool::execute(const json& args, ToolContext& ctx) {
    fs::path root = ctx.projectRoot;
    if (args.contains("path")) {
        fs::path p = pathutil::resolveSafely(args["path"].get<std::string>(), ctx);
        if (!p.empty()) root = p;
    }

    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return ToolResult::error("not a directory: " + pathutil::toUtf8(root));
    }

    bool recursive = args.value("recursive", false);
    std::string pattern;
    bool hasPattern = args.contains("pattern");
    if (hasPattern) {
        pattern = args["pattern"].get<std::string>();
    }

    struct Entry {
        std::string name;
        std::string type;    // "DIR", "FILE", "SYMLINK", etc.
        uintmax_t size = 0;
        fs::file_time_type mtime;
    };

    std::vector<std::pair<fs::path, Entry>> entries;
    entries.reserve(128);

    const pathutil::IgnoreRules ignore = pathutil::IgnoreRules::load(ctx.projectRoot);

    if (recursive) {
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        while (it != end) {
            if (ctx.cancelled && ctx.cancelled->load()) break;

            const fs::directory_entry& entry = *it;
            std::error_code ec2;

            if (entry.is_directory(ec2)) {
                const std::string dirName = pathutil::toUtf8(entry.path().filename());
                const bool ignored = ignore.isIgnored(entry.path(), true);
                if (pathutil::isExcludedDir(dirName) || ignored) {
                    it.disable_recursion_pending();
                }
                if (!hasPattern && !ignored) {
                    std::string rel = pathutil::toUtf8(fs::relative(entry.path(), root, ec2));
                    entries.emplace_back(entry.path(), Entry{rel, "DIR", 0, entry.last_write_time(ec2)});
                    if (entries.size() >= kMaxResults) break;
                }
            } else if (entry.is_regular_file(ec2) || entry.is_symlink(ec2)) {
                if (ignore.isIgnored(entry.path())) { (void)it.increment(ec); continue; }
                std::string rel = pathutil::toUtf8(fs::relative(entry.path(), root, ec2));
                bool match = true;
                if (hasPattern) {
                    try {
                        std::regex re(pathutil::globToRegex(pattern));
                        match = std::regex_match(rel, re);
                    } catch (...) {
                        match = false;
                    }
                }
                if (match) {
                    std::string t = entry.is_symlink(ec2) ? "SYMLINK" : "FILE";
                    entries.emplace_back(entry.path(),
                        Entry{rel, t, entry.is_regular_file(ec2) ? entry.file_size(ec2) : 0, entry.last_write_time(ec2)});
                    if (entries.size() >= kMaxResults) break;
                }
            }

            (void)it.increment(ec);
        }
    } else {
        fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        fs::directory_iterator end;
        while (it != end) {
            if (ctx.cancelled && ctx.cancelled->load()) break;

            const fs::directory_entry& entry = *it;
            std::error_code ec2;
            std::string name = pathutil::toUtf8(entry.path().filename());

            bool isDir = entry.is_directory(ec2);
            if (isDir && pathutil::isExcludedDir(name)) {
                ++it;
                continue;
            }
            if (ignore.isIgnored(entry.path(), isDir)) {
                ++it;
                continue;
            }

            bool match = true;
            if (hasPattern) {
                try {
                    std::regex re(pathutil::globToRegex(pattern));
                    match = std::regex_match(name, re);
                } catch (...) {
                    match = false;
                }
            }

            if (match) {
                std::string t = isDir ? "DIR" : (entry.is_symlink(ec2) ? "SYMLINK" : "FILE");
                entries.emplace_back(entry.path(),
                    Entry{std::move(name), t,
                          entry.is_regular_file(ec2) || entry.is_symlink(ec2) ? entry.file_size(ec2) : 0,
                          entry.last_write_time(ec2)});
                if (entries.size() >= kMaxResults) break;
            }
            ++it;
        }
    }

    // Sort: directories first, then by name.
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) {
                  bool aDir = a.second.type == "DIR";
                  bool bDir = b.second.type == "DIR";
                  if (aDir != bDir) return aDir > bDir;
                  return a.second.name < b.second.name;
              });

    std::ostringstream out;
    size_t nDirs = 0, nFiles = 0;
    for (const auto& [fullPath, e] : entries) {
        out << "[" << e.type << "] ";
        if (e.type == "FILE" || e.type == "SYMLINK") {
            // Format size with human-readable units.
            std::string sizeStr;
            if (e.size < 1024) {
                sizeStr = std::to_string(e.size) + " B";
            } else if (e.size < 1024 * 1024) {
                sizeStr = std::to_string(e.size / 1024) + " KB";
            } else {
                sizeStr = std::to_string(e.size / (1024 * 1024)) + " MB";
            }
            out << sizeStr << "  ";
        } else {
            out << "       ";
        }
        out << e.name << "\n";
        if (e.type == "DIR") ++nDirs; else ++nFiles;
    }

    std::string result = out.str();
    if (result.empty()) {
        result = "[empty directory or no entries matched: " + pathutil::toUtf8(root) + "]";
    } else {
        // Prepend summary.
        std::ostringstream header;
        header << pathutil::toUtf8(root) << " -- " << nDirs << " dir(s), " << nFiles << " file(s)";
        if (entries.size() >= kMaxResults) {
            header << " [truncated at " << kMaxResults << " entries]";
        }
        header << "\n" << std::string(header.str().size(), '-') << "\n";
        result = header.str() + result;
    }
    return ToolResult::success(std::move(result));
}
