#pragma once

#include "Tool.h"

class ReplaceLinesTool : public Tool {
public:
    std::string name() const override { return "replace_lines"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
