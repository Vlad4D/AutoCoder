#pragma once

#include "Tool.h"

class WriteTool : public Tool {
public:
    std::string name() const override { return "write"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
