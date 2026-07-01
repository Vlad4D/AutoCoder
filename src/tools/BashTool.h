#pragma once

#include "Tool.h"

class BashTool : public Tool {
public:
    std::string name() const override { return "bash"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
