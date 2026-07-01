#pragma once

#include "Tool.h"

class MoveSpanTool : public Tool {
public:
    std::string name() const override { return "move_span"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
