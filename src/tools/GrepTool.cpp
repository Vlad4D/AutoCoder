#include "GrepTool.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <vector>

#include <QEventLoop>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "IgnoreRules.h"
#include "PathUtil.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxOutputBytes = 200 * 1024;
constexpr size_t kBinarySniffBytes = 4096;

bool looksBinary(const std::string& sample) {
    for (char c : sample) if (c == '\0') return true;
    return false;
}

enum class OutputMode { Content, FilesWithMatches, Count };

OutputMode parseMode(const std::string& s) {
    if (s == "content")            return OutputMode::Content;
    if (s == "count")              return OutputMode::Count;
    return OutputMode::FilesWithMatches;
}

ToolResult runWithRipgrep(const QString& rg,
                          const std::string& pattern,
                          const fs::path& root,
                          const std::optional<std::string>& globPat,
                          OutputMode mode,
                          std::atomic<bool>* cancelled,
                          const std::vector<std::string>& extraExcludeGlobs) {
    QStringList args;
    args << "--no-heading" << "--line-number" << "--no-messages";
    if (mode == OutputMode::FilesWithMatches) args << "--files-with-matches";
    else if (mode == OutputMode::Count)        args << "--count";

    // Exclude ignored/secret files (ripgrep already honors .gitignore itself).
    for (const std::string& g : extraExcludeGlobs) {
        args << "--glob" << QString::fromStdString(g);
    }

    if (globPat) {
        args << "--glob" << QString::fromStdString(*globPat);
    }
    args << QString::fromStdString(pattern)
         << QString::fromUtf8(pathutil::toUtf8(root).c_str());

    QProcess proc;
    proc.setProgram(rg);
    proc.setArguments(args);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start();
    if (!proc.waitForStarted(5000)) {
        return ToolResult::error("could not start rg: " + proc.errorString().toStdString());
    }

    QEventLoop loop;
    bool wasCancelled = false;
    bool timedOut = false;

    QObject::connect(&proc, &QProcess::finished, &loop, &QEventLoop::quit);

    QTimer cancelTimer;
    cancelTimer.setInterval(150);
    QObject::connect(&cancelTimer, &QTimer::timeout, &loop, [&]() {
        if (cancelled && cancelled->load()) {
            wasCancelled = true;
            proc.kill();
            loop.quit();
        }
    });
    cancelTimer.start();

    QTimer::singleShot(60000, &loop, [&]() {
        timedOut = true;
        proc.kill();
        loop.quit();
    });

    loop.exec();
    if (proc.state() != QProcess::NotRunning) proc.waitForFinished(2000);

    if (wasCancelled) return ToolResult::error("[cancelled by user]");
    if (timedOut)     return ToolResult::error("rg timed out");

    QByteArray out = proc.readAllStandardOutput();
    if (out.size() > static_cast<int>(kMaxOutputBytes)) {
        out = out.left(kMaxOutputBytes) + "\n[... truncated]";
    }
    int exit = proc.exitCode();
    if (out.isEmpty() && exit == 1) {
        return ToolResult::success("[no matches]");
    }
    if (exit != 0 && exit != 1) {
        return ToolResult::error(
            "rg exited " + std::to_string(exit) + ":\n" + out.toStdString());
    }
    return ToolResult::success(out.toStdString().empty() ? "[no matches]" : out.toStdString());
}

ToolResult runFallback(const std::string& pattern,
                       const fs::path& root,
                       const std::optional<std::string>& globPat,
                       OutputMode mode,
                       std::atomic<bool>* cancelled,
                       bool rootIsFile,
                       const pathutil::IgnoreRules& ignore) {
    std::regex re;
    try {
        re = std::regex(pattern, std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
        return ToolResult::error(std::string("invalid regex: ") + e.what());
    }

    std::optional<std::regex> globRe;
    if (globPat) {
        try {
            globRe = std::regex(pathutil::globToRegex(*globPat), std::regex::ECMAScript);
        } catch (const std::regex_error& e) {
            return ToolResult::error(std::string("invalid glob: ") + e.what());
        }
    }

    std::ostringstream out;
    size_t bytesEmitted = 0;
    bool truncated = false;

    auto appendOut = [&](const std::string& s) {
        if (truncated) return;
        if (bytesEmitted + s.size() > kMaxOutputBytes) {
            out << "\n[... truncated]";
            truncated = true;
            return;
        }
        out << s;
        bytesEmitted += s.size();
    };

    // Collect the list of files to search.
    std::vector<fs::path> files;

    if (rootIsFile) {
        if (!ignore.isIgnored(root)) files.push_back(root);
    } else {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            return ToolResult::error("not a directory: " + pathutil::toUtf8(root));
        }

        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;

        while (it != end) {
            if (cancelled && cancelled->load()) break;
            if (truncated) break;

            const fs::directory_entry& entry = *it;
            std::error_code ec2;
            if (entry.is_directory(ec2)) {
                const std::string dirName = pathutil::toUtf8(entry.path().filename());
                if (pathutil::isExcludedDir(dirName) || ignore.isIgnored(entry.path(), true)) {
                    it.disable_recursion_pending();
                }
            } else if (entry.is_regular_file(ec2)) {
                if (!ignore.isIgnored(entry.path())) files.push_back(entry.path());
            }
            (void)it.increment(ec);
        }
    }

    for (const auto& filePath : files) {
        if (cancelled && cancelled->load()) break;
        if (truncated) break;

        std::error_code ecRel;
        const std::string rel = pathutil::toUtf8(fs::relative(filePath, root, ecRel));
        // If relative call failed (e.g. on different drives), fall back to full path.
        const std::string displayPath = ecRel ? pathutil::toUtf8(filePath) : rel;

        // Apply glob filter if present.
        if (globRe && !std::regex_match(displayPath, *globRe)) {
            continue;
        }

        std::ifstream in(filePath, std::ios::binary);
        if (!in) continue;

        std::string sniff(kBinarySniffBytes, '\0');
        in.read(sniff.data(), kBinarySniffBytes);
        sniff.resize(in.gcount());
        if (looksBinary(sniff)) continue;
        in.clear(); in.seekg(0);

        std::string line;
        int lineno = 0;
        int fileMatches = 0;
        std::ostringstream perFile;
        while (std::getline(in, line)) {
            ++lineno;
            if (std::regex_search(line, re)) {
                ++fileMatches;
                if (mode == OutputMode::Content) {
                    perFile << displayPath << ":" << lineno << ":" << line << "\n";
                }
            }
        }

        if (fileMatches > 0) {
            switch (mode) {
                case OutputMode::FilesWithMatches: appendOut(displayPath + "\n"); break;
                case OutputMode::Count:            appendOut(displayPath + ":" + std::to_string(fileMatches) + "\n"); break;
                case OutputMode::Content:          appendOut(perFile.str()); break;
            }
        }
    }

    std::string s = out.str();
    return ToolResult::success(s.empty() ? "[no matches]" : s);
}

}  // namespace

json GrepTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "grep"},
            {"description",
             "Search file contents for a regex pattern. Skips common generated/VCS directories."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"pattern",     {{"type", "string"}, {"description", "Regex or literal text to search for."}}},
                    {"path",        {{"type", "string"}, {"description", "Optional root directory; defaults to project root."}}},
                    {"glob",        {{"type", "string"}, {"description", "Optional file-name glob filter."}}},
                    {"output_mode", {{"type", "string"}, {"enum", json::array({"content", "files_with_matches", "count"})},
                                     {"description", "Output format. Default \"files_with_matches\"."}}}
                }},
                {"required", json::array({"pattern"})}
            }}
        }}
    };
}

ToolResult GrepTool::execute(const json& args, ToolContext& ctx) {
    if (!args.contains("pattern")) return ToolResult::error("missing required arg: pattern");
    const std::string pattern = args["pattern"].get<std::string>();

    fs::path root = ctx.projectRoot;
    if (args.contains("path")) {
        fs::path p = pathutil::resolveSafely(args["path"].get<std::string>(), ctx);
        if (!p.empty()) root = p;
    }

    // If the resolved path is a regular file, search that single file directly
    // by using the fallback or rg with a file argument.
    std::error_code ec2;
    bool rootIsFile = fs::is_regular_file(root, ec2);

    if (!rootIsFile) {
        std::error_code ec3;
        if (!fs::is_directory(root, ec3)) {
            return ToolResult::error("path not found: " + pathutil::toUtf8(root));
        }
    }

    std::optional<std::string> globPat;
    if (args.contains("glob")) globPat = args["glob"].get<std::string>();

    OutputMode mode = parseMode(args.value("output_mode", "files_with_matches"));

    const pathutil::IgnoreRules ignore = pathutil::IgnoreRules::load(ctx.projectRoot);

    QString rg = QStandardPaths::findExecutable("rg");
    if (!rg.isEmpty()) {
        return runWithRipgrep(rg, pattern, root, globPat, mode, ctx.cancelled,
                              ignore.ripgrepExcludeGlobs());
    }
    // Fallback: pass root as search root; if rootIsFile the fallback will handle it.
    return runFallback(pattern, root, globPat, mode, ctx.cancelled, rootIsFile, ignore);
}
