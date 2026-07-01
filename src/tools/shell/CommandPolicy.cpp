#include "tools/shell/CommandPolicy.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <vector>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QSettings>
#include <QString>
#include <QStringList>

#include "tools/shell/ShellGrammar.h"

namespace autocoder::shell {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Basename of argv[0], lowercased, with a trailing executable extension stripped.
// Mirrors InternalShell's isBlockedExecutable() base-name handling.
std::string programName(const std::string& arg0) {
    size_t slash = arg0.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? arg0 : arg0.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) {
        std::string ext = toLower(base.substr(dot + 1));
        static const std::set<std::string> exeExts =
            {"exe", "bat", "cmd", "com", "ps1", "sh"};
        if (exeExts.count(ext)) base = base.substr(0, dot);
    }
    return toLower(base);
}

// Catastrophic system commands -- never permitted, even with consent.
// Superset of InternalShell::isBlockedExecutable so the gate fails fast.
const std::set<std::string>& hardBlockNames() {
    static const std::set<std::string> s = {
        "shutdown", "reboot", "restart", "halt", "poweroff", "init",
        "format", "diskpart", "mkfs", "fdisk",
        "regedit", "reg", "rundll32", "takeown", "icacls", "cacls",
        "net", "bcdedit", "bootsect", "bootrec", "subst", "attrib", "wmic",
    };
    return s;
}

const std::set<std::string>& rmNames() {
    static const std::set<std::string> s = {"rm", "del", "erase", "rmdir", "rd"};
    return s;
}

const std::set<std::string>& privilegeNames() {
    static const std::set<std::string> s = {"sudo", "doas", "runas", "su"};
    return s;
}

const std::set<std::string>& networkNames() {
    static const std::set<std::string> s = {
        "curl", "wget", "scp", "sftp", "ftp", "nc", "ncat", "telnet", "rsync",
    };
    return s;
}

// Silent allowlist: builds, tests, linters/formatters, read-only inspection.
// NOTE: package managers and git are handled separately (subcommand-aware).
// `cd` is safe by itself: changing directory grants no capability -- every
// command chained after it is classified on its own merits, and allowlisted
// tools already accept absolute paths as arguments anyway. Without it, the
// common LLM habit of prefixing `cd <dir> && cmake ...` forced an approval
// prompt for otherwise-silent builds.
const std::set<std::string>& safeNames() {
    static const std::set<std::string> s = {
        // shell built-ins / read-only inspection
        "cd", "ls", "dir", "pwd", "echo", "cat", "type", "head", "tail", "wc",
        "grep", "findstr", "find", "sort", "which", "where", "true", "false",
        "diff", "cmp", "stat", "file", "tree", "du", "df", "env", "printenv",
        "basename", "dirname", "realpath", "readlink", "date", "whoami", "hostname",
        // build systems
        "cmake", "make", "ninja", "msbuild", "meson", "bazel", "gradle", "mvn",
        // compilers / interpreters used for build+run in-folder
        "gcc", "g++", "clang", "clang++", "cl", "rustc", "javac", "tsc",
        // test runners / language tools (NON-install verbs only via subcommand check)
        "pytest", "ctest", "jest", "vitest", "phpunit", "rspec",
        // linters / formatters
        "clang-format", "clang-tidy", "prettier", "eslint", "ruff", "black",
        "flake8", "pylint", "mypy", "gofmt", "rustfmt", "shellcheck", "cppcheck",
    };
    return s;
}

// git subcommands that only read or make in-folder, checkpoint-recoverable
// changes. Everything else (push, reset, clean, checkout, rebase, pull, ...)
// falls through to Prompt.
const std::set<std::string>& safeGitSubcommands() {
    static const std::set<std::string> s = {
        "status", "diff", "log", "show", "add", "commit", "fetch", "tag",
        "blame", "ls-files", "rev-parse", "describe", "branch", "remote",
        "config", "stash", "shortlog", "whatchanged", "cat-file", "grep",
    };
    return s;
}

// Package managers: install/add verbs fetch + execute remote code (Prompt);
// other verbs (run/test/build/...) are safe (Allow).
bool isPackageManager(const std::string& prog) {
    static const std::set<std::string> pm = {
        "npm", "pnpm", "yarn", "bun", "pip", "pip3", "pipx", "cargo", "gem",
        "apt", "apt-get", "brew", "go", "dotnet", "composer", "nuget", "choco",
        "conda", "poetry",
    };
    return pm.count(prog) > 0;
}

bool isPackageManagerInstall(const std::vector<std::string>& argv) {
    static const std::set<std::string> installVerbs = {
        "install", "add", "i", "ci", "require", "get", "global", "uninstall",
        "remove", "rm", "update", "upgrade",
    };
    for (size_t k = 1; k < argv.size(); ++k) {
        const std::string& a = argv[k];
        if (a.empty() || a[0] == '-') continue;  // skip flags to reach the verb
        return installVerbs.count(toLower(a)) > 0;
    }
    // A bare package manager with no verb (e.g. `npm`) just prints help: safe.
    return false;
}

// Does this argument denote a filesystem/home root (deleting it recursively is
// catastrophic regardless of the project folder)?
bool isDangerousDeleteRoot(const std::string& arg) {
    std::string a = arg;
    // strip a single trailing slash and a trailing /* glob
    if (a.size() >= 2 && (a.substr(a.size() - 2) == "/*" || a.substr(a.size() - 2) == "\\*"))
        a = a.substr(0, a.size() - 2);
    while (a.size() > 1 && (a.back() == '/' || a.back() == '\\')) a.pop_back();

    if (a == "/" || a == "\\" || a == "~" || a.empty()) return true;
    // Windows drive root: "C:", "C:\", "C:/"
    if (a.size() == 2 && std::isalpha(static_cast<unsigned char>(a[0])) && a[1] == ':')
        return true;
    // Critical absolute system dirs.
    static const std::set<std::string> roots = {
        "/etc", "/usr", "/bin", "/sbin", "/var", "/boot", "/lib", "/dev",
        "/system", "/home", "/root", "/users", "/library", "/applications",
    };
    return roots.count(toLower(a)) > 0;
}

struct SegResult {
    Verdict verdict = Verdict::Allow;
    std::string reason;
};

SegResult classifySegment(const std::vector<std::string>& argv) {
    if (argv.empty()) return {};  // env-assignment-only segment: nothing runs.
    const std::string prog = programName(argv[0]);

    if (hardBlockNames().count(prog))
        return {Verdict::HardBlock, "'" + prog + "' is a dangerous system command"};

    if (prog == "dd") {
        for (const std::string& a : argv)
            if (startsWith(a, "of=/dev/"))
                return {Verdict::HardBlock, "dd writing directly to a device"};
    }

    if (rmNames().count(prog)) {
        bool recursive = false, force = false, rootTarget = false;
        for (size_t k = 1; k < argv.size(); ++k) {
            const std::string& a = argv[k];
            if (!a.empty() && a[0] == '-') {
                if (a == "--recursive" || a.find('r') != std::string::npos
                    || a.find('R') != std::string::npos) recursive = true;
                if (a == "--force" || a.find('f') != std::string::npos) force = true;
            } else if (a == "/s" || a == "/S") {
                recursive = true;
            } else if (a == "/q" || a == "/Q") {
                force = true;
            } else if (isDangerousDeleteRoot(a)) {
                rootTarget = true;
            }
        }
        if (rmNames().count(prog) && (prog == "rmdir" || prog == "rd")) recursive = true;
        if (rootTarget)
            return {Verdict::HardBlock,
                    "recursive/forced delete targeting a filesystem or home root"};
        (void)recursive; (void)force;
        return {Verdict::Prompt, "deletes files ('" + prog + "')"};
    }

    if (privilegeNames().count(prog)) {
        // Classify the wrapped command too, so `sudo reboot` is HardBlocked, not
        // merely Prompted. Privilege itself is at least Prompt-worthy.
        const std::vector<std::string> inner(argv.begin() + 1, argv.end());
        SegResult r = classifySegment(inner);
        if (r.verdict == Verdict::HardBlock) return r;
        return {Verdict::Prompt, "runs with elevated privilege ('" + prog + "')"};
    }

    if (networkNames().count(prog))
        return {Verdict::Prompt, "accesses the network ('" + prog + "')"};

    if (isPackageManager(prog)) {
        if (isPackageManagerInstall(argv))
            return {Verdict::Prompt,
                    "installs packages / runs fetched code ('" + prog + "')"};
        return {Verdict::Allow, {}};  // run/test/build/etc.
    }

    if (prog == "git") {
        std::string sub;
        for (size_t k = 1; k < argv.size(); ++k) {
            if (!argv[k].empty() && argv[k][0] == '-') continue;  // skip -C, -c, etc.
            sub = toLower(argv[k]);
            break;
        }
        if (sub.empty()) return {Verdict::Allow, {}};  // bare `git` prints help
        // Force-push / hard reset / clean are extra-destructive but still Prompt.
        if (safeGitSubcommands().count(sub)) return {Verdict::Allow, {}};
        return {Verdict::Prompt, "potentially destructive VCS command ('git " + sub + "')"};
    }

    if (prog == "if") {
        // `if [not] exist <path> <branch> [else <branch>]`: what actually runs
        // is a branch, so classify the branches and keep the worst verdict
        // (`if exist x echo hi` stays silent, `if exist x del x` prompts as a
        // delete). Any other if-form falls through to the default Prompt.
        size_t cond = 1;
        if (argv.size() > 1 && toLower(argv[1]) == "not") cond = 2;
        if (argv.size() >= cond + 2 && toLower(argv[cond]) == "exist") {
            auto stripParens = [](std::vector<std::string> a) {
                if (!a.empty() && !a.front().empty() && a.front().front() == '(')
                    a.front().erase(a.front().begin());
                if (!a.empty() && !a.back().empty() && a.back().back() == ')')
                    a.back().pop_back();
                while (!a.empty() && a.front().empty()) a.erase(a.begin());
                while (!a.empty() && a.back().empty()) a.pop_back();
                return a;
            };
            const std::vector<std::string> rest(
                argv.begin() + static_cast<ptrdiff_t>(cond + 2), argv.end());
            size_t elseIndex = rest.size();
            for (size_t k = 0; k < rest.size(); ++k) {
                if (toLower(rest[k]) == "else") { elseIndex = k; break; }
            }
            std::vector<std::vector<std::string>> branches;
            branches.push_back(stripParens(
                {rest.begin(), rest.begin() + static_cast<ptrdiff_t>(elseIndex)}));
            if (elseIndex < rest.size()) {
                branches.push_back(stripParens(
                    {rest.begin() + static_cast<ptrdiff_t>(elseIndex + 1), rest.end()}));
            }
            SegResult worst{Verdict::Allow, {}};
            for (const auto& branch : branches) {
                if (branch.empty()) continue;
                SegResult r = classifySegment(branch);
                if (static_cast<int>(r.verdict) > static_cast<int>(worst.verdict))
                    worst = r;
            }
            return worst;
        }
    }

    if (safeNames().count(prog)) return {Verdict::Allow, {}};

    // Generic fallback: the program was not recognized as a safe tool, nor
    // classified into a known risk category. It could execute arbitrary code
    // or modify files in unpredictable ways.
    return {Verdict::Prompt,
            "can execute arbitrary code or modify the system ('" + prog + "')"};
}

bool looksLikeForkBomb(const std::string& cmd) {
    return cmd.find(":(){") != std::string::npos
        || cmd.find(":|:&") != std::string::npos
        || cmd.find(":|: &") != std::string::npos;
}

}  // namespace

PolicyResult classifyCommand(const std::string& command,
                             const std::set<std::string>& userAllowlist) {
    // Exact full-command match against the user's always-allow list. Verbatim
    // on the raw string -- never a prefix -- so chaining can't be whitelisted.
    if (userAllowlist.count(command)) return {Verdict::Allow, "user-approved command", {}};

    const std::string trimmed = trim(command);
    if (trimmed.empty()) return {Verdict::Allow, {}, {}};

    if (looksLikeForkBomb(trimmed))
        return {Verdict::HardBlock, "resembles a fork bomb", trimmed};

    std::string error;
    auto segments = parseForClassification(command, error);
    if (!segments) {
        // Unsupported/rejected syntax (substitution, redirection, ...). Fail closed.
        return {Verdict::Prompt,
                error.empty() ? "command could not be parsed" : error, trimmed};
    }

    PolicyResult worst{Verdict::Allow, {}, {}};
    for (const ParsedSegment& seg : *segments) {
        SegResult sr = classifySegment(seg.argv);
        // Combine with ordering HardBlock > Prompt > Allow.
        if (static_cast<int>(sr.verdict) > static_cast<int>(worst.verdict)) {
            worst.verdict = sr.verdict;
            worst.reason = sr.reason;
            worst.offendingSegment = seg.argv.empty() ? trimmed : seg.argv[0];
            if (!seg.argv.empty()) {
                std::string full;
                for (const std::string& a : seg.argv) {
                    if (!full.empty()) full += ' ';
                    full += a;
                }
                worst.offendingSegment = full;
            }
        }
    }
    return worst;
}

// ----- QSettings persistence -----

namespace {
const char* kAllowlistGroup    = "shell/allowlist";
const char* kConsentVersionKey = "shell/consentVersion";
const char* kConsentTimeKey    = "shell/consentTimestamp";
}  // namespace

std::set<std::string> loadUserAllowlist() {
    QSettings s;
    s.beginGroup(QString::fromUtf8(kAllowlistGroup));
    const QStringList keys = s.childKeys();
    std::set<std::string> out;
    for (const QString& k : keys) out.insert(s.value(k).toString().toStdString());
    s.endGroup();
    return out;
}

void addToUserAllowlist(const std::string& exactCommand) {
    const QString q = QString::fromStdString(exactCommand);
    // Hash for the key (commands contain '/' which QSettings treats as a group).
    const QString key = QString::fromUtf8(
        QCryptographicHash::hash(q.toUtf8(), QCryptographicHash::Sha1).toHex());
    QSettings s;
    s.beginGroup(QString::fromUtf8(kAllowlistGroup));
    s.setValue(key, q);
    s.endGroup();
}

bool hasShellConsent() {
    QSettings s;
    return s.contains(QString::fromUtf8(kConsentVersionKey));
}

void recordShellConsent() {
    QSettings s;
    s.setValue(QString::fromUtf8(kConsentVersionKey),
               QCoreApplication::applicationVersion());
    s.setValue(QString::fromUtf8(kConsentTimeKey),
               QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
}

}  // namespace autocoder::shell
