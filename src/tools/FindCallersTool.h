#pragma once

#include "Tool.h"

class FindCallersTool : public Tool {
public:
    std::string name() const override { return "find_callers"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
