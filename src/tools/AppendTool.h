#pragma once

#include "Tool.h"

class AppendTool : public Tool {
public:
    std::string name() const override { return "append"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
