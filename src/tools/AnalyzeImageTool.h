#pragma once

#include "Tool.h"

// Image analysis without vision. The LLM cannot see pixels, so any image it
// generates (render output, screenshots, icons) is otherwise unverifiable --
// in practice agents either trust a file blindly or burn iterations guessing.
// This tool reports: parse validity (with a header hex dump when corrupt),
// dimensions/format, color statistics (distinct colors, uniformity, dominant
// colors, mean), and a coarse ASCII luminance preview so shapes are
// perceivable in plain text.
class AnalyzeImageTool : public Tool {
public:
    std::string name() const override { return "analyze_image"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
