#include "TestHarness.h"

#include <nlohmann/json.hpp>

#include "agent/AgentRunner.h"
#include "persistence/ConversationStore.h"
#include "tools/PathUtil.h"

// Regression for the conversation-switch data-loss bug: streamed assistant text
// is only committed to the conversation in onStreamFinished, so aborting an
// in-flight turn (e.g. switching conversations mid-stream) used to discard the
// reply. For a first turn that left only [system, user] on disk, making the
// whole conversation look emptied on reload. AgentRunner::fail() now salvages
// the partial reply. Driven via test seams (no network).

using namespace test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {
QString qpath(const fs::path& p) { return QString::fromStdString(pathutil::toUtf8(p)); }
}  // namespace

void suite_partial_reply() {
    const fs::path proj = freshDir("partial_reply");
    ConversationStore store;

    // --- A: streamed text before an interruption is preserved on disk ---
    {
        AgentRunner agent;
        agent.setProject(qpath(proj));
        const QString conv = agent.currentConversationIdForTest();
        const std::string convId = conv.toStdString();

        agent.beginTurnForTest("split biggest source file");
        // The model starts streaming a reply...
        agent.feedAssistantTokenForTest("Let me ");
        agent.feedAssistantTokenForTest("read the largest file first.");
        // ...and the user switches conversations, aborting the stream.
        agent.interruptTurnForTest();

        auto saved = store.load(proj, convId);
        const auto& m = saved.messages();
        CHECK_MSG(m.size() == 3,
                  "system + user + salvaged assistant reply are persisted");
        CHECK(m[0].value("role", "") == "system");
        CHECK(m[1].value("role", "") == "user");
        CHECK_MSG(m.back().value("role", "") == "assistant",
                  "the interrupted turn's partial reply is committed");
        CHECK_MSG(m.back().value("content", "")
                      == "Let me read the largest file first.",
                  "the full streamed text (all fragments) is preserved");
    }

    // --- B: an interruption with no streamed text adds no empty assistant msg ---
    {
        AgentRunner agent;
        agent.setProject(qpath(proj));
        const QString conv = agent.currentConversationIdForTest();
        const std::string convId = conv.toStdString();

        agent.beginTurnForTest("what is this project");
        // No tokens streamed before the interruption (e.g. aborted before the
        // first byte, or while a tool was about to run).
        agent.interruptTurnForTest();

        auto saved = store.load(proj, convId);
        const auto& m = saved.messages();
        CHECK_MSG(m.size() == 2,
                  "no partial text => no spurious assistant message");
        CHECK(m[1].value("role", "") == "user");
    }

    // --- C: the salvaged partial does not bleed into the next turn ---
    {
        AgentRunner agent;
        agent.setProject(qpath(proj));
        const QString conv = agent.currentConversationIdForTest();
        const std::string convId = conv.toStdString();

        agent.beginTurnForTest("first prompt");
        agent.feedAssistantTokenForTest("partial one");
        agent.interruptTurnForTest();

        // A fresh turn begins; its (separate) interruption must only carry its
        // own streamed text, not the previous turn's.
        agent.beginTurnForTest("second prompt");
        agent.feedAssistantTokenForTest("partial two");
        agent.interruptTurnForTest();

        auto saved = store.load(proj, convId);
        const auto& m = saved.messages();
        // system, user1, assistant(partial one), user2, assistant(partial two)
        CHECK(m.size() == 5);
        CHECK(m[2].value("role", "") == "assistant");
        CHECK(m[2].value("content", "") == "partial one");
        CHECK(m[4].value("role", "") == "assistant");
        CHECK_MSG(m[4].value("content", "") == "partial two",
                  "each turn salvages only its own streamed text");
    }
}
