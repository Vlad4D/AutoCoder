// Consolidated AutoCoder test runner. Builds as the `autocoder_tests` target.
// No network or UI required. Returns non-zero if any check fails.

#include <cstdio>
#include <exception>

#include <QCoreApplication>

#include "Suites.h"
#include "TestHarness.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("autocoder_tests");
    QCoreApplication::setOrganizationName("AutoCoder");

    struct Suite { const char* name; void (*fn)(); };
    const Suite suites[] = {
        { "paths",      suite_paths },
        { "store",      suite_store },
        { "edit_tools", suite_edit_tools },
        { "policy",     suite_policy },
        { "revert_e2e", suite_revert_e2e },
        { "partial_reply", suite_partial_reply },
        { "highlight",  suite_highlight },
        { "repomap",    suite_repomap },
        { "llm_cache",  suite_llm_cache },
        { "llm_body",   suite_llm_body },
        { "systemprompt", suite_systemprompt },
        { "autoverify", suite_autoverify },
        { "shell",      suite_shell },
        { "image",      suite_image },
        { "debugrunner", suite_debugrunner },
    };

    for (const Suite& s : suites) {
        const int before = test::g_failures;
        std::printf("[%s]\n", s.name);
        try {
            s.fn();
        } catch (const std::exception& e) {
            std::printf("  EXCEPTION: %s\n", e.what());
            ++test::g_failures;
        } catch (...) {
            std::printf("  EXCEPTION: unknown\n");
            ++test::g_failures;
        }
        std::printf("  %s\n", (test::g_failures == before) ? "ok" : "FAILED");
    }

    std::printf("\n%d checks, %d failure(s)\n", test::g_checks, test::g_failures);
    return test::g_failures == 0 ? 0 : 1;
}
