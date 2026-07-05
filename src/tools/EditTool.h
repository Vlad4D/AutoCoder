#pragma once

#include "Tool.h"

class EditTool : public Tool {
public:
    std::string name() const override { return "edit"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;

    // Diagnostic for a failed match: given the LF-normalised file content and
    // the LF-normalised old_string that was not found, describe the closest
    // matching region (line range, what differs -- trailing whitespace,
    // indentation, or partial line matches -- plus a numbered snippet to copy
    // from) so the model can self-correct in one step instead of retrying
    // blind. Always returns a non-empty, self-contained hint sentence.
    // Pure function; exposed for the offline tests.
    static std::string describeNearestMatch(const std::string& contentLf,
                                            const std::string& oldStringLf);
};
