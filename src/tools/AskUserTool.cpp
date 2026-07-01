#include "AskUserTool.h"

using nlohmann::json;

json AskUserTool::schema() const {
    json optionsSchema = {
        {"type", "array"},
        {"items", {{"type", "string"}}},
        {"description", "Optional answer choices."}
    };
    return {
        {"type", "function"},
        {"function", {
            {"name", "ask_user"},
            {"description",
             "Ask the user for clarification, confirmation, or information only "
             "they can provide. Preferably include answer choices."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"question", {{"type", "string"}, {"description", "The question to ask the user. Be specific and clear."}}},
                    {"options", optionsSchema}
                }},
                {"required", json::array({"question"})}
            }}
        }}
    };
}

ToolResult AskUserTool::execute(const json& args, ToolContext& ctx) {
    if (!args.contains("question")) {
        return ToolResult::error("missing required arg: question");
    }
    std::string question = args["question"].get<std::string>();

    // Strip leading/trailing blank lines so the UI doesn't show a huge gap
    // between the question text and the input field.
    auto trimTrailingBlankLines = [](std::string& s) {
        // Strip trailing whitespace (spaces, tabs) then blank lines.
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
    };
    auto trimLeadingBlankLines = [](std::string& s) {
        auto it = s.begin();
        while (it != s.end() && (*it == '\n' || *it == '\r'))
            ++it;
        if (it != s.begin())
            s.erase(s.begin(), it);
    };
    trimLeadingBlankLines(question);
    trimTrailingBlankLines(question);

    // Store the question in the context so AgentRunner can pick it up.
    if (ctx.pendingUserQuestion) {
        *ctx.pendingUserQuestion = question;
    }

    // Store any predefined options.
    if (ctx.pendingUserOptions) {
        ctx.pendingUserOptions->clear();
        if (args.contains("options") && args["options"].is_array()) {
            for (const auto& opt : args["options"]) {
                ctx.pendingUserOptions->push_back(opt.get<std::string>());
            }
        }
    }

    return ToolResult::success("[ask_user] Waiting for your response... please answer in the chat.");
}
