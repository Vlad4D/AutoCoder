#include "TestHarness.h"

#include <string>

#include <QObject>
#include <QString>

#include "agent/AgentRunner.h"
#include "agent/SystemPrompt.h"
#include "tools/PathUtil.h"

// System prompt assembly: project guidance embedding, truncation, and file
// priority. Uses temp directories only -- no network.

using namespace test;

void suite_systemprompt() {
    // ---- guidance file content is embedded verbatim ----
    {
        fs::path root = freshDir("sysprompt_embed");
        writeFile(root / "CLAUDE.md",
                  "# Rules\nAlways use tabs.\nNever touch generated/.\n");
        const std::string prompt = SystemPrompt::build(root);
        CHECK_MSG(prompt.find("Project guidance (CLAUDE.md) -- follow these instructions:")
                      != std::string::npos,
                  "guidance header present");
        CHECK_MSG(prompt.find("Always use tabs.") != std::string::npos,
                  "guidance content embedded");
        CHECK_MSG(prompt.find("Never touch generated/.") != std::string::npos,
                  "full content embedded when under the cap");
        CHECK_MSG(prompt.find("[guidance truncated") == std::string::npos,
                  "no truncation marker for a small file");

        // Byte-stable: unchanged tree -> identical prompt (cache-friendly).
        CHECK_MSG(SystemPrompt::build(root) == prompt,
                  "prompt is byte-identical across rebuilds");
    }

    // ---- no guidance file -> no guidance section ----
    {
        fs::path root = freshDir("sysprompt_none");
        writeFile(root / "main.cpp", "int main() {}\n");
        const std::string prompt = SystemPrompt::build(root);
        CHECK_MSG(prompt.find("Project guidance") == std::string::npos,
                  "no guidance section without a guidance file");
    }

    // ---- priority order: AutoCoder.md wins over CLAUDE.md ----
    {
        fs::path root = freshDir("sysprompt_priority");
        writeFile(root / "AutoCoder.md", "autocoder-guidance-body\n");
        writeFile(root / "CLAUDE.md", "claude-guidance-body\n");
        const std::string prompt = SystemPrompt::build(root);
        CHECK_MSG(prompt.find("autocoder-guidance-body") != std::string::npos,
                  "AutoCoder.md content embedded");
        CHECK_MSG(prompt.find("claude-guidance-body") == std::string::npos,
                  "lower-priority CLAUDE.md ignored");
    }

    // ---- readGuidanceCapped: truncation at a line boundary + marker ----
    {
        fs::path root = freshDir("sysprompt_trunc");
        std::string big;
        for (int i = 0; i < 200; ++i)
            big += "line " + std::to_string(i) + " padding padding padding\n";
        writeFile(root / "CLAUDE.md", big);

        const std::string capped =
            SystemPrompt::readGuidanceCapped(root / "CLAUDE.md", 1024);
        CHECK_MSG(capped.size() < big.size(), "content was cut");
        CHECK_MSG(capped.find("[guidance truncated -- read CLAUDE.md for the rest]")
                      != std::string::npos,
                  "truncation marker names the file");
        // The cut lands on a line boundary: the byte in the source at the cut
        // position must be the newline the cut was aligned to (not mid-line).
        const std::size_t marker = capped.find("\n[guidance truncated");
        CHECK_MSG(marker != std::string::npos
                      && big.compare(0, marker, capped, 0, marker) == 0,
                  "prefix before the marker is an untouched byte-identical prefix");
        CHECK_MSG(marker != std::string::npos && marker < big.size()
                      && big[marker] == '\n',
                  "the cut is aligned to a source line boundary");

        // Full prompt with a small (under-cap) file: content embedded, no marker.
        writeFile(root / "SMALL.md", "just one line\n");
        fs::path smallRoot = freshDir("sysprompt_small");
        writeFile(smallRoot / "CLAUDE.md", "just one line\n");
        const std::string smallPrompt = SystemPrompt::build(smallRoot);
        CHECK_MSG(smallPrompt.find("just one line") != std::string::npos,
                  "under-cap guidance is embedded whole");
        CHECK_MSG(smallPrompt.find("[guidance truncated") == std::string::npos,
                  "under-cap guidance carries no truncation marker");
    }

    // ---- readGuidanceCapped: no-newline cut backs off UTF-8 boundary ----
    {
        fs::path root = freshDir("sysprompt_utf8");
        // A single newline-free line: 300 'a' then 100 3-byte chars (U+20AC).
        std::string euro;  // UTF-8 for U+20AC
        euro += static_cast<char>(0xE2);
        euro += static_cast<char>(0x82);
        euro += static_cast<char>(0xAC);
        std::string body(300, 'a');
        for (int i = 0; i < 100; ++i) body += euro;
        writeFile(root / "AGENTS.md", body);
        // Cap at 350: lands inside a euro sequence; must back off to a boundary.
        const std::string capped =
            SystemPrompt::readGuidanceCapped(root / "AGENTS.md", 350);
        const std::size_t marker = capped.find("\n[guidance truncated");
        CHECK_MSG(marker != std::string::npos, "truncated with a marker");
        const std::string prefix = capped.substr(0, marker);
        // Every euro (3 bytes) in the prefix must be whole: count trailing
        // non-'a' bytes and require the count be a multiple of 3.
        std::size_t nonAscii = 0;
        for (char c : prefix) if (c != 'a') ++nonAscii;
        CHECK_MSG(nonAscii % 3 == 0,
                  "cut never splits a multi-byte UTF-8 code point");
    }

    // ---- CRLF input is normalized to LF ----
    {
        fs::path root = freshDir("sysprompt_crlf");
        writeFile(root / "AGENTS.md", "one\r\ntwo\r\n");
        const std::string content =
            SystemPrompt::readGuidanceCapped(root / "AGENTS.md", 1024);
        CHECK_MSG(content == "one\ntwo\n", "carriage returns stripped");
    }

    // ---- unreadable/missing file -> empty ----
    {
        fs::path root = freshDir("sysprompt_missing");
        CHECK(SystemPrompt::readGuidanceCapped(root / "nope.md", 1024).empty());
    }

    // ---- build signals: emitted only when the prompt is actually (re)built ----
    {
        fs::path root = freshDir("sysprompt_signals");
        writeFile(root / "main.cpp", "int main() { return 0; }\n");

        AgentRunner agent;
        agent.setProject(QString::fromStdString(pathutil::toUtf8(root)));

        int started = 0, finished = 0, lastBytes = 0;
        QObject::connect(&agent, &AgentRunner::systemPromptBuildStarted,
                         [&]() { ++started; });
        QObject::connect(&agent, &AgentRunner::systemPromptBuildFinished,
                         [&](int bytes) { ++finished; lastBytes = bytes; });

        agent.beginTurnForTest(QStringLiteral("first"));
        agent.endTurnForTest();
        CHECK_MSG(started == 1 && finished == 1,
                  "first turn builds the system prompt exactly once");
        CHECK_MSG(lastBytes > 0, "build reports the prompt size");

        agent.beginTurnForTest(QStringLiteral("second"));
        agent.endTurnForTest();
        CHECK_MSG(started == 1 && finished == 1,
                  "later turns reuse the prompt without rebuilding");

        agent.setReasoningGuidance(false);  // flags the prompt stale
        agent.beginTurnForTest(QStringLiteral("third"));
        agent.endTurnForTest();
        CHECK_MSG(started == 2 && finished == 2,
                  "a stale flag triggers exactly one rebuild");
    }
}
