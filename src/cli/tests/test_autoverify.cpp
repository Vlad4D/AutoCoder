#include "TestHarness.h"

#include <nlohmann/json.hpp>

#include "agent/AgentRunner.h"
#include "tools/PathUtil.h"

// Auto-verify gate through a real AgentRunner, driven via the test-only
// seams (same approach as the revert suite). The check commands are the
// internal-shell built-ins `true` / `false`, so no external process or
// network is involved. A failing check calls sendToLlm(), which errors out
// harmlessly here (no API key) -- the takeover flag and the appended
// [Auto-verify] feedback message are what we assert on.

using namespace test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {
QString qpath(const fs::path& p) { return QString::fromStdString(pathutil::toUtf8(p)); }

int autoVerifyMessageCount(const json& msgs) {
    int n = 0;
    for (const auto& m : msgs) {
        if (m.value("role", "") != "user") continue;
        if (!m.contains("content") || !m["content"].is_string()) continue;
        if (m["content"].get_ref<const std::string&>().starts_with("[Auto-verify]")) ++n;
    }
    return n;
}
}  // namespace

void suite_autoverify() {
    const fs::path proj = freshDir("autoverify");

    AgentRunner agent;
    agent.setProject(qpath(proj));

    // ---- disabled (no command): never takes over ----
    agent.beginTurnForTest("create a file");
    CHECK(agent.dispatchToolForTest("write", json{{"path", "a.txt"}, {"content", "x"}}).ok);
    CHECK_MSG(!agent.runAutoVerifyForTest(), "no check command -> turn closes normally");
    agent.endTurnForTest();

    // ---- command set, but the turn modified no files: skipped ----
    agent.setCheckCommand(QStringLiteral("false"));
    agent.beginTurnForTest("read-only turn");
    CHECK(agent.dispatchToolForTest("read", json{{"path", "a.txt"}}).ok);
    CHECK_MSG(!agent.runAutoVerifyForTest(), "read-only turn skips the check entirely");
    agent.endTurnForTest();

    // ---- passing check: turn closes normally ----
    agent.setCheckCommand(QStringLiteral("true"));
    agent.beginTurnForTest("edit + passing check");
    CHECK(agent.dispatchToolForTest("edit", json{{"path", "a.txt"},
            {"old_string", "x"}, {"new_string", "y"}}).ok);
    CHECK_MSG(!agent.runAutoVerifyForTest(), "passing check does not take over");
    CHECK(autoVerifyMessageCount(agent.messagesForTest()) == 0);
    agent.endTurnForTest();

    // ---- failing check: takes over twice, then gives up ----
    agent.setCheckCommand(QStringLiteral("false"));
    agent.beginTurnForTest("edit + failing check");
    CHECK(agent.dispatchToolForTest("edit", json{{"path", "a.txt"},
            {"old_string", "y"}, {"new_string", "z"}}).ok);
    CHECK_MSG(agent.runAutoVerifyForTest(), "failing check takes over (attempt 1)");
    CHECK_MSG(agent.runAutoVerifyForTest(), "failing check takes over (attempt 2)");
    CHECK_MSG(!agent.runAutoVerifyForTest(), "attempts are capped at 2 per turn");
    CHECK_MSG(autoVerifyMessageCount(agent.messagesForTest()) == 2,
              "each takeover appended one [Auto-verify] feedback message");
    agent.endTurnForTest();

    // ---- the cap resets on the next turn ----
    agent.beginTurnForTest("next turn, still failing");
    CHECK(agent.dispatchToolForTest("edit", json{{"path", "a.txt"},
            {"old_string", "z"}, {"new_string", "w"}}).ok);
    CHECK_MSG(agent.runAutoVerifyForTest(), "attempt counter is per turn");
    agent.endTurnForTest();

    // ---- unparsable tool-argument diagnostics ----
    // Small blob: a plain parse error with the full raw text echoed back.
    {
        const std::string small = "{\"path\": \"a.txt\", \"content\": ";
        const std::string msg = AgentRunner::describeUnparsableToolArgs("syntax error", small);
        CHECK(msg.find("could not parse tool arguments: syntax error") != std::string::npos);
        CHECK_MSG(msg.find("raw: " + small) != std::string::npos, msg);
        CHECK_MSG(msg.find("truncated at the output-token limit") == std::string::npos,
                  "small blobs are not diagnosed as truncation");
    }
    // Large blob (a `write` cut mid-string at the output-token limit): the
    // model is told to split via write+append, and only a prefix is echoed.
    {
        std::string big = "{\"path\": \"big.cpp\", \"content\": \"";
        big.append(20 * 1024, 'x');  // never closed: truncated mid-string
        const std::string msg = AgentRunner::describeUnparsableToolArgs("unexpected end", big);
        CHECK_MSG(msg.find("truncated at the output-token limit") != std::string::npos, msg);
        CHECK(msg.find("`append`") != std::string::npos);
        CHECK_MSG(msg.size() < 2048, "the broken blob is not echoed back in full");
        CHECK(msg.find("raw (first 500 of " + std::to_string(big.size()) + " bytes):")
              != std::string::npos);
    }
}
