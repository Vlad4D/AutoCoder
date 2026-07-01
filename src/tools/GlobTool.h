#pragma once

#include "Tool.h"

class GlobTool : public Tool {
public:
    std::string name() const override { return "glob"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
