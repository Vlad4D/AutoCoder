// Tests for the debug-capture runner (DebugRunner) and its integration into
// the internal shell. Uses the `crashprobe` helper executable built next to
// autocoder_tests. Windows-only: on other platforms the suite is a no-op.

#include "Suites.h"
#include "TestHarness.h"

#if defined(_WIN32)

#include <QCoreApplication>
#include <QProcessEnvironment>

#include "tools/ToolContext.h"
#include "tools/shell/DebugRunner.h"
#include "tools/shell/InternalShell.h"

namespace {

namespace fs = std::filesystem;
using autocoder::shell::DebugRunResult;
using autocoder::shell::runUnderDebugger;

fs::path probePath() {
    return fs::path(QCoreApplication::applicationDirPath().toStdWString())
         / "crashprobe.exe";
}

}  // namespace

void suite_debugrunner() {
    const fs::path probe = probePath();
    CHECK_MSG(fs::exists(probe),
              "crashprobe.exe must be built next to autocoder_tests");
    if (!fs::exists(probe)) return;

    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const fs::path cwd = probe.parent_path();
    const int kTimeout = 20000;
    const int kMaxOut = 64 * 1024;

    // Clean exit: stdout captured, no crash flagged.
    DebugRunResult ok = runUnderDebugger(probe, {"ok"}, cwd, env,
                                         kTimeout, kMaxOut, nullptr, nullptr);
    CHECK(ok.launched);
    CHECK(ok.exitCode == 0);
    CHECK(!ok.crashed);
    CHECK_MSG(ok.output.find("ok") != std::string::npos, ok.output);

    // Nonzero exit code propagates and is not misreported as a crash.
    DebugRunResult e3 = runUnderDebugger(probe, {"exit3"}, cwd, env,
                                         kTimeout, kMaxOut, nullptr, nullptr);
    CHECK(e3.launched);
    CHECK(e3.exitCode == 3);
    CHECK(!e3.crashed);

    // OutputDebugString text is captured even on a successful run.
    DebugRunResult ods = runUnderDebugger(probe, {"ods"}, cwd, env,
                                          kTimeout, kMaxOut, nullptr, nullptr);
    CHECK(ods.launched);
    CHECK(ods.exitCode == 0);
    CHECK_MSG(ods.debugOutput.find("simulated validation message") != std::string::npos,
              ods.debugOutput);
    CHECK(ods.output.find("ods sent") != std::string::npos);

    // Unhandled access violation: exit code, summary, prior stdout and prior
    // OutputDebugString must all be reported.
    DebugRunResult cr = runUnderDebugger(probe, {"crash"}, cwd, env,
                                         kTimeout, kMaxOut, nullptr, nullptr);
    CHECK(cr.launched);
    CHECK(cr.crashed);
    CHECK(cr.exitCode == static_cast<int>(0xC0000005));
    CHECK_MSG(cr.crashSummary.find("access violation") != std::string::npos,
              cr.crashSummary);
    CHECK_MSG(cr.crashSummary.find("writing") != std::string::npos, cr.crashSummary);
    CHECK_MSG(cr.crashSummary.find("crashprobe.exe") != std::string::npos,
              cr.crashSummary);
    CHECK_MSG(cr.debugOutput.find("last words before the crash") != std::string::npos,
              cr.debugOutput);
    CHECK_MSG(cr.output.find("about to crash") != std::string::npos, cr.output);

    // Integration: an executable inside the project root is routed through
    // the debug runner by the internal shell, so a crash shows up in the tool
    // result with the exception summary and the debug output section.
    const fs::path root = test::freshDir("debugrunner_proj");
    std::error_code ec;
    fs::copy_file(probe, root / "crashprobe.exe", ec);
    CHECK_MSG(!ec, ec.message());

    ToolContext ctx;
    ctx.projectRoot = root;

    ToolResult crashed = autocoder::shell::runInternalShell(
        "./crashprobe.exe crash", ctx, kTimeout, kMaxOut);
    CHECK(!crashed.ok);
    CHECK_MSG(crashed.content.find("[process crashed: access violation")
                  != std::string::npos,
              crashed.content);
    CHECK_MSG(crashed.content.find("[debug output (OutputDebugString)]")
                  != std::string::npos,
              crashed.content);
    CHECK(crashed.content.find("about to crash") != std::string::npos);

    // Integration: a clean run through the debug runner still reports exit 0.
    ToolResult clean = autocoder::shell::runInternalShell(
        "./crashprobe.exe ok", ctx, kTimeout, kMaxOut);
    CHECK_MSG(clean.ok, clean.content);
    CHECK(clean.content.find("[exit code: 0]") != std::string::npos);
}

#else  // !_WIN32

void suite_debugrunner() {}

#endif
