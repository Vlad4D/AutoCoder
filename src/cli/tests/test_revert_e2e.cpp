#include "TestHarness.h"

#include <nlohmann/json.hpp>

#include "agent/AgentRunner.h"
#include "persistence/ConversationStore.h"
#include "tools/PathUtil.h"

// End-to-end revert through a real AgentRunner: take a checkpoint, run real
// tools that modify files, finish the turn (flushing snapshots into the
// checkpoint), then revertToCheckpoint -- the exact path that was broken. No
// network: turns are driven via the test-only seams.

using namespace test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {
QString qpath(const fs::path& p) { return QString::fromStdString(pathutil::toUtf8(p)); }
}  // namespace

void suite_revert_e2e() {
    const fs::path proj = freshDir("revert_e2e");

    // --- Regression: an early cancel after the first prompt must not leave
    // only an orphan checkpoint with no conversation JSON.
    {
        AgentRunner agent;
        agent.setProject(qpath(proj));
        const QString conv = agent.currentConversationIdForTest();
        CHECK(!conv.isEmpty());

        const int t0 = agent.beginTurnForTest("create dx12 demo");
        CHECK(t0 == 0);

        ConversationStore store;
        const std::string convId = conv.toStdString();
        CHECK_MSG(store.exists(proj, convId),
                  "fresh user turn is persisted before assistant/network work");
        auto saved = store.load(proj, convId);
        CHECK(saved.messages().size() >= 2);
        CHECK(saved.messages()[0].value("role", "") == "system");
        CHECK(saved.messages()[1].value("role", "") == "user");
        CHECK(saved.messages()[1].value("content", "") == "create dx12 demo");
        CHECK(store.listCheckpoints(proj, convId).size() == 1);

        agent.cancel();
    }

    // --- A: modify an existing file, then revert that (most-recent) turn ---
    {
        AgentRunner agent;
        agent.setProject(qpath(proj));
        const QString conv = agent.currentConversationIdForTest();
        CHECK(!conv.isEmpty());

        writeFile(proj / "f.txt", "ORIG");
        const int t0 = agent.beginTurnForTest("modify f.txt");
        CHECK(agent.dispatchToolForTest("read", json{{"path", "f.txt"}}).ok);
        CHECK(agent.dispatchToolForTest("edit", json{{"path", "f.txt"},
                {"old_string", "ORIG"}, {"new_string", "MODIFIED"}}).ok);
        agent.endTurnForTest();
        CHECK_MSG(readFile(proj / "f.txt") == "MODIFIED", "edit applied during the turn");

        agent.revertToCheckpoint(conv, t0);
        CHECK_MSG(readFile(proj / "f.txt") == "ORIG",
                  "reverting the most-recent turn restores the modified file");
    }

    // --- B: create a new file in a turn, then revert -> file deleted ---
    {
        AgentRunner agent;
        agent.setProject(qpath(proj));
        const QString conv = agent.currentConversationIdForTest();

        const int t0 = agent.beginTurnForTest("create g.txt");
        CHECK(agent.dispatchToolForTest("write", json{{"path", "g.txt"},
                {"content", "NEW"}}).ok);
        agent.endTurnForTest();
        CHECK(fs::exists(proj / "g.txt"));

        agent.revertToCheckpoint(conv, t0);
        CHECK_MSG(!fs::exists(proj / "g.txt"),
                  "reverting deletes a file created during the turn");
    }

    // --- C: two turns edit the same file; reverting the earlier turn undoes both ---
    {
        AgentRunner agent;
        agent.setProject(qpath(proj));
        const QString conv = agent.currentConversationIdForTest();

        writeFile(proj / "h.txt", "H0");
        const int t0 = agent.beginTurnForTest("edit h -> H1");
        CHECK(agent.dispatchToolForTest("read", json{{"path", "h.txt"}}).ok);
        CHECK(agent.dispatchToolForTest("edit", json{{"path", "h.txt"},
                {"old_string", "H0"}, {"new_string", "H1"}}).ok);
        agent.endTurnForTest();

        const int t1 = agent.beginTurnForTest("edit h -> H2");
        CHECK(agent.dispatchToolForTest("edit", json{{"path", "h.txt"},
                {"old_string", "H1"}, {"new_string", "H2"}}).ok);
        agent.endTurnForTest();

        CHECK(readFile(proj / "h.txt") == "H2");
        CHECK(t1 == t0 + 1);

        agent.revertToCheckpoint(conv, t0);
        CHECK_MSG(readFile(proj / "h.txt") == "H0",
                  "reverting the earlier turn undoes both turns");
    }

    // --- D: a turn that changes NO files must not revert a prior turn's change ---
    {
        AgentRunner agent;
        agent.setProject(qpath(proj));
        const QString conv = agent.currentConversationIdForTest();

        writeFile(proj / "k.txt", "K0");
        const int t0 = agent.beginTurnForTest("edit k -> K1");
        CHECK(agent.dispatchToolForTest("read", json{{"path", "k.txt"}}).ok);
        CHECK(agent.dispatchToolForTest("edit", json{{"path", "k.txt"},
                {"old_string", "K0"}, {"new_string", "K1"}}).ok);
        agent.endTurnForTest();

        const int t1 = agent.beginTurnForTest("no file changes");  // read-only turn
        CHECK(agent.dispatchToolForTest("read", json{{"path", "k.txt"}}).ok);
        agent.endTurnForTest();

        agent.revertToCheckpoint(conv, t1);
        CHECK_MSG(readFile(proj / "k.txt") == "K1",
                  "reverting a no-op turn leaves the prior turn's change intact");
    }
}
