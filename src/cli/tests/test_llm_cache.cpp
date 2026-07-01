#include "TestHarness.h"

#include <nlohmann/json.hpp>

#include "llm/LlmClient.h"

// Anthropic prompt-caching breakpoints: pure JSON transform on the request
// body, tested without any network.

using namespace test;
using nlohmann::json;

namespace {
bool marked(const json& block) {
    return block.is_object() && block.contains("cache_control")
        && block["cache_control"].value("type", "") == "ephemeral";
}
}  // namespace

void suite_llm_cache() {
    // ---- system string becomes a single cached text block ----
    json body = {
        {"system", "you are a bot"},
        {"messages", json::array({
            json{{"role", "user"},
                 {"content", json::array({ json{{"type", "text"}, {"text", "hi"}} })}},
            json{{"role", "assistant"},
                 {"content", json::array({
                     json{{"type", "text"}, {"text", "calling a tool"}},
                     json{{"type", "tool_use"}, {"id", "t1"}, {"name", "read"},
                          {"input", json::object()}} })}},
            json{{"role", "user"},
                 {"content", json::array({
                     json{{"type", "tool_result"}, {"tool_use_id", "t1"},
                          {"content", "data"}} })}}
        })}
    };
    LlmClient::addClaudeCacheBreakpoints(body);

    CHECK_MSG(body["system"].is_array(), "system string converted to block array");
    CHECK_MSG(marked(body["system"][0]), "system block carries the breakpoint");
    CHECK(body["system"][0].value("text", "") == "you are a bot");

    const json& msgs = body["messages"];
    CHECK_MSG(marked(msgs[2]["content"].back()),
              "last message's final block is a breakpoint (tool_result)");
    CHECK_MSG(marked(msgs[1]["content"].back()),
              "second-to-last message's final block is a breakpoint (tool_use)");
    CHECK_MSG(!marked(msgs[1]["content"][0]),
              "only the final block of a message is marked");
    CHECK_MSG(!marked(msgs[0]["content"].back()),
              "messages before the final two are left unmarked");

    // ---- string-content messages cannot carry markers and are skipped ----
    json body2 = {
        {"messages", json::array({
            json{{"role", "user"},
                 {"content", json::array({ json{{"type", "text"}, {"text", "a"}} })}},
            json{{"role", "user"}, {"content", ""}},
            json{{"role", "user"}, {"content", ""}}
        })}
    };
    LlmClient::addClaudeCacheBreakpoints(body2);
    CHECK_MSG(marked(body2["messages"][0]["content"].back()),
              "marker falls back to the nearest block-array message");

    // ---- degenerate bodies must not throw or invent fields ----
    json body3 = {{"messages", json::array()}};
    LlmClient::addClaudeCacheBreakpoints(body3);
    CHECK(body3["messages"].empty());

    json body4 = json::object();
    LlmClient::addClaudeCacheBreakpoints(body4);
    CHECK(!body4.contains("system"));
    CHECK(!body4.contains("messages"));
}
