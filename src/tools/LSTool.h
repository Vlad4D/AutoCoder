#pragma once

#include "Tool.h"

class LSTool : public Tool {
public:
    std::string name() const override { return "ls"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
