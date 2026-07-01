#pragma once

#include "Tool.h"

class EditTool : public Tool {
public:
    std::string name() const override { return "edit"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
