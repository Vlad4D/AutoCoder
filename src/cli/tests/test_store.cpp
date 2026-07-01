#include "TestHarness.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "agent/AgentRunner.h"
#include "agent/Conversation.h"
#include "persistence/ConversationStore.h"
#include "tools/PathUtil.h"

using namespace test;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {
std::string canon(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return pathutil::toUtf8(c.empty() ? p : c);
}
ConversationStore::Checkpoint mkCp(int turn,
        std::vector<std::pair<std::string, std::string>> snaps) {
    ConversationStore::Checkpoint cp;
    cp.turnIndex = turn;
    cp.userMessage = "msg" + std::to_string(turn);
    cp.conversationMessages = json::array();
    cp.fileSnapshots = std::move(snaps);
    return cp;
}
}  // namespace

void suite_store() {
    const fs::path proj = freshDir("store_proj");
    ConversationStore store;
    const std::string conv = "conv-test";
    store.removeCheckpoints(proj, conv);  // clean any AppData state from prior runs

    // ---- Checkpoint snapshot overwrite (regression: Windows atomic-replace) ----
    store.saveCheckpoint(proj, conv, mkCp(0, {}));               // new file
    store.updateCheckpointSnapshots(proj, conv, 0, {{"A", "o1"}});  // update EXISTING file
    {
        auto cp = store.loadCheckpoint(proj, conv, 0);
        CHECK_MSG(cp.fileSnapshots.size() == 1,
                  "updateCheckpointSnapshots persists on an existing checkpoint file");
        if (cp.fileSnapshots.size() == 1) CHECK(cp.fileSnapshots[0].second == "o1");
    }
    store.updateCheckpointSnapshots(proj, conv, 0, {{"A", "o2"}, {"B", "ob"}});  // overwrite again
    CHECK(store.loadCheckpoint(proj, conv, 0).fileSnapshots.size() == 2);

    // ---- base64 round-trip: binary content with NUL bytes + UTF-8 key ----
    {
        std::string binary;
        binary.push_back('\0');
        binary += "\x01\x02\xff";
        binary.push_back('\0');
        binary += "tail";
        const std::string key = "C:/proj/uni_\xC3\xA9.txt";  // valid UTF-8 (é)
        store.updateCheckpointSnapshots(proj, conv, 0, {{key, binary}});
        auto cp = store.loadCheckpoint(proj, conv, 0);
        CHECK(cp.fileSnapshots.size() == 1);
        if (cp.fileSnapshots.size() == 1) {
            CHECK_MSG(cp.fileSnapshots[0].second == binary,
                      "binary content with NUL survives base64 round-trip");
            CHECK(cp.fileSnapshots[0].first == key);
        }
    }

    // ---- listCheckpoints / removeCheckpoint ----
    store.saveCheckpoint(proj, conv, mkCp(1, {}));
    store.saveCheckpoint(proj, conv, mkCp(2, {}));
    {
        CHECK(store.listCheckpoints(proj, conv).size() == 3);  // 0,1,2
        CHECK(store.removeCheckpoint(proj, conv, 2));
        CHECK(store.listCheckpoints(proj, conv).size() == 2);
    }

    // ---- Conversation save / load / overwrite ----
    {
        const std::string id = "conv-rt";
        Conversation c(proj, "test-model");
        c.setSystemPrompt("sys");
        c.addUser("hello");
        c.addAssistant(json{{"role", "assistant"}, {"content", "hi"}});
        CHECK(!store.exists(proj, id));
        store.save(proj, id, c);
        CHECK(store.exists(proj, id));
        CHECK(store.load(proj, id).messages().size() == c.messages().size());

        c.addUser("again");
        store.save(proj, id, c);  // overwrite existing conversation file
        CHECK_MSG(store.load(proj, id).messages().size() == c.messages().size(),
                  "conversation re-save overwrites the existing file");
        store.remove(proj, id);
        CHECK(!store.exists(proj, id));
    }

    // ---- Revert merge + restore (AgentRunner static helpers) ----
    {
        const std::string rconv = "conv-revert";
        store.removeCheckpoints(proj, rconv);

        const fs::path A = proj / "A.txt";            // existing, modified in turn 0
        const fs::path B = proj / "sub" / "B.txt";    // created in turn 1
        writeFile(A, "MODIFIED_A");                   // current (post-edit) on-disk state
        writeFile(B, "NEW_B");

        store.saveCheckpoint(proj, rconv, mkCp(0, {{canon(A), "ORIG_A"}}));  // pre-image of A
        store.saveCheckpoint(proj, rconv, mkCp(1, {{canon(B), ""}}));        // B didn't exist

        // Revert to turn 0 -> undo turns 0 and 1: A restored, B deleted.
        auto merged0 = AgentRunner::collectRevertSnapshots(store, proj, rconv, 0);
        CHECK(merged0.size() == 2);
        CHECK(AgentRunner::applyRevertSnapshots(merged0, proj).empty());
        CHECK_MSG(readFile(A) == "ORIG_A", "revert restores a modified file");
        CHECK_MSG(!fs::exists(B), "revert deletes a file created during the turn");

        // First-insertion-wins: A in both checkpoints; the target turn's pre-image wins.
        writeFile(A, "MODIFIED_A");
        store.removeCheckpoints(proj, rconv);
        store.saveCheckpoint(proj, rconv, mkCp(0, {{canon(A), "ORIG_A"}}));
        store.saveCheckpoint(proj, rconv, mkCp(1, {{canon(A), "MID_A"}}));
        auto merged = AgentRunner::collectRevertSnapshots(store, proj, rconv, 0);
        CHECK(merged.size() == 1);
        AgentRunner::applyRevertSnapshots(merged, proj);
        CHECK_MSG(readFile(A) == "ORIG_A", "target checkpoint pre-image wins on merge");

        // Reverting to the later turn only undoes that turn.
        writeFile(A, "MODIFIED_A");
        auto merged1 = AgentRunner::collectRevertSnapshots(store, proj, rconv, 1);
        CHECK(merged1.size() == 1);
        AgentRunner::applyRevertSnapshots(merged1, proj);
        CHECK_MSG(readFile(A) == "MID_A", "revert to a later turn uses that turn's pre-image");
    }
}
