#include "TestHarness.h"

#include <nlohmann/json.hpp>

#include "llm/LlmClient.h"

// Request-body construction: temperature plumbing and model-aware max_tokens
// clamps. Pure functions of the client configuration -- no network.

using namespace test;
using nlohmann::json;

namespace {

json userMessages() {
    return json::array({ json{{"role", "user"}, {"content", "hi"}} });
}

json readTool() {
    return json::array({ json{
        {"type", "function"},
        {"function", {
            {"name", "read"},
            {"description", "read a file"},
            {"parameters", json::object()}
        }}
    }});
}

}  // namespace

void suite_llm_body() {
    // ---- defaults: temperature omitted, deepseek-chat output cap applied ----
    {
        LlmClient c;
        json body = c.buildOpenAiBody(userMessages(), readTool(), true);
        CHECK_MSG(!body.contains("temperature"),
                  "temperature unset by default -> field omitted");
        CHECK_MSG(body.value("max_tokens", 0) == 8192,
                  "default 16384 clamps to deepseek-chat's 8K output cap");
        CHECK(body.value("model", "") == "deepseek-chat");
        CHECK(body.value("stream", false) == true);
        CHECK_MSG(body["stream_options"].value("include_usage", false),
                  "stream_options preserved");
        CHECK(body.value("tool_choice", "") == "auto");
        CHECK(body["tools"].is_array() && body["tools"].size() == 1);
        CHECK(body["messages"].is_array() && body["messages"].size() == 1);
    }

    // ---- temperature set -> field present; clamped to [0, 2] ----
    {
        LlmClient c;
        c.setTemperature(0.0);
        json body = c.buildOpenAiBody(userMessages(), {}, false);
        CHECK_MSG(body.contains("temperature"), "temperature 0.0 is sent");
        CHECK(body["temperature"].get<double>() == 0.0);
        CHECK_MSG(!body.contains("stream_options"),
                  "non-streaming body has no stream_options");

        c.setTemperature(5.0);
        body = c.buildOpenAiBody(userMessages(), {}, false);
        CHECK_MSG(body["temperature"].get<double>() == 2.0,
                  "temperature clamps to OpenAI-style max 2.0");
    }

    // ---- negative sentinel unsets it again ----
    {
        LlmClient c;
        c.setTemperature(0.3);
        c.setTemperature(-1.0);
        json body = c.buildOpenAiBody(userMessages(), {}, false);
        CHECK_MSG(!body.contains("temperature"),
                  "negative temperature = provider default (omitted)");
    }

    // ---- deepseek-reasoner: sampling params suppressed, 64K output cap ----
    {
        LlmClient c;
        c.setModel("deepseek-reasoner");
        c.setTemperature(0.0);
        json body = c.buildOpenAiBody(userMessages(), {}, true);
        CHECK_MSG(!body.contains("temperature"),
                  "reasoner models reject sampling params -> omitted");
        CHECK_MSG(body.value("max_tokens", 0) == 16384,
                  "16384 fits under the reasoner's 64K cap");
        c.setMaxTokens(100000);
        body = c.buildOpenAiBody(userMessages(), {}, true);
        CHECK_MSG(body.value("max_tokens", 0) == 65536,
                  "reasoner output clamps to 64K");
    }

    // ---- unknown OpenAI-compatible model: generic 128K cap ----
    {
        LlmClient c;
        c.setModel("some-custom-model");
        c.setMaxTokens(200000);
        json body = c.buildOpenAiBody(userMessages(), {}, false);
        CHECK_MSG(body.value("max_tokens", 0) == 128000,
                  "unknown models keep the generic 128K clamp");
        c.setTemperature(0.7);
        body = c.buildOpenAiBody(userMessages(), {}, false);
        CHECK_MSG(body["temperature"].get<double>() == 0.7,
                  "non-reasoner custom model still gets temperature");
    }

    // ---- Claude body: temperature present + clamped to [0, 1] ----
    {
        LlmClient c;
        c.setProvider("claude");
        c.setModel("claude-sonnet-4-6");
        json body = c.buildClaudeBody(userMessages(), readTool(), true);
        CHECK_MSG(!body.contains("temperature"),
                  "Claude default: temperature omitted");
        CHECK(body.value("max_tokens", 0) == 16384);

        c.setTemperature(0.7);
        body = c.buildClaudeBody(userMessages(), readTool(), true);
        CHECK(body["temperature"].get<double>() == 0.7);

        c.setTemperature(1.5);
        body = c.buildClaudeBody(userMessages(), readTool(), true);
        CHECK_MSG(body["temperature"].get<double>() == 1.0,
                  "temperature clamps to Anthropic max 1.0");

        // Anthropic tool schema conversion is unaffected.
        CHECK(body["tools"].is_array());
        CHECK(body["tools"][0].value("name", "") == "read");
        CHECK(body["tools"][0].contains("input_schema"));
    }

    // ---- Claude model-aware max_tokens clamp ----
    {
        LlmClient c;
        c.setProvider("claude");
        c.setMaxTokens(60000);

        c.setModel("claude-3-5-sonnet-latest");
        CHECK_MSG(c.buildClaudeBody(userMessages(), {}, false).value("max_tokens", 0) == 8192,
                  "claude-3.5 output clamps to 8K");

        c.setModel("claude-3-opus-20240229");
        CHECK_MSG(c.buildClaudeBody(userMessages(), {}, false).value("max_tokens", 0) == 4096,
                  "claude-3 opus output clamps to 4K");

        c.setModel("claude-sonnet-4-6");
        CHECK_MSG(c.buildClaudeBody(userMessages(), {}, false).value("max_tokens", 0) == 60000,
                  "claude 4.x keeps a large request under its 64K cap");
    }
}
