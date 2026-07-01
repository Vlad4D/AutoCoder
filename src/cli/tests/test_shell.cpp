#include "TestHarness.h"

#include <set>
#include <string>

#include "tools/ToolContext.h"
#include "tools/shell/InternalShell.h"

// Internal-shell built-in diagnostics: wc byte counts, file sizes for
// single-file ls targets, and deferred %ERRORLEVEL% / $? expansion. These
// gaps cost real agent iterations in the DX12 bigtest runs.

using namespace test;
namespace fs = std::filesystem;

void suite_shell() {
    const fs::path root = freshDir("shell");
    std::set<fs::path> readSet;
    ToolContext ctx;
    ctx.projectRoot = root;
    ctx.readSet = &readSet;

    writeFile(root / "data.txt", "one\ntwo\nthree\n");  // 3 lines, 14 bytes

    auto run = [&](const std::string& cmd) {
        return autocoder::shell::runInternalShell(cmd, ctx, 30000, 1 << 20);
    };
    auto has = [](const ToolResult& r, const std::string& needle) {
        return r.content.find(needle) != std::string::npos;
    };

    // ---- wc: -l, -c, -lc ----
    {
        ToolResult r = run("wc -l data.txt");
        CHECK_MSG(r.ok && has(r, "3 "), "wc -l counts lines");
    }
    {
        ToolResult r = run("wc -c data.txt");
        CHECK_MSG(r.ok && has(r, "14 "), "wc -c counts bytes");
    }
    {
        ToolResult r = run("wc -lc data.txt");
        CHECK_MSG(r.ok && has(r, "3 14 "), "wc -lc prints lines then bytes");
    }
    {
        ToolResult r = run("wc -x data.txt");
        CHECK_MSG(!r.ok, "unknown wc flag is rejected");
    }

    // ---- ls on a single file includes the byte size ----
    {
        ToolResult r = run("ls data.txt");
        CHECK_MSG(r.ok && has(r, "[FILE] 14 B"), "ls <file> prints the size");
    }
    {
        ToolResult r = run("ls -l data.txt");
        CHECK_MSG(r.ok && has(r, "[FILE] 14 B"), "ls -l <file> prints the size");
    }

    // ---- %ERRORLEVEL% / $? resolve to the PREVIOUS segment's exit code ----
    {
        ToolResult r = run("false || echo code=%ERRORLEVEL%");
        CHECK_MSG(r.ok && has(r, "code=1"), "%ERRORLEVEL% sees the failed segment");
    }
    {
        ToolResult r = run("true && echo code=%errorlevel%");
        CHECK_MSG(r.ok && has(r, "code=0"), "%errorlevel% is case-insensitive");
    }
    {
        ToolResult r = run("false || echo code=$?");
        CHECK_MSG(r.ok && has(r, "code=1"), "$? sees the failed segment");
    }
    {
        ToolResult r = run("echo lvl=%ERRORLEVEL%");
        CHECK_MSG(r.ok && has(r, "lvl=0"), "exit code starts at 0");
    }

    // ---- cmd.exe-isms that cost agent iterations in the dx12 GUI session ----

    // del /Q: slash switch, not a path on drive Q:
    {
        writeFile(root / "victim.txt", "x");
        ToolResult r = run("del /Q victim.txt");
        CHECK_MSG(r.ok, "del /Q treats /Q as a switch: " + r.content);
        CHECK_MSG(!fs::exists(root / "victim.txt"), "del /Q removed the file");
    }
    // del /q on a missing file maps to force: must not fail the chain
    {
        ToolResult r = run("del /q missing_file.txt && echo proceeded");
        CHECK_MSG(r.ok && has(r, "proceeded"), "del /q missing is quiet: " + r.content);
    }
    // rmdir /s /q removes a tree
    {
        writeFile(root / "junk" / "sub" / "f.txt", "x");
        ToolResult r = run("rmdir /s /q junk");
        CHECK_MSG(r.ok, "rmdir /s /q: " + r.content);
        CHECK_MSG(!fs::exists(root / "junk"), "rmdir /s /q removed the tree");
    }
    // single & is the cmd unconditional separator
    {
        ToolResult r = run("echo first & echo second");
        CHECK_MSG(r.ok && has(r, "first") && has(r, "second"),
                  "& separates commands: " + r.content);
    }
    // & runs the second command even when the first fails (unlike &&)
    {
        ToolResult r = run("del missing.txt 2>nul & echo still_here");
        CHECK_MSG(has(r, "still_here"), "& is unconditional: " + r.content);
    }
    // a LONE trailing & is still background execution: unsupported
    {
        ToolResult r = run("echo hi &");
        CHECK_MSG(!r.ok && has(r, "background execution"),
                  "trailing & stays rejected: " + r.content);
    }
    // one-line if exist with a real command branch
    {
        writeFile(root / "stale.txt", "x");
        ToolResult r = run("if exist stale.txt del stale.txt");
        CHECK_MSG(r.ok, "if exist <file> del <file>: " + r.content);
        CHECK_MSG(!fs::exists(root / "stale.txt"), "if-exist branch executed");
    }
    // if exist on a missing path is a successful no-op (no else required)
    {
        ToolResult r = run("if exist no_such.txt del no_such.txt && echo chained");
        CHECK_MSG(r.ok && has(r, "chained"), "if-exist no-op chains: " + r.content);
    }
    // if not exist
    {
        ToolResult r = run("if not exist no_such.txt echo absent");
        CHECK_MSG(r.ok && has(r, "absent"), "if not exist: " + r.content);
    }
    // parenthesized then/else branches still work
    {
        ToolResult r = run("if exist data.txt (echo yes) else (echo no)");
        CHECK_MSG(r.ok && has(r, "yes") && !has(r, "no"), "if/else: " + r.content);
    }
    // copy /Y: switch filtered, overwrite succeeds
    {
        writeFile(root / "srcfile.txt", "new content");
        writeFile(root / "dstfile.txt", "old content");
        ToolResult r = run("copy /Y srcfile.txt dstfile.txt");
        CHECK_MSG(r.ok, "copy /Y: " + r.content);
        CHECK_MSG(readFile(root / "dstfile.txt") == "new content", "copy /Y overwrote");
    }
#if defined(_WIN32)
    // slash switches on EXTERNAL commands must not trip the path-escape
    // advisory ("/c" is not drive C:)
    {
        ToolResult r = run("cmd /c echo advisory_probe");
        CHECK_MSG(has(r, "advisory_probe"), "cmd /c runs: " + r.content);
        CHECK_MSG(!has(r, "security advisory"),
                  "switch args raise no advisory: " + r.content);
    }
#endif

    // ---- glob expansion in ls/dir ----
    {
        writeFile(root / "foo.txt", "a");
        writeFile(root / "bar.txt", "b");
        ToolResult r = run("dir *.txt");
        CHECK_MSG(r.ok, "dir *.txt glob expansion: " + r.content);
        CHECK_MSG(has(r, "foo.txt") && has(r, "bar.txt"),
                  "dir *.txt lists both .txt files: " + r.content);
    }
    {
        ToolResult r = run("dir *.nonexistent_ext");
        CHECK_MSG(r.ok, "dir with no-glob-match succeeds: " + r.content);
    }
    {
        writeFile(root / ".hidden", "x");
        ToolResult r = run("dir .*");
        CHECK_MSG(r.ok, "dir .* glob match: " + r.content);
        CHECK_MSG(has(r, ".hidden"), "dir .* lists dot-file: " + r.content);
    }
}
