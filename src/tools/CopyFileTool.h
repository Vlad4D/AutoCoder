#pragma once

#include "Tool.h"

// Copy a file from one in-project path to another, byte-for-byte. Avoids routing
// large file contents through the model (which breaks `write` for big files) and
// clears the destination's read-only attribute first. Both paths must be inside
// the project root.
class CopyFileTool : public Tool {
public:
    std::string name() const override { return "copy_file"; }
    nlohmann::json schema() const override;
    ToolResult execute(const nlohmann::json& args, ToolContext& ctx) override;
};
