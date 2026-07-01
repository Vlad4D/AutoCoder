#pragma once

#include "Tool.h"

class MoveFuncTool : public Tool {
public:
    std::string name() const override { return "move_func"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
