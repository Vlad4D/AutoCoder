#pragma once

#include "Tool.h"

class ReadTool : public Tool {
public:
    std::string name() const override { return "read"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
