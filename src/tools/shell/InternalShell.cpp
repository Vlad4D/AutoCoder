#include "InternalShell.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <vector>

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "tools/PathUtil.h"
#include "tools/shell/DebugRunner.h"
#include "tools/shell/ShellGrammar.h"

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace autocoder::shell {

namespace fs = std::filesystem;

namespace {

enum class Connector { Always, And, Or, Pipe };

struct CommandNode {
    Connector connector = Connector::Always;
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string>> envAssignments;
};

struct Token {
    enum class Type { Word, Semicolon, And, Or, Pipe };
    Type type = Type::Word;
    std::string text;
};

struct ShellState {
    fs::path cwd;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
};

struct ExecResult {
    int exitCode = 0;
    bool ok = true;
    bool cancelled = false;
    bool timedOut = false;
    std::string output;
    std::string error;
};

bool isNameStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool isNameChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool isEnvAssignment(std::string_view s) {
    if (s.empty() || !isNameStart(s[0])) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i] == '=') return i > 0;
        if (!isNameChar(s[i])) return false;
    }
    return false;
}

bool hasAt(const std::string& s, size_t pos, std::string_view needle) {
    return pos + needle.size() <= s.size()
        && std::string_view(s).substr(pos, needle.size()) == needle;
}

bool isShellSpecialEscape(char c) {
    switch (c) {
        case ' ': case '\t': case '\r': case '\n':
        case '$': case '"': case '\'': case '\\':
        case ';': case '&': case '|': case '<': case '>':
        case '(': case ')':
            return true;
        default:
            return false;
    }
}

bool isNullDeviceName(std::string_view s) {
    if (s == "/dev/null") return true;
    if (s.size() != 3) return false;
    return std::tolower(static_cast<unsigned char>(s[0])) == 'n'
        && std::tolower(static_cast<unsigned char>(s[1])) == 'u'
        && std::tolower(static_cast<unsigned char>(s[2])) == 'l';
}

bool consumeNullRedirection(const std::string& command, size_t& i, const std::string& word) {
    if (command[i] != '>') return false;
    if (!word.empty() && word != "1" && word != "2") return false;
    if (i + 1 < command.size() && command[i + 1] == '>') return false;

    size_t j = i + 1;
    while (j < command.size() && std::isspace(static_cast<unsigned char>(command[j]))) ++j;
    const size_t len = hasAt(command, j, "/dev/null") ? 9 : 3;
    if (j + len <= command.size() && isNullDeviceName(std::string_view(command).substr(j, len))) {
        size_t end = j + len;
        if (end == command.size()
            || std::isspace(static_cast<unsigned char>(command[end]))
            || command[end] == ';'
            || command[end] == '&'
            || command[end] == '|') {
            i = end - 1;
            return true;
        }
    }
    return false;
}

// Exit-code variables (%ERRORLEVEL%, $?) cannot be expanded at lex time: the
// whole command line is lexed before anything runs, so they would always be
// empty. They expand to this marker, which the execution loop replaces with
// the previous segment's exit code just before each command runs.
constexpr std::string_view kExitCodeMarker = "\x01" "ERRORLEVEL" "\x01";

std::string envValue(const ShellState& state, std::string_view name) {
    QString key = QString::fromStdString(std::string(name));
    if (!state.env.contains(key)) {
        QString upper = key.toUpper();
        if (upper == QStringLiteral("ERRORLEVEL"))
            return std::string(kExitCodeMarker);
    }
    return state.env.value(key).toStdString();
}

void expandVariable(const std::string& in, size_t& i, std::string& out, const ShellState& state) {
    if (i + 1 >= in.size()) {
        out.push_back('$');
        return;
    }

    if (in[i + 1] == '{') {
        size_t end = in.find('}', i + 2);
        if (end == std::string::npos) {
            out.push_back('$');
            return;
        }
        out += envValue(state, std::string_view(in).substr(i + 2, end - i - 2));
        i = end;
        return;
    }

    if (in[i + 1] == '?') {
        // POSIX last-exit-code variable; resolved at execution time.
        out += kExitCodeMarker;
        ++i;
        return;
    }

    if (!isNameStart(in[i + 1])) {
        out.push_back('$');
        return;
    }

    size_t end = i + 2;
    while (end < in.size() && isNameChar(in[end])) ++end;
    out += envValue(state, std::string_view(in).substr(i + 1, end - i - 1));
    i = end - 1;
}

std::string expandPercentVariables(const std::string& in, const ShellState& state) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out.push_back(in[i]);
            continue;
        }
        size_t end = in.find('%', i + 1);
        if (end == std::string::npos || end == i + 1) {
            out.push_back(in[i]);
            continue;
        }
        out += envValue(state, std::string_view(in).substr(i + 1, end - i - 1));
        i = end;
    }
    return out;
}

std::optional<std::vector<Token>> lex(const std::string& command,
                                      const ShellState& state,
                                      std::string& error) {
    std::vector<Token> tokens;
    std::string word;
    ShellState expansionState = state;
    QProcessEnvironment commandStartEnv = state.env;
    bool commandHasNonAssignment = false;
    bool inSingle = false;
    bool inDouble = false;

    auto flushWord = [&]() {
        if (!word.empty()) {
            if (!commandHasNonAssignment && isEnvAssignment(word)) {
                size_t eq = word.find('=');
                expansionState.env.insert(QString::fromStdString(word.substr(0, eq)),
                                          QString::fromStdString(word.substr(eq + 1)));
            } else {
                commandHasNonAssignment = true;
            }
            tokens.push_back(Token{Token::Type::Word, std::move(word)});
            word.clear();
        }
    };

    auto finishCommand = [&]() {
        flushWord();
        if (commandHasNonAssignment) {
            expansionState.env = commandStartEnv;
        } else {
            commandStartEnv = expansionState.env;
        }
        commandHasNonAssignment = false;
    };

    for (size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];

        if (inSingle) {
            if (c == '\'') inSingle = false;
            else word.push_back(c);
            continue;
        }

        if (inDouble) {
            if (c == '"') {
                inDouble = false;
            } else if (c == '\\' && i + 1 < command.size()) {
                const char next = command[i + 1];
                if (next == '$' || next == '"' || next == '\\' || next == '\n') {
                    if (next != '\n') word.push_back(next);
                    ++i;
                } else {
                    word.push_back(c);
                }
            } else if (c == '$') {
                expandVariable(command, i, word, expansionState);
            } else {
                word.push_back(c);
            }
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c))) {
            flushWord();
            continue;
        }
        if (c == '\'') {
            inSingle = true;
            continue;
        }
        if (c == '"') {
            inDouble = true;
            continue;
        }
        if (c == '\\' && i + 1 < command.size()) {
            const char next = command[i + 1];
            if (isShellSpecialEscape(next)) {
                word.push_back(next);
                ++i;
            } else {
                word.push_back(c);
            }
            continue;
        }
        if (c == '$') {
            if (i + 1 < command.size() && command[i + 1] == '(') {
                error = "unsupported shell syntax: command substitution";
                return std::nullopt;
            }
            expandVariable(command, i, word, expansionState);
            continue;
        }
        if (c == '`') {
            error = "unsupported shell syntax: command substitution";
            return std::nullopt;
        }
        if (!word.empty() && (word == "1" || word == "2") && hasAt(command, i, ">&1")) {
            // AutoCoder already merges stdout and stderr. Accept common shell
            // capture idioms such as `2>&1` as no-ops for model compatibility.
            word.clear();
            i += 3;
            continue;
        }
        if (!word.empty() && word == "1" && hasAt(command, i, ">&2")) {
            word.clear();
            i += 3;
            continue;
        }
        if (c == '>' && hasAt(command, i, ">&1")) {
            i += 2;
            continue;
        }
        if (c == '>' && consumeNullRedirection(command, i, word)) {
            word.clear();
            continue;
        }
        if (c == '|' || c == '<' || c == '>') {
            if (c == '|' && i + 1 < command.size() && command[i + 1] == '|') {
                finishCommand();
                tokens.push_back(Token{Token::Type::Or, {}});
                ++i;
                continue;
            }
            if (c == '|') {
                finishCommand();
                tokens.push_back(Token{Token::Type::Pipe, {}});
                continue;
            }
            if (c == '<' && i + 1 < command.size() && command[i + 1] == '<') {
                error = "unsupported shell syntax: heredoc. Use write/append for generated files.";
            } else {
                error = "unsupported shell syntax: redirection is not supported yet";
            }
            return std::nullopt;
        }
        if (c == '&') {
            if (i + 1 < command.size() && command[i + 1] == '&') {
                finishCommand();
                tokens.push_back(Token{Token::Type::And, {}});
                ++i;
                continue;
            }
            // cmd.exe-style unconditional separator (`del x 2>nul & app.exe`).
            // Only a LONE trailing `&` means bash background execution, which
            // stays unsupported.
            size_t j = i + 1;
            while (j < command.size() && std::isspace(static_cast<unsigned char>(command[j]))) ++j;
            if (j >= command.size()) {
                error = "unsupported shell syntax: background execution";
                return std::nullopt;
            }
            finishCommand();
            tokens.push_back(Token{Token::Type::Semicolon, {}});
            continue;
        }
        if (c == ';') {
            finishCommand();
            tokens.push_back(Token{Token::Type::Semicolon, {}});
            continue;
        }
        word.push_back(c);
    }

    if (inSingle || inDouble) {
        error = "unterminated quote";
        return std::nullopt;
    }

    flushWord();
    for (auto& token : tokens) {
        if (token.type == Token::Type::Word) {
            token.text = expandPercentVariables(token.text, expansionState);
        }
    }
    return tokens;
}

std::optional<std::vector<CommandNode>> parse(const std::vector<Token>& tokens, std::string& error) {
    std::vector<CommandNode> commands;
    Connector nextConnector = Connector::Always;
    size_t i = 0;

    while (i < tokens.size()) {
        if (tokens[i].type != Token::Type::Word) {
            error = "empty command near command separator";
            return std::nullopt;
        }

        CommandNode cmd;
        cmd.connector = nextConnector;

        while (i < tokens.size() && tokens[i].type == Token::Type::Word) {
            const std::string& text = tokens[i].text;
            if (cmd.argv.empty() && isEnvAssignment(text)) {
                size_t eq = text.find('=');
                cmd.envAssignments.emplace_back(text.substr(0, eq), text.substr(eq + 1));
            } else {
                cmd.argv.push_back(text);
            }
            ++i;
        }

        if (!cmd.argv.empty() || !cmd.envAssignments.empty()) {
            commands.push_back(std::move(cmd));
        }

        if (i >= tokens.size()) break;
        if (tokens[i].type == Token::Type::Semicolon) nextConnector = Connector::Always;
        else if (tokens[i].type == Token::Type::And) nextConnector = Connector::And;
        else if (tokens[i].type == Token::Type::Or) nextConnector = Connector::Or;
        else if (tokens[i].type == Token::Type::Pipe) nextConnector = Connector::Pipe;
        ++i;
    }

    return commands;
}

SegmentConnector toSegmentConnector(Connector c) {
    switch (c) {
        case Connector::And:  return SegmentConnector::And;
        case Connector::Or:   return SegmentConnector::Or;
        case Connector::Pipe: return SegmentConnector::Pipe;
        case Connector::Always:
        default:              return SegmentConnector::Always;
    }
}

std::string joinArgs(const std::vector<std::string>& args, size_t start = 0) {
    std::string out;
    for (size_t i = start; i < args.size(); ++i) {
        if (i > start) out.push_back(' ');
        out += args[i];
    }
    return out;
}

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

fs::path resolveFromCwd(const fs::path& cwd, const std::string& arg);

bool hasGlobMagic(const std::string& s) {
    return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}

std::vector<fs::path> expandPathArgument(const fs::path& cwd, const std::string& arg) {
    if (!hasGlobMagic(arg)) return {resolveFromCwd(cwd, arg)};

    fs::path patternPath = pathutil::fromUtf8(arg);
    fs::path fullPattern = patternPath.is_absolute()
        ? patternPath.lexically_normal()
        : (cwd / patternPath).lexically_normal();
    fs::path dir = fullPattern.parent_path();
    if (dir.empty()) dir = cwd;

    std::string filePattern = pathutil::toUtf8(fullPattern.filename());
    std::vector<fs::path> matches;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return matches;

    std::regex re;
    try {
        re = std::regex(pathutil::globToRegex(filePattern), std::regex::ECMAScript);
    } catch (...) {
        return matches;
    }

    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         it != end;
         it.increment(ec)) {
        std::string name = pathutil::toUtf8(it->path().filename());
        if (std::regex_match(name, re)) matches.push_back(it->path());
    }
    std::sort(matches.begin(), matches.end());
    return matches;
}

std::vector<std::string> stripCommandParens(std::vector<std::string> args) {
    if (!args.empty() && !args.front().empty() && args.front().front() == '(') {
        args.front().erase(args.front().begin());
    }
    if (!args.empty() && !args.back().empty() && args.back().back() == ')') {
        args.back().pop_back();
    }
    // Bare "(" / ")" tokens become empty after stripping; drop them.
    while (!args.empty() && args.front().empty()) args.erase(args.begin());
    while (!args.empty() && args.back().empty()) args.pop_back();
    return args;
}

// True for single-letter cmd.exe-style switches such as /Q, /s, /Y. These are
// option flags, not paths: a bare "/Q" treated as a path resolves to a
// drive-root directory and trips the sandbox check (seen live as
// `del /Q file` -> "refusing to remove outside project root: Q:/").
bool isSlashSwitch(const std::string& arg) {
    return arg.size() == 2 && arg[0] == '/'
        && std::isalpha(static_cast<unsigned char>(arg[1]));
}

char slashSwitchLetter(const std::string& arg) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(arg[1])));
}

fs::path resolveFromCwd(const fs::path& cwd, const std::string& arg) {
    fs::path p = pathutil::fromUtf8(arg);
    if (p.is_absolute()) return p.lexically_normal();
    return (cwd / p).lexically_normal();
}

// resolveFromCwd + sandbox containment check. Returns an empty path when the
// resolved path escapes the project root. Read/write builtins must treat an
// empty result as a refusal so the shell cannot reach outside ctx.projectRoot.
fs::path resolveContained(const fs::path& cwd, const std::string& arg, const fs::path& root) {
    fs::path p = resolveFromCwd(cwd, arg);
    if (!pathutil::containedIn(p, root)) return {};
    return p;
}

std::string displayPath(const fs::path& path) {
    return pathutil::toUtf8(path);
}

std::string displayRelativePath(const fs::path& path, const fs::path& base) {
    std::error_code ec;
    fs::path rel = fs::relative(path, base, ec);
    return displayPath(ec ? path : rel);
}

bool readWholeFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buf;
    buf << in.rdbuf();
    out = buf.str();
    return true;
}

size_t countFileLines(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    size_t lines = 0;
    bool sawAny = false;
    char ch = '\0';
    while (in.get(ch)) {
        sawAny = true;
        if (ch == '\n') ++lines;
    }
    if (sawAny && ch != '\n') ++lines;
    return lines;
}

void rememberRead(const fs::path& path, ToolContext& ctx) {
    if (!ctx.readSet) return;
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(path, ec);
    ctx.readSet->insert(canonical.empty() ? path : canonical);
}

void snapshotBeforeWrite(const fs::path& path, ToolContext& ctx) {
    if (!ctx.fileBeforeSnapshots) return;
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(path, ec);
    const fs::path& key = canonical.empty() ? path : canonical;
    if (ctx.fileBeforeSnapshots->find(key) != ctx.fileBeforeSnapshots->end()) return;

    std::string oldContent;
    if (fs::exists(path, ec) && fs::is_regular_file(path, ec)) {
        (void)readWholeFile(path, oldContent);
    }
    (*ctx.fileBeforeSnapshots)[key] = std::move(oldContent);
}

// Snapshot every regular file that is about to be destroyed. For a directory
// target this walks the whole tree so a recursive delete stays recoverable by
// the checkpoint/revert mechanism (snapshotBeforeWrite alone only captures the
// top-level node).
void snapshotTreeBeforeWrite(const fs::path& path, ToolContext& ctx) {
    if (!ctx.fileBeforeSnapshots) return;
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        for (fs::recursive_directory_iterator it(path, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec)) snapshotBeforeWrite(it->path(), ctx);
        }
    } else if (fs::is_regular_file(path, ec)) {
        snapshotBeforeWrite(path, ctx);
    }
}

bool removeRecursive(const fs::path& path, bool recursive, std::string& error) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        error = "path not found: " + displayPath(path);
        return false;
    }
    if (fs::is_directory(path, ec) && !recursive) {
        error = "is a directory: " + displayPath(path) + " (pass -r to remove recursively)";
        return false;
    }
    if (recursive) fs::remove_all(path, ec);
    else fs::remove(path, ec);
    if (ec) {
        error = "remove failed: " + ec.message();
        return false;
    }
    return true;
}

#if defined(_WIN32)
// Wrap a spawned process in a Job Object configured to kill the entire tree
// when the job handle closes. This ensures grandchildren (e.g. a build tool's
// sub-processes) are terminated on cancel/timeout, not just the direct child.
struct WindowsJobKiller {
    HANDLE job = nullptr;

    void attach(qint64 pid) {
        if (pid <= 0) return;
        job = CreateJobObjectW(nullptr, nullptr);
        if (!job) return;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        HANDLE ph = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                static_cast<DWORD>(pid));
        if (ph) {
            AssignProcessToJobObject(job, ph);
            CloseHandle(ph);
        }
    }
    // Closing the last handle to the job terminates every process still in it.
    ~WindowsJobKiller() { if (job) CloseHandle(job); }
};
#endif

bool isExecutableFile(const QString& path) {
    QFileInfo info(path);
    return info.exists() && info.isFile();
}

// Case-insensitive base-name check; extensions are stripped first.
bool isBlockedExecutable(const QString& absolutePath) {
    static const QSet<QString> blocked = {
        QStringLiteral("shutdown"),
        QStringLiteral("reboot"),
        QStringLiteral("restart"),
        QStringLiteral("format"),
        QStringLiteral("diskpart"),
        QStringLiteral("regedit"),
        QStringLiteral("reg"),
        QStringLiteral("rundll32"),
        QStringLiteral("takeown"),
        QStringLiteral("icacls"),
        QStringLiteral("net"),
        QStringLiteral("bcdedit"),
        QStringLiteral("bootsect"),
        QStringLiteral("bootrec"),
        QStringLiteral("subst"),
        QStringLiteral("attrib"),
        QStringLiteral("cacls"),
        QStringLiteral("wmic"),
    };
    QFileInfo info(absolutePath);
    const QString base = info.completeBaseName().toLower();
    return blocked.contains(base);
}

// Scan argv for path-like arguments that resolve outside ctx.projectRoot.
// This is advisory only -- external programs are still executed. The warning
// is appended to the output so the LLM can self-correct.
// Does NOT scan flags (arguments starting with -).
std::string checkPathEscapeWarnings(const CommandNode& cmd,
                                     const ShellState& state,
                                     const ToolContext& ctx) {
    std::vector<std::string> warnings;
    for (const std::string& arg : cmd.argv) {
        if (arg.empty() || arg[0] == '-') continue;
        // Skip cmd.exe-style switches (/M, /c, /nologo, /EHsc, ...): a leading
        // slash with no further separator is an option flag, and fromUtf8's
        // drive-alias handling would otherwise resolve e.g. "/M" to "M:/" and
        // raise a bogus outside-project advisory (seen live with `findstr /M`).
        if (arg[0] == '/' && arg.find('/', 1) == std::string::npos
            && arg.find('\\') == std::string::npos) continue;
        // Only inspect arguments that look path-like: contain a slash/backslash,
        // or are an absolute Windows path (e.g. C:foo), or contain "..".
        bool looksLikePath = arg.find('/') != std::string::npos
                          || arg.find('\\') != std::string::npos
                          || arg.find("..") != std::string::npos
                          || (arg.size() >= 2 && std::isalpha(static_cast<unsigned char>(arg[0]))
                              && arg[1] == ':');
        if (!looksLikePath) continue;

        std::error_code ec;
        fs::path resolved = (state.cwd / pathutil::fromUtf8(arg)).lexically_normal();
        if (!pathutil::containedIn(resolved, ctx.projectRoot)) {
            warnings.push_back(
                "  - argument \"" + arg + "\" resolves to \""
                + pathutil::toUtf8(resolved)
                + "\" which is outside the project root");
        }
    }
    if (warnings.empty()) return {};
    std::string msg = "\n[security advisory: the following arguments reference paths "
                      "outside the project directory:\n";
    for (const auto& w : warnings) msg += w + "\n";
    msg += "If this was not intentional, cancel the tool call and retry.]\n";
    return msg;
}

QStringList executableSuffixes(const ShellState& state) {
#if defined(_WIN32)
    return state.env.value(QStringLiteral("PATHEXT"), QStringLiteral(".COM;.EXE;.BAT;.CMD"))
        .split(';', Qt::SkipEmptyParts);
#else
    Q_UNUSED(state);
    return QStringList{QString()};
#endif
}

QString findExecutableForCommand(const std::string& command, const ShellState& state) {
    QString cmd = QString::fromStdString(command);
    auto tryPath = [&](const QString& candidate) -> QString {
        if (isExecutableFile(candidate)) return QFileInfo(candidate).absoluteFilePath();
#if defined(_WIN32)
        for (const QString& suffix : executableSuffixes(state)) {
            if (isExecutableFile(candidate + suffix.toLower())) {
                return QFileInfo(candidate + suffix.toLower()).absoluteFilePath();
            }
            if (isExecutableFile(candidate + suffix.toUpper())) {
                return QFileInfo(candidate + suffix.toUpper()).absoluteFilePath();
            }
        }
#endif
        return {};
    };

    if (cmd.contains('/') || cmd.contains('\\')) {
        fs::path path = resolveFromCwd(state.cwd, command);
        return tryPath(QString::fromUtf8(pathutil::toUtf8(path).c_str()));
    }

    const QString pathValue = state.env.value(QStringLiteral("PATH"));
    const QStringList dirs = pathValue.split(QDir::listSeparator(), Qt::SkipEmptyParts);
    for (const QString& dir : dirs) {
        QString found = tryPath(QDir(dir).filePath(cmd));
        if (!found.isEmpty()) return found;
    }

#if defined(_WIN32)
    QString found = tryPath(QDir(QString::fromUtf8(pathutil::toUtf8(state.cwd).c_str())).filePath(cmd));
    if (!found.isEmpty()) return found;
#endif
    return {};
}

std::string formatListEntry(const fs::directory_entry& entry, const fs::path& base) {
    std::error_code ec;
    std::string type = entry.is_directory(ec) ? "[DIR] " : "[FILE]";
    std::string name = displayPath(fs::relative(entry.path(), base, ec));
    if (ec) name = displayPath(entry.path().filename());
    if (entry.is_regular_file(ec)) {
        return type + " " + std::to_string(entry.file_size(ec)) + " B  " + name + "\n";
    }
    return type + "         " + name + "\n";
}

ExecResult runExternal(const CommandNode& cmd,
                       ShellState state,
                       ToolContext& ctx,
                       int timeoutMs,
                       int maxOutputBytes,
                       const std::string& stdinText = {}) {
    ExecResult result;
    if (cmd.argv.empty()) return result;

    for (const auto& [key, value] : cmd.envAssignments) {
        state.env.insert(QString::fromStdString(key), QString::fromStdString(value));
    }

    QString program = findExecutableForCommand(cmd.argv[0], state);
    if (program.isEmpty()) {
        return {127, false, false, false, {}, "command not found: " + cmd.argv[0]};
    }

    // Block dangerous system executables that have no legitimate use in a
    // coding-agent context but could cause real damage if invoked.
    if (isBlockedExecutable(program)) {
        const QString base = QFileInfo(program).completeBaseName().toLower();
        return {126, false, false, false, {},
            ("blocked by AutoCoder security: '" + base + "' is a dangerous system command "
             "that is not allowed. If you need to modify files, use the dedicated "
             "read/write/edit tools instead.").toStdString()};
    }

    QStringList args;
    for (size_t i = 1; i < cmd.argv.size(); ++i) {
        args << QString::fromStdString(cmd.argv[i]);
    }

    QFileInfo programInfo(program);
#if defined(_WIN32)
    const QString suffix = programInfo.suffix().toLower();
    if (suffix == QStringLiteral("bat") || suffix == QStringLiteral("cmd")) {
        args.prepend(program);
        args.prepend(QStringLiteral("/c"));
        program = QStringLiteral("cmd.exe");
    }

    // Executables that live inside the project root (i.e. the agent's own
    // builds) run under the debug-capture runner: a crash then reports the
    // exception, faulting address and module instead of a bare negative exit
    // code, and OutputDebugString text (e.g. D3D12 debug-layer validation)
    // becomes part of the result. System toolchains keep the plain runner --
    // they already report errors through normal output.
    if (stdinText.empty()) {
        const fs::path programPath = pathutil::fromUtf8(program.toStdString());
        if (pathutil::containedIn(programPath, ctx.projectRoot)) {
            std::vector<std::string> argStrings;
            argStrings.reserve(static_cast<size_t>(args.size()));
            for (const QString& a : args) argStrings.push_back(a.toUtf8().toStdString());
            DebugRunResult dbg = runUnderDebugger(programPath, argStrings, state.cwd,
                                                  state.env, timeoutMs, maxOutputBytes,
                                                  ctx.cancelled, ctx.onOutput);
            if (dbg.launched) {
                result.output = std::move(dbg.output);
                if (dbg.droppedOutputBytes > 0) {
                    result.output += "\n[... " + std::to_string(dbg.droppedOutputBytes)
                                   + " bytes truncated; process output continued to be drained]";
                }
                if (dbg.crashed) {
                    result.output += "\n[process crashed: " + dbg.crashSummary + "]";
                }
                if (!dbg.debugOutput.empty()) {
                    result.output += "\n[debug output (OutputDebugString)]\n" + dbg.debugOutput;
                    if (!result.output.empty() && result.output.back() != '\n') result.output += "\n";
                    if (dbg.droppedDebugBytes > 0) {
                        result.output += "[... " + std::to_string(dbg.droppedDebugBytes)
                                       + " debug-output bytes truncated]\n";
                    }
                }
                result.output += checkPathEscapeWarnings(cmd, state, ctx);
                result.exitCode = dbg.exitCode;
                result.cancelled = dbg.cancelled;
                result.timedOut = dbg.timedOut;
                result.ok = !dbg.crashed && !dbg.cancelled && !dbg.timedOut
                          && dbg.exitCode == 0;
                return result;
            }
            // launched == false: fall through to the QProcess runner below.
        }
    }
#endif

    QProcess proc;
    proc.setProgram(program);
    proc.setArguments(args);
    proc.setWorkingDirectory(QString::fromUtf8(pathutil::toUtf8(state.cwd).c_str()));
    proc.setProcessEnvironment(state.env);
    proc.setProcessChannelMode(QProcess::MergedChannels);

    proc.start();
    if (!proc.waitForStarted(5000)) {
        result.exitCode = 126;
        result.ok = false;
        result.error = "could not start '" + cmd.argv[0] + "': " + proc.errorString().toStdString();
        return result;
    }
#if defined(_WIN32)
    // Kill the whole process tree on cancel/timeout, not just the direct child.
    WindowsJobKiller jobKiller;
    jobKiller.attach(proc.processId());
#endif
    if (!stdinText.empty()) {
        proc.write(stdinText.data(), static_cast<qint64>(stdinText.size()));
    }
    proc.closeWriteChannel();

    QEventLoop loop;
    QByteArray captured;
    qsizetype dropped = 0;
    bool truncationNoticeEmitted = false;

    auto drain = [&]() {
        QByteArray chunk = proc.readAllStandardOutput();
        if (chunk.isEmpty()) return;
        QByteArray visible;
        const qsizetype remaining = maxOutputBytes - captured.size();
        if (remaining > 0) {
            visible = chunk.left(remaining);
            captured += visible;
        }
        if (visible.size() < chunk.size()) dropped += chunk.size() - visible.size();
        if (!visible.isEmpty() && ctx.onOutput) {
            ctx.onOutput(QString::fromUtf8(visible).toStdString());
        }
        if (dropped > 0 && !truncationNoticeEmitted) {
            truncationNoticeEmitted = true;
            if (ctx.onOutput) {
                ctx.onOutput(QStringLiteral(
                    "\n[... output truncated at %1 bytes; process output continues to be drained ...]\n")
                    .arg(maxOutputBytes)
                    .toStdString());
            }
        }
    };

    QObject::connect(&proc, &QProcess::readyReadStandardOutput, &loop, drain);
    QObject::connect(&proc, &QProcess::finished, &loop, &QEventLoop::quit);

    QTimer cancelTimer;
    cancelTimer.setInterval(150);
    QObject::connect(&cancelTimer, &QTimer::timeout, &loop, [&]() {
        if (ctx.cancelled && ctx.cancelled->load()) {
            result.cancelled = true;
            proc.kill();
            loop.quit();
        }
    });
    cancelTimer.start();

    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
        result.timedOut = true;
        proc.kill();
        loop.quit();
    });
    timeoutTimer.start(timeoutMs);

    loop.exec();

    if (proc.state() != QProcess::NotRunning) {
        proc.waitForFinished(2000);
    }
    drain();

    result.output = QString::fromUtf8(captured).toStdString();
    if (dropped > 0) {
        result.output += "\n[... " + std::to_string(dropped)
                       + " bytes truncated; process output continued to be drained]";
    }
    // Append advisory path-escape warning (non-blocking).
    result.output += checkPathEscapeWarnings(cmd, state, ctx);
    result.exitCode = proc.exitCode();
    result.ok = proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
    return result;
}

bool isBuiltin(const std::string& name);

ExecResult runBuiltin(const CommandNode& cmd,
                      ShellState& state,
                      ToolContext& ctx,
                      const std::string& stdinText = {}) {
    const auto& argv = cmd.argv;
    const std::string& name = argv[0];
    std::ostringstream out;

    if (name == "true") return {};
    if (name == "false") return {1, false};

    if (name == "pwd") {
        out << displayPath(state.cwd) << "\n";
        return {0, true, false, false, out.str()};
    }

    if (name == "cd") {
        fs::path target = argv.size() > 1 ? resolveFromCwd(state.cwd, argv[1]) : ctx.projectRoot;
        std::error_code ec;
        if (!fs::is_directory(target, ec)) {
            return {1, false, false, false, {}, "not a directory: " + displayPath(target)};
        }
        // Clamp to project root: reject cd to paths outside ctx.projectRoot.
        if (!pathutil::containedIn(target, ctx.projectRoot)) {
            return {1, false, false, false, {},
                "refusing to cd outside project root: " + displayPath(target)};
        }
        state.cwd = fs::weakly_canonical(target, ec);
        if (ec) state.cwd = target.lexically_normal();
        return {};
    }

    if (name == "echo") {
        out << joinArgs(argv, 1) << "\n";
        return {0, true, false, false, out.str()};
    }

    if (name == "wc") {
        bool countLines = false;
        bool countBytes = false;
        std::vector<std::string> paths;
        for (size_t i = 1; i < argv.size(); ++i) {
            const std::string& a = argv[i];
            if (a.size() > 1 && a[0] == '-'
                && a.find_first_not_of("lc", 1) == std::string::npos) {
                if (a.find('l', 1) != std::string::npos) countLines = true;
                if (a.find('c', 1) != std::string::npos) countBytes = true;
            } else if (!a.empty() && a[0] == '-') {
                return {2, false, false, false, {},
                        "unsupported wc options; supported: wc -l|-c|-lc <files>"};
            } else {
                paths.push_back(a);
            }
        }
        if (!countLines && !countBytes) {
            return {2, false, false, false, {},
                    "unsupported wc options; supported: wc -l|-c|-lc <files>"};
        }

        auto emitCounts = [&](uintmax_t lines, uintmax_t bytes, const std::string& label) {
            bool first = true;
            if (countLines) { out << lines; first = false; }
            if (countBytes) { if (!first) out << " "; out << bytes; }
            if (!label.empty()) out << " " << label;
            out << "\n";
        };

        if (paths.empty()) {
            size_t lines = 0;
            for (char ch : stdinText) if (ch == '\n') ++lines;
            if (!stdinText.empty() && stdinText.back() != '\n') ++lines;
            emitCounts(lines, stdinText.size(), {});
            return {0, true, false, false, out.str()};
        }

        uintmax_t totalLines = 0;
        uintmax_t totalBytes = 0;
        int emitted = 0;
        for (const std::string& arg : paths) {
            std::vector<fs::path> expanded = expandPathArgument(state.cwd, arg);
            for (const fs::path& path : expanded) {
                if (!pathutil::containedIn(path, ctx.projectRoot)) continue;
                std::error_code ec;
                if (!fs::is_regular_file(path, ec)) continue;
                const size_t lines = countLines ? countFileLines(path) : 0;
                uintmax_t bytes = countBytes ? fs::file_size(path, ec) : 0;
                if (ec) bytes = 0;  // file_size returns uintmax_t(-1) on error
                totalLines += lines;
                totalBytes += bytes;
                ++emitted;
                emitCounts(lines, bytes, displayPath(path));
            }
        }
        if (emitted == 0) return {1, false, false, false, {}, "wc: no matching files"};
        if (emitted > 1) emitCounts(totalLines, totalBytes, "total");
        return {0, true, false, false, out.str()};
    }

    if (name == "sort") {
        bool reverse = false;
        bool numeric = false;
        std::vector<std::string> fileArgs;
        for (size_t i = 1; i < argv.size(); ++i) {
            if (argv[i].size() > 1 && argv[i][0] == '-') {
                if (argv[i].find('r') != std::string::npos) reverse = true;
                if (argv[i].find('n') != std::string::npos) numeric = true;
            } else {
                fileArgs.push_back(argv[i]);
            }
        }

        // Read from file operands if given, otherwise from stdin.
        std::string input;
        if (fileArgs.empty()) {
            input = stdinText;
        } else {
            for (const std::string& arg : fileArgs) {
                std::vector<fs::path> expanded = expandPathArgument(state.cwd, arg);
                for (const fs::path& path : expanded) {
                    if (!pathutil::containedIn(path, ctx.projectRoot)) {
                        return {1, false, false, false, {},
                            "refusing to read path outside project root: " + arg};
                    }
                    std::string contents;
                    if (!readWholeFile(path, contents)) {
                        return {1, false, false, false, {},
                            "sort: could not open: " + displayPath(path)};
                    }
                    rememberRead(path, ctx);
                    input += contents;
                    if (!contents.empty() && contents.back() != '\n') input += '\n';
                }
            }
        }

        std::vector<std::string> lines;
        std::istringstream in(input);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);

        auto numericPrefix = [](const std::string& s) {
            size_t i = 0;
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            bool neg = false;
            if (i < s.size() && s[i] == '-') {
                neg = true;
                ++i;
            }
            long long value = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                value = value * 10 + (s[i] - '0');
                ++i;
            }
            return neg ? -value : value;
        };

        std::sort(lines.begin(), lines.end(), [&](const std::string& a, const std::string& b) {
            if (numeric) {
                long long av = numericPrefix(a);
                long long bv = numericPrefix(b);
                if (av != bv) return reverse ? av > bv : av < bv;
            }
            return reverse ? a > b : a < b;
        });
        for (const auto& sortedLine : lines) out << sortedLine << "\n";
        return {0, true, false, false, out.str()};
    }

    if (name == "head") {
        int limit = 10;
        for (size_t i = 1; i < argv.size(); ++i) {
            if (argv[i] == "-n" && i + 1 < argv.size()) {
                try { limit = std::stoi(argv[++i]); } catch (...) { limit = 10; }
            } else if (argv[i].size() > 1 && argv[i][0] == '-'
                       && std::isdigit(static_cast<unsigned char>(argv[i][1]))) {
                try { limit = std::stoi(argv[i].substr(1)); } catch (...) { limit = 10; }
            }
        }
        std::istringstream in(stdinText);
        std::string line;
        int emitted = 0;
        while (emitted < limit && std::getline(in, line)) {
            out << line << "\n";
            ++emitted;
        }
        return {0, true, false, false, out.str()};
    }

    if (name == "tail") {
        int limit = 10;
        for (size_t i = 1; i < argv.size(); ++i) {
            if (argv[i] == "-n" && i + 1 < argv.size()) {
                try { limit = std::stoi(argv[++i]); } catch (...) { limit = 10; }
            } else if (argv[i].size() > 1 && argv[i][0] == '-'
                       && std::isdigit(static_cast<unsigned char>(argv[i][1]))) {
                try { limit = std::stoi(argv[i].substr(1)); } catch (...) { limit = 10; }
            }
        }
        std::istringstream in(stdinText);
        std::vector<std::string> allLines;
        std::string line;
        while (std::getline(in, line)) allLines.push_back(line);
        int start = std::max(0, static_cast<int>(allLines.size()) - limit);
        for (int i = start; i < static_cast<int>(allLines.size()); ++i) {
            out << allLines[i] << "\n";
        }
        return {0, true, false, false, out.str()};
    }

    if (name == "if") {
        // Supported cmd.exe forms:
        //   if exist <path> <command...>
        //   if not exist <path> <command...>
        //   if [not] exist <path> (<command...>) else (<command...>)
        // The taken branch runs like any other command (builtin or external).
        size_t cond = 1;
        bool negate = false;
        if (argv.size() > 1 && lowerAscii(argv[1]) == "not") {
            negate = true;
            cond = 2;
        }
        if (argv.size() < cond + 2 || lowerAscii(argv[cond]) != "exist") {
            return {2, false, false, false, {},
                    "unsupported if syntax; supported: if [not] exist <path> "
                    "<command> [else <command>]"};
        }

        const fs::path probe = resolveFromCwd(state.cwd, argv[cond + 1]);
        std::error_code ec;
        const bool exists = fs::exists(probe, ec);

        const size_t branchStart = cond + 2;
        size_t elseIndex = 0;  // 0 = no else clause
        for (size_t i = branchStart; i < argv.size(); ++i) {
            if (lowerAscii(argv[i]) == "else") {
                elseIndex = i;
                break;
            }
        }

        std::vector<std::string> branch;
        if (exists != negate) {
            branch.assign(argv.begin() + static_cast<ptrdiff_t>(branchStart),
                          elseIndex ? argv.begin() + static_cast<ptrdiff_t>(elseIndex)
                                    : argv.end());
        } else if (elseIndex != 0 && elseIndex + 1 < argv.size()) {
            branch.assign(argv.begin() + static_cast<ptrdiff_t>(elseIndex + 1), argv.end());
        }
        branch = stripCommandParens(std::move(branch));
        if (branch.empty()) return {};  // condition picked an empty branch: no-op

        CommandNode branchCmd;
        branchCmd.argv = std::move(branch);
        if (isBuiltin(branchCmd.argv[0])) return runBuiltin(branchCmd, state, ctx);
        return runExternal(branchCmd, state, ctx, 120000, 200 * 1024);
    }

    if (name == "ls" || name == "dir") {
        bool recursive = false;
        std::vector<fs::path> targets;
        for (size_t i = 1; i < argv.size(); ++i) {
            if (argv[i] == "-r" || argv[i] == "-R") recursive = true;
            else if (argv[i] == "-la" || argv[i] == "-al" || argv[i] == "-l" || argv[i] == "-a") {}
            else if (isSlashSwitch(argv[i])) {
                // cmd.exe switches: /s recurses; /b, /a, /w etc. don't change
                // what this simplified listing prints.
                if (slashSwitchLetter(argv[i]) == 's') recursive = true;
            }
            else {
                std::vector<fs::path> expanded = expandPathArgument(state.cwd, argv[i]);
                if (expanded.empty() && hasGlobMagic(argv[i])) continue;  // no glob match → skip
                for (const fs::path& t : expanded) {
                    if (!pathutil::containedIn(t, ctx.projectRoot)) {
                        return {1, false, false, false, out.str(),
                            "refusing to list path outside project root: " + argv[i]};
                    }
                    targets.push_back(t);
                }
            }
        }
        if (targets.empty()) targets.push_back(state.cwd);
        size_t count = 0;
        for (const auto& target : targets) {
            std::error_code ec;
            if (fs::is_regular_file(target, ec)) {
                // Match the directory-entry format: include the byte size.
                std::error_code ec2;
                uintmax_t bytes = fs::file_size(target, ec2);
                if (ec2) bytes = 0;  // file_size returns uintmax_t(-1) on error
                out << "[FILE] " << bytes << " B  "
                    << displayPath(target) << "\n";
                continue;
            }
            if (!fs::is_directory(target, ec)) {
                return {1, false, false, false, out.str(), "not a directory: " + displayPath(target)};
            }
            if (targets.size() > 1) out << displayPath(target) << ":\n";
            if (recursive) {
                fs::recursive_directory_iterator it(target, fs::directory_options::skip_permission_denied, ec);
                fs::recursive_directory_iterator end;
                while (it != end && count < 500) {
                    if (ctx.cancelled && ctx.cancelled->load()) return {130, false, true, false, out.str()};
                    out << formatListEntry(*it, target);
                    ++count;
                    (void)it.increment(ec);
                }
            } else {
                std::vector<fs::directory_entry> entries;
                for (fs::directory_iterator it(target, fs::directory_options::skip_permission_denied, ec), end;
                     it != end;
                     it.increment(ec)) {
                    entries.push_back(*it);
                }
                std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                    std::error_code ecA;
                    std::error_code ecB;
                    bool aDir = a.is_directory(ecA);
                    bool bDir = b.is_directory(ecB);
                    if (aDir != bDir) return aDir > bDir;
                    return a.path().filename().generic_string() < b.path().filename().generic_string();
                });
                for (const auto& entry : entries) {
                    if (count++ >= 500) break;
                    out << formatListEntry(entry, target);
                }
            }
            if (count >= 500) out << "[... truncated at 500 entries]\n";
        }
        return {0, true, false, false, out.str()};
    }

    if (name == "find_large_files" || name == "large_files") {
        fs::path root = state.cwd;
        uintmax_t minBytes = 50000;
        size_t maxResults = 200;
        bool includeLines = true;
        std::vector<std::string> extensions;

        for (size_t i = 1; i < argv.size(); ++i) {
            const std::string argLower = lowerAscii(argv[i]);
            if ((argLower == "--min-bytes" || argLower == "--min-size") && i + 1 < argv.size()) {
                try { minBytes = static_cast<uintmax_t>(std::stoull(argv[++i])); } catch (...) {}
            } else if (argLower == "--max-results" && i + 1 < argv.size()) {
                try { maxResults = static_cast<size_t>(std::stoull(argv[++i])); } catch (...) {}
            } else if ((argLower == "--ext" || argLower == "--extension") && i + 1 < argv.size()) {
                std::string ext = argv[++i];
                if (!ext.empty() && ext[0] != '.') ext = "." + ext;
                extensions.push_back(lowerAscii(ext));
            } else if (argLower == "--glob" && i + 1 < argv.size()) {
                std::string pat = argv[++i];
                if (pat.size() > 2 && pat[0] == '*' && pat[1] == '.') {
                    extensions.push_back(lowerAscii(pat.substr(1)));
                }
            } else if (argLower == "--no-lines") {
                includeLines = false;
            } else if (argLower == "--lines") {
                includeLines = true;
            } else {
                root = resolveContained(state.cwd, argv[i], ctx.projectRoot);
                if (root.empty()) return {1, false, false, false, {},
                    "refusing to scan path outside project root: " + argv[i]};
            }
        }

        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            return {1, false, false, false, {}, "not a directory: " + displayPath(root)};
        }

        struct LargeFile {
            uintmax_t size = 0;
            fs::path path;
        };
        std::vector<LargeFile> matches;

        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        while (it != end) {
            if (ctx.cancelled && ctx.cancelled->load()) return {130, false, true, false, out.str()};
            const fs::directory_entry& entry = *it;
            std::error_code ec2;
            if (entry.is_directory(ec2)) {
                const std::string dirName = pathutil::toUtf8(entry.path().filename());
                if (pathutil::isExcludedDir(dirName)) it.disable_recursion_pending();
            } else if (entry.is_regular_file(ec2)) {
                const std::string ext = lowerAscii(displayPath(entry.path().extension()));
                bool extOk = extensions.empty()
                    || std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
                uintmax_t size = entry.file_size(ec2);
                if (extOk && !ec2 && size > minBytes) {
                    matches.push_back(LargeFile{size, entry.path()});
                }
            }
            (void)it.increment(ec);
        }

        std::sort(matches.begin(), matches.end(), [](const LargeFile& a, const LargeFile& b) {
            if (a.size != b.size) return a.size > b.size;
            return a.path.generic_string() < b.path.generic_string();
        });

        size_t emitted = 0;
        for (const auto& match : matches) {
            if (emitted++ >= maxResults) {
                out << "[... truncated at " << maxResults << " results]\n";
                break;
            }
            out << match.size << " bytes";
            if (includeLines) {
                out << ", " << countFileLines(match.path) << " lines";
            }
            out << " - " << displayRelativePath(match.path, root) << "\n";
        }
        if (matches.empty()) {
            out << "[no files larger than " << minBytes << " bytes under "
                << displayPath(root) << "]\n";
        }
        return {0, true, false, false, out.str()};
    }

    if (name == "cat" || name == "type") {
        for (size_t i = 1; i < argv.size(); ++i) {
            fs::path path = resolveContained(state.cwd, argv[i], ctx.projectRoot);
            if (path.empty()) return {1, false, false, false, out.str(),
                "refusing to read path outside project root: " + argv[i]};
            std::ifstream in(path, std::ios::binary);
            if (!in) return {1, false, false, false, out.str(), "could not open: " + displayPath(path)};
            out << in.rdbuf();
            rememberRead(path, ctx);
        }
        return {0, true, false, false, out.str()};
    }

    if (name == "mkdir") {
        bool parents = false;
        bool madeAny = false;
        for (size_t i = 1; i < argv.size(); ++i) {
            if (argv[i] == "-p") {
                parents = true;
                continue;
            }
            fs::path path = resolveContained(state.cwd, argv[i], ctx.projectRoot);
            if (path.empty()) return {1, false, false, false, out.str(),
                "refusing to create directory outside project root: " + argv[i]};
            std::error_code ec;
            if (parents) fs::create_directories(path, ec);
            else fs::create_directory(path, ec);
            if (ec) return {1, false, false, false, out.str(), "mkdir failed: " + ec.message()};
            madeAny = true;
        }
        if (!madeAny) return {1, false, false, false, {}, "missing operand"};
        return {};
    }

    if (name == "rm" || name == "del" || name == "erase") {
        bool recursive = false;
        bool force = false;
        std::vector<fs::path> targets;
        for (size_t i = 1; i < argv.size(); ++i) {
            if (argv[i] == "-r" || argv[i] == "-R") recursive = true;
            else if (argv[i] == "-f") force = true;
            else if (argv[i] == "-rf" || argv[i] == "-fr") {
                recursive = true;
                force = true;
            } else if (isSlashSwitch(argv[i])) {
                // cmd.exe switches: /s recurses, /f forces, /q (quiet) maps to
                // force so `del /q missing` does not fail the chain. Others
                // (/p, /a) are ignored.
                const char sw = slashSwitchLetter(argv[i]);
                if (sw == 's') recursive = true;
                else if (sw == 'f' || sw == 'q') force = true;
            } else {
                targets.push_back(resolveFromCwd(state.cwd, argv[i]));
            }
        }
        if (targets.empty()) return {1, false, false, false, {}, "missing operand"};
        for (const auto& target : targets) {
            snapshotTreeBeforeWrite(target, ctx);
            // Check containment for safety.
            if (!pathutil::containedIn(target, ctx.projectRoot)) {
                return {1, false, false, false, out.str(),
                    "refusing to remove outside project root: " + displayPath(target)};
            }
            std::string error;
            bool removed = removeRecursive(target, recursive, error);
            if (!removed) {
                if (force) {
                    // With -f, suppress only "not found" errors; surface others.
                    if (error.find("not found") == std::string::npos) {
                        out << "rm: " << error << "\n";
                    }
                } else {
                    return {1, false, false, false, out.str(), error};
                }
            }
        }
        return {};
    }

    if (name == "rmdir" || name == "rd") {
        bool recursive = false;
        std::vector<std::string> operands;
        for (size_t i = 1; i < argv.size(); ++i) {
            if (isSlashSwitch(argv[i])) {
                // `rmdir /s /q <dir>` is the standard cmd.exe recursive delete.
                if (slashSwitchLetter(argv[i]) == 's') recursive = true;
            } else {
                operands.push_back(argv[i]);
            }
        }
        if (operands.empty()) return {1, false, false, false, {}, "missing operand"};
        for (const std::string& operand : operands) {
            fs::path path = resolveContained(state.cwd, operand, ctx.projectRoot);
            if (path.empty()) return {1, false, false, false, out.str(),
                "refusing to remove path outside project root: " + operand};
            if (recursive) {
                snapshotTreeBeforeWrite(path, ctx);
                std::string error;
                if (!removeRecursive(path, true, error)) {
                    return {1, false, false, false, out.str(), error};
                }
            } else {
                snapshotBeforeWrite(path, ctx);
                std::error_code ec;
                fs::remove(path, ec);
                if (ec) return {1, false, false, false, out.str(), "rmdir failed: " + ec.message()};
            }
        }
        return {};
    }

    if (name == "cp" || name == "copy") {
        // Filter cmd.exe switches (`copy /Y a b`): overwrite-without-prompt is
        // already the behavior, so they only need to not be read as paths.
        std::vector<std::string> operands;
        for (size_t i = 1; i < argv.size(); ++i) {
            if (!isSlashSwitch(argv[i])) operands.push_back(argv[i]);
        }
        if (operands.size() < 2) return {1, false, false, false, {}, "missing source or destination"};
        fs::path dst = resolveFromCwd(state.cwd, operands.back());
        if (!pathutil::containedIn(dst, ctx.projectRoot)) {
            return {1, false, false, false, {},
                "refusing to copy outside project root: " + operands.back()};
        }
        std::error_code ec;
        const bool dstIsDir = fs::is_directory(dst, ec);

        // Collect sources (all operands except the last)
        std::vector<fs::path> sources;
        for (size_t i = 0; i + 1 < operands.size(); ++i) {
            auto expanded = expandPathArgument(state.cwd, operands[i]);
            sources.insert(sources.end(), expanded.begin(), expanded.end());
        }

        for (const auto& src : sources) {
            if (!pathutil::containedIn(src, ctx.projectRoot)) {
                return {1, false, false, false, {},
                    "refusing to copy from outside project root: " + displayPath(src)};
            }
            fs::path actualDst = dstIsDir ? (dst / src.filename()) : dst;
            if (!pathutil::containedIn(actualDst, ctx.projectRoot)) {
                return {1, false, false, false, {},
                    "refusing to copy to outside project root: " + displayPath(actualDst)};
            }
            snapshotBeforeWrite(actualDst, ctx);
            // Clear any read-only attribute on the destination (e.g. a Perforce
            // file) so the overwrite succeeds, matching the write/edit tools.
            pathutil::makeWritable(actualDst, ctx.projectRoot);
            fs::copy_file(src, actualDst, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                return {1, false, false, false, {},
                    "copy failed: " + displayPath(src) + " -> " + displayPath(actualDst) + ": " + ec.message()};
            }
            rememberRead(actualDst, ctx);
        }
        return {};
    }

    if (name == "mv" || name == "move") {
        std::vector<std::string> operands;
        for (size_t i = 1; i < argv.size(); ++i) {
            if (!isSlashSwitch(argv[i])) operands.push_back(argv[i]);  // e.g. move /Y
        }
        if (operands.size() < 2) return {1, false, false, false, {}, "missing source or destination"};
        fs::path dst = resolveFromCwd(state.cwd, operands.back());
        if (!pathutil::containedIn(dst, ctx.projectRoot)) {
            return {1, false, false, false, {},
                "refusing to move outside project root: " + operands.back()};
        }
        std::error_code ec;
        const bool dstIsDir = fs::is_directory(dst, ec);

        std::vector<fs::path> sources;
        for (size_t i = 0; i + 1 < operands.size(); ++i) {
            auto expanded = expandPathArgument(state.cwd, operands[i]);
            sources.insert(sources.end(), expanded.begin(), expanded.end());
        }

        for (const auto& src : sources) {
            if (!pathutil::containedIn(src, ctx.projectRoot)) {
                return {1, false, false, false, {},
                    "refusing to move from outside project root: " + displayPath(src)};
            }
            fs::path actualDst = dstIsDir ? (dst / src.filename()) : dst;
            if (!pathutil::containedIn(actualDst, ctx.projectRoot)) {
                return {1, false, false, false, {},
                    "refusing to move to outside project root: " + displayPath(actualDst)};
            }
            snapshotBeforeWrite(src, ctx);
            snapshotBeforeWrite(actualDst, ctx);
            // Clear read-only on the destination so a move that overwrites an
            // existing (e.g. Perforce) file succeeds.
            pathutil::makeWritable(actualDst, ctx.projectRoot);
            fs::rename(src, actualDst, ec);
            if (ec) {
                return {1, false, false, false, {},
                    "move failed: " + displayPath(src) + " -> " + displayPath(actualDst) + ": " + ec.message()};
            }
            rememberRead(actualDst, ctx);
        }
        return {};
    }

    if (name == "touch") {
        if (argv.size() < 2) return {1, false, false, false, {}, "missing operand"};
        for (size_t i = 1; i < argv.size(); ++i) {
            fs::path path = resolveContained(state.cwd, argv[i], ctx.projectRoot);
            if (path.empty()) return {1, false, false, false, {},
                "refusing to touch path outside project root: " + argv[i]};
            snapshotBeforeWrite(path, ctx);
            fs::path parent = path.parent_path();
            std::error_code ec;
            if (!parent.empty()) fs::create_directories(parent, ec);
            std::ofstream file(path, std::ios::app | std::ios::binary);
            if (!file) return {1, false, false, false, {}, "touch failed: " + displayPath(path)};
            rememberRead(path, ctx);
        }
        return {};
    }

    if (name == "which" || name == "where") {
        if (argv.size() < 2) return {1, false, false, false, {}, "missing command"};
        for (size_t i = 1; i < argv.size(); ++i) {
            QString found = findExecutableForCommand(argv[i], state);
            if (found.isEmpty()) return {1, false, false, false, out.str(), "not found: " + argv[i]};
            out << found.toStdString() << "\n";
        }
        return {0, true, false, false, out.str()};
    }

    return runExternal(cmd, state, ctx, 120000, 200 * 1024);
}

bool isBuiltin(const std::string& name) {
    static constexpr std::string_view builtins[] = {
        "pwd", "cd", "echo", "ls", "dir", "cat", "type", "mkdir", "rm", "del",
        "erase", "rmdir", "rd", "cp", "copy", "mv", "move", "touch", "which",
        "where", "if", "find_large_files", "large_files", "wc", "sort", "head",
        "tail", "true", "false"
    };
    return std::any_of(std::begin(builtins), std::end(builtins),
                       [&](std::string_view b) { return name == b; });
}

std::string rewriteCommonCompatibilityForms(const std::string& command) {
    std::smatch match;
    static const std::regex bashForWc(
        R"((.*?)(for\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\s+(.+?);\s*do\s+wc\s+-l\s+"\$\3"(?:\s+\d?>\s*(?:/dev/null|nul))?\s*;\s*done)(.*))",
        std::regex::icase);
    if (std::regex_match(command, match, bashForWc)) {
        return match[1].str() + "wc -l " + match[4].str() + match[5].str();
    }
    return command;
}

}  // namespace

std::optional<std::vector<ParsedSegment>>
parseForClassification(const std::string& command, std::string& error) {
    ShellState state;  // default cwd (unused by lexing) + system environment.
    auto tokens = lex(command, state, error);
    if (!tokens) return std::nullopt;
    auto commands = parse(*tokens, error);
    if (!commands) return std::nullopt;

    std::vector<ParsedSegment> out;
    out.reserve(commands->size());
    for (const auto& cmd : *commands) {
        ParsedSegment seg;
        seg.connector = toSegmentConnector(cmd.connector);
        seg.argv = cmd.argv;
        out.push_back(std::move(seg));
    }
    return out;
}

ToolResult runInternalShell(const std::string& command,
                            ToolContext& ctx,
                            int timeoutMs,
                            int maxOutputBytes) {
    ShellState state;
    std::error_code ec;
    state.cwd = fs::weakly_canonical(ctx.projectRoot, ec);
    if (ec || state.cwd.empty()) state.cwd = ctx.projectRoot.lexically_normal();

    const std::string rewrittenCommand = rewriteCommonCompatibilityForms(command);

    std::string error;
    auto tokens = lex(rewrittenCommand, state, error);
    if (!tokens) return ToolResult::error(error);

    auto commands = parse(*tokens, error);
    if (!commands) return ToolResult::error(error);

    std::string allOutput;
    int lastExit = 0;
    bool ok = true;
    bool cancelled = false;
    bool timedOut = false;
    size_t droppedOutputBytes = 0;
    bool truncationNoticeEmitted = false;

    auto appendCaptured = [&](const std::string& text, bool stream) {
        if (text.empty()) return;
        const size_t remaining = allOutput.size() < static_cast<size_t>(maxOutputBytes)
            ? static_cast<size_t>(maxOutputBytes) - allOutput.size()
            : 0;
        std::string_view visible(text.data(), std::min(text.size(), remaining));
        if (!visible.empty()) {
            allOutput.append(visible.data(), visible.size());
            if (stream && ctx.onOutput) {
                ctx.onOutput(std::string(visible));
            }
        }
        if (visible.size() < text.size()) {
            droppedOutputBytes += text.size() - visible.size();
            if (!truncationNoticeEmitted) {
                truncationNoticeEmitted = true;
                const std::string notice = "\n[... output truncated at "
                    + std::to_string(maxOutputBytes)
                    + " bytes; process output continues to be drained ...]\n";
                if (stream && ctx.onOutput) ctx.onOutput(notice);
            }
        }
    };

    auto executeCommand = [&](const CommandNode& cmd, const std::string& stdinText) {
        if (isBuiltin(cmd.argv[0])) {
            return runBuiltin(cmd, state, ctx, stdinText);
        }
        return runExternal(cmd, state, ctx, timeoutMs, maxOutputBytes, stdinText);
    };

    // Replace deferred exit-code markers (%ERRORLEVEL%, $?) with the exit
    // code of the previous segment, now that it is known.
    auto resolveExitMarkers = [&](CommandNode node) {
        auto subst = [&](std::string& s) {
            size_t pos;
            while ((pos = s.find(kExitCodeMarker)) != std::string::npos) {
                s.replace(pos, kExitCodeMarker.size(), std::to_string(lastExit));
            }
        };
        for (std::string& a : node.argv) subst(a);
        for (auto& kv : node.envAssignments) subst(kv.second);
        return node;
    };

    for (size_t commandIndex = 0; commandIndex < commands->size(); ++commandIndex) {
        if ((*commands)[commandIndex].connector == Connector::And && lastExit != 0) continue;
        if ((*commands)[commandIndex].connector == Connector::Or && lastExit == 0) continue;
        if ((*commands)[commandIndex].connector == Connector::Pipe) continue;

        const CommandNode cmd = resolveExitMarkers((*commands)[commandIndex]);

        if (cmd.argv.empty()) {
            for (const auto& [key, value] : cmd.envAssignments) {
                state.env.insert(QString::fromStdString(key), QString::fromStdString(value));
            }
            lastExit = 0;
            continue;
        }

        ExecResult result = executeCommand(cmd, {});
        while (commandIndex + 1 < commands->size()
               && (*commands)[commandIndex + 1].connector == Connector::Pipe) {
            ++commandIndex;
            if (result.cancelled || result.timedOut) break;
            const CommandNode pipeCmd = resolveExitMarkers((*commands)[commandIndex]);
            if (pipeCmd.argv.empty()) continue;
            result = executeCommand(pipeCmd, result.output);
        }

        appendCaptured(result.output, isBuiltin(cmd.argv[0]));
        if (!result.error.empty()) {
            if (!allOutput.empty() && allOutput.back() != '\n') appendCaptured("\n", isBuiltin(cmd.argv[0]));
            appendCaptured(result.error + "\n", isBuiltin(cmd.argv[0]));
        }

        lastExit = result.exitCode;
        ok = result.ok;
        cancelled = result.cancelled;
        timedOut = result.timedOut;
        if (cancelled || timedOut) break;
    }

    if (cancelled) {
        return ToolResult::error("[cancelled by user]\npartial output:\n" + allOutput);
    }
    if (timedOut) {
        return ToolResult::error("command timed out after " + std::to_string(timeoutMs)
                                 + "ms; partial output:\n" + allOutput);
    }

    if (droppedOutputBytes > 0) {
        allOutput += "\n[... " + std::to_string(droppedOutputBytes)
                   + " bytes truncated; command output continued to be drained]";
    }

    allOutput += "\n[exit code: " + std::to_string(lastExit) + "]";
    allOutput += "\n[shell: AutoCoder internal shell v1]";

    if (!ok || lastExit != 0) return ToolResult::error(std::move(allOutput));
    return ToolResult::success(std::move(allOutput));
}

}  // namespace autocoder::shell
