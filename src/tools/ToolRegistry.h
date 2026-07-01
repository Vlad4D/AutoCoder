#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "Tool.h"

class ToolRegistry {
public:
    ToolRegistry() = default;
    ToolRegistry(const ToolRegistry&) = delete;
    ToolRegistry& operator=(const ToolRegistry&) = delete;
    ToolRegistry(ToolRegistry&&) = default;
    ToolRegistry& operator=(ToolRegistry&&) = default;

    void registerTool(std::unique_ptr<Tool> tool);

    Tool* find(const std::string& name) const;

    // OpenAI-compatible "tools" array for the request body.
    nlohmann::json toolsArray() const;

    // Dispatch by name; catches exceptions and returns them as error results.
    ToolResult dispatch(const std::string& name, const nlohmann::json& args, ToolContext& ctx) const;

    // Convenience: registry pre-populated with the tools available so far.
    // Step 2: read + glob. Later steps add the rest.
    static ToolRegistry makeDefault();

private:
    std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
    std::vector<std::string> insertionOrder_;
};
