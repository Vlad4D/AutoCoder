#pragma once

#include "Tool.h"

class GrepTool : public Tool {
public:
    std::string name() const override { return "grep"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
