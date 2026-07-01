#pragma once

#include <string>

#include "tools/Tool.h"

namespace autocoder::shell {

ToolResult runInternalShell(const std::string& command,
                            ToolContext& ctx,
                            int timeoutMs,
                            int maxOutputBytes);

}  // namespace autocoder::shell
