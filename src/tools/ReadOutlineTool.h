#pragma once

#include "Tool.h"

class ReadOutlineTool : public Tool {
public:
    std::string name() const override { return "read_outline"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
