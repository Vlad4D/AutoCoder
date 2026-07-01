#pragma once

#include "Tool.h"

class FindCalleesTool : public Tool {
public:
    std::string name() const override { return "find_callees"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
