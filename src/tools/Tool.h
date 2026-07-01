#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "ToolContext.h"

struct ToolResult {
    bool ok = false;
    std::string content;

    static ToolResult success(std::string c) { return { true, std::move(c) }; }
    static ToolResult error(std::string c)   { return { false, std::move(c) }; }
};

class Tool {
public:
    virtual ~Tool() = default;

    // Tool name as exposed to the LLM (must match the function-call name).
    virtual std::string name() const = 0;

    // OpenAI-style tools[] entry: { "type": "function", "function": { name, description, parameters } }.
    virtual nlohmann::json schema() const = 0;

    // Execute with parsed JSON args. Throwing is allowed; the registry catches.
    virtual ToolResult execute(const nlohmann::json& args, ToolContext& ctx) = 0;
};
