#pragma once

#include "Tool.h"

class AskUserTool : public Tool {
public:
    std::string name() const override { return "ask_user"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
