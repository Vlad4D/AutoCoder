#include "BashTool.h"

#include "tools/shell/InternalShell.h"

using nlohmann::json;

namespace {

constexpr int kDefaultTimeoutMs = 120000;
constexpr int kMaxOutputBytes = 200 * 1024;

}  // namespace

json BashTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "bash"},
            {"description",
             "Run a command through AutoCoder's internal cross-platform shell. "
             "Use it for builds, tests, project source-control commands, package managers, and quick filesystem "
             "checks. It supports simple commands, common operators, `cd`, env vars, "
             "common built-ins, and direct execution of programs such as p4, cmake, "
             "npm, and python. Avoid complex shell constructs; use "
             "read/write/append/edit/grep/glob for file inspection and edits. "
             "For normal source-file inspection, avoid repr-style escaped dumps "
             "that print literal \\n or \\t noise. "
             "Returns combined stdout+stderr with the exit code."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"command",    {{"type", "string"},  {"description", "Command to run."}}},
                    {"timeout_ms", {{"type", "integer"}, {"description", "Timeout in milliseconds. Default 120000."}}},
                    {"explanation",{{"type", "string"},  {"description",
                        "A short, plain-language explanation of what this command does and why "
                        "you are running it (what you want to achieve). Shown to the user if the "
                        "command needs their approval. Always include it."}}}
                }},
                {"required", json::array({"command"})}
            }}
        }}
    };
}

ToolResult BashTool::execute(const json& args, ToolContext& ctx) {
    if (!args.contains("command")) return ToolResult::error("missing required arg: command");
    const std::string command = args["command"].get<std::string>();
    const int contextDefault = ctx.defaultBashTimeoutMs > 0 ? ctx.defaultBashTimeoutMs
                                                            : kDefaultTimeoutMs;
    const int timeoutMs = args.value("timeout_ms", contextDefault);

    return autocoder::shell::runInternalShell(command, ctx, timeoutMs, kMaxOutputBytes);
}
