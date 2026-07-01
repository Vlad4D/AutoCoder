#include "ToolRegistry.h"

#include <exception>

#include "AnalyzeImageTool.h"
#include "AppendTool.h"
#include "BashTool.h"
#include "CopyFileTool.h"
#include "EditTool.h"
#include "FindCalleesTool.h"
#include "FindCallersTool.h"
#include "GlobTool.h"
#include "GrepTool.h"
#include "LSTool.h"
#include "MoveFuncTool.h"
#include "MoveSpanTool.h"
#include "AskUserTool.h"
#include "ReadTool.h"
#include "ReadOutlineTool.h"
#include "ReplaceLinesTool.h"
#include "WriteTool.h"

void ToolRegistry::registerTool(std::unique_ptr<Tool> tool) {
    auto name = tool->name();
    insertionOrder_.push_back(name);
    tools_.emplace(std::move(name), std::move(tool));
}

Tool* ToolRegistry::find(const std::string& name) const {
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : it->second.get();
}

nlohmann::json ToolRegistry::toolsArray() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& name : insertionOrder_) {
        arr.push_back(tools_.at(name)->schema());
    }
    return arr;
}

ToolResult ToolRegistry::dispatch(const std::string& name,
                                  const nlohmann::json& args,
                                  ToolContext& ctx) const {
    Tool* tool = find(name);
    if (!tool) return ToolResult::error("unknown tool: " + name);
    try {
        return tool->execute(args, ctx);
    } catch (const std::exception& e) {
        return ToolResult::error(std::string("tool threw: ") + e.what());
    } catch (...) {
        return ToolResult::error("tool threw unknown exception");
    }
}

ToolRegistry ToolRegistry::makeDefault() {
    ToolRegistry r;
    r.registerTool(std::make_unique<BashTool>());
    r.registerTool(std::make_unique<ReadTool>());
    r.registerTool(std::make_unique<WriteTool>());
    r.registerTool(std::make_unique<AppendTool>());
    r.registerTool(std::make_unique<EditTool>());
    r.registerTool(std::make_unique<CopyFileTool>());
    r.registerTool(std::make_unique<GlobTool>());
    r.registerTool(std::make_unique<LSTool>());
    r.registerTool(std::make_unique<GrepTool>());
    r.registerTool(std::make_unique<AnalyzeImageTool>());
    r.registerTool(std::make_unique<ReplaceLinesTool>());
    r.registerTool(std::make_unique<FindCallersTool>());
    r.registerTool(std::make_unique<FindCalleesTool>());
    r.registerTool(std::make_unique<ReadOutlineTool>());
    r.registerTool(std::make_unique<MoveSpanTool>());
    r.registerTool(std::make_unique<MoveFuncTool>());
    r.registerTool(std::make_unique<AskUserTool>());
    return r;
}
