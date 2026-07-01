cmake_minimum_required(VERSION 3.21)
project(AutoCoder VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

find_package(Qt6 6.5 COMPONENTS Core Gui Network Widgets REQUIRED)

qt_standard_project_setup()

# ----- Vendored header-only deps -----
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party
)

# ----- Core library: tools + llm + agent + persistence (no UI) -----
set(AUTOCODER_CORE_SOURCES
    src/tools/Tool.h
    src/tools/ToolContext.h
    src/tools/ToolRegistry.h
    src/tools/ToolRegistry.cpp
    src/tools/CodeStructure.h
    src/tools/CodeStructure.cpp
    src/tools/PathUtil.h
    src/tools/PathUtil.cpp
    src/tools/IgnoreRules.h
    src/tools/IgnoreRules.cpp
    src/tools/ReadTool.h
    src/tools/ReadTool.cpp
    src/tools/WriteTool.h
    src/tools/WriteTool.cpp
    src/tools/AppendTool.h
    src/tools/AppendTool.cpp
    src/tools/EditTool.h
    src/tools/EditTool.cpp
    src/tools/CopyFileTool.h
    src/tools/CopyFileTool.cpp
    src/tools/GlobTool.h
    src/tools/GlobTool.cpp
    src/tools/GrepTool.h
    src/tools/GrepTool.cpp
    src/tools/AnalyzeImageTool.h
    src/tools/AnalyzeImageTool.cpp
    src/tools/BashTool.h
    src/tools/BashTool.cpp
    src/tools/shell/InternalShell.h
    src/tools/shell/InternalShell.cpp
    src/tools/shell/DebugRunner.h
    src/tools/shell/DebugRunner.cpp
    src/tools/shell/ShellGrammar.h
    src/tools/shell/CommandPolicy.h
    src/tools/shell/CommandPolicy.cpp
    src/tools/ReplaceLinesTool.h
    src/tools/ReplaceLinesTool.cpp
    src/tools/FindCallersTool.h
    src/tools/FindCallersTool.cpp
    src/tools/FindCalleesTool.h
    src/tools/FindCalleesTool.cpp
    src/tools/ReadOutlineTool.h
    src/tools/ReadOutlineTool.cpp
    src/tools/MoveSpanTool.h
    src/tools/MoveSpanTool.cpp
    src/tools/MoveFuncTool.h
    src/tools/MoveFuncTool.cpp
    src/tools/LSTool.h
    src/tools/LSTool.cpp
    src/tools/AskUserTool.h
    src/tools/AskUserTool.cpp

    src/diagnostics/CrashHandler.h
    src/diagnostics/CrashHandler.cpp
    src/diagnostics/TraceTimer.h
    src/diagnostics/TraceTimer.cpp

    src/llm/LlmClient.h
    src/llm/LlmClient.cpp

    src/agent/Conversation.h
    src/agent/Conversation.cpp
    src/agent/SystemPrompt.h
    src/agent/SystemPrompt.cpp
    src/agent/RepoMap.h
    src/agent/RepoMap.cpp
    src/agent/AgentRunner.h
    src/agent/AgentRunner.cpp

    src/persistence/ConversationStore.h
    src/persistence/ConversationStore.cpp
    src/persistence/SecretStore.h
    src/persistence/SecretStore.cpp
)

qt_add_library(autocoder_core STATIC
    ${AUTOCODER_CORE_SOURCES}
)
target_include_directories(autocoder_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(autocoder_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    nlohmann_json
)
if(WIN32)
    target_link_libraries(autocoder_core PUBLIC DbgHelp Advapi32)
endif()

# ----- GUI executable -----
qt_add_executable(AutoCoder WIN32
    src/build_date.cpp
    src/main.cpp
    resources/autocoder.rc
    resources/autocoder.qrc
    src/ui/AskUserWidget.h
    src/ui/AskUserWidget.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/ChatView.h
    src/ui/ChatView.cpp
    src/ui/MessageWidget.h
    src/ui/MessageWidget.cpp
    src/ui/ImagePreview.h
    src/ui/ImagePreview.cpp
    src/ui/ToolCallWidget.h
    src/ui/ToolCallWidget.cpp
    src/ui/CodeColorizer.h
    src/ui/CodeColorizer.cpp
    src/ui/InputBar.h
    src/ui/InputBar.cpp
    src/ui/SettingsDialog.h
    src/ui/SettingsDialog.cpp
)
target_link_libraries(AutoCoder PRIVATE
    autocoder_core
    Qt6::Widgets
)

# Let the .rc file find its sibling .ico (in resources/)
set_source_files_properties("resources/autocoder.rc" PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
)
if(WIN32)
  # By default MSVC looks for .rc includes relative to the .rc file itself,
  # so autocoder.ico in the same resources/ directory should be found.
  # No extra action needed.
endif()

# Touch build_date.cpp before every build so __DATE__/__TIME__ are always fresh.
add_custom_command(TARGET AutoCoder PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E touch_nocreate ${CMAKE_SOURCE_DIR}/src/build_date.cpp
    COMMENT "Touching build_date.cpp to refresh __DATE__/__TIME__"
)

# ----- Temporary CLI test executable -----
qt_add_executable(autocoder_cli
    src/cli/cli_main.cpp
)
target_link_libraries(autocoder_cli PRIVATE
    autocoder_core
)

# ----- Offline unit tests (no API key / network / UI needed) -----
qt_add_executable(autocoder_tests
    src/cli/tests/test_main.cpp
    src/cli/tests/TestHarness.h
    src/cli/tests/Suites.h
    src/cli/tests/test_paths.cpp
    src/cli/tests/test_store.cpp
    src/cli/tests/test_edit_tools.cpp
    src/cli/tests/test_policy.cpp
    src/cli/tests/test_revert_e2e.cpp
    src/cli/tests/test_partial_reply.cpp
    src/cli/tests/test_highlight.cpp
    src/cli/tests/test_repomap.cpp
    src/cli/tests/test_llm_cache.cpp
    src/cli/tests/test_autoverify.cpp
    src/cli/tests/test_shell.cpp
    src/cli/tests/test_image.cpp
    src/cli/tests/test_debugrunner.cpp
    src/ui/CodeColorizer.h
    src/ui/CodeColorizer.cpp
)
target_link_libraries(autocoder_tests PRIVATE
    autocoder_core
)

# Crash-probe helper used by the debug-runner tests; built into the same
# runtime directory as autocoder_tests so the suite can find it by name.
add_executable(crashprobe src/cli/tests/helpers/crashprobe.cpp)
add_dependencies(autocoder_tests crashprobe)

if(WIN32)
    set_target_properties(AutoCoder PROPERTIES WIN32_EXECUTABLE TRUE)

    if(TARGET Qt6::windeployqt)
        add_custom_command(TARGET AutoCoder POST_BUILD
            COMMAND Qt6::windeployqt
                    --no-translations
                    --no-compiler-runtime
                    "$<TARGET_FILE:AutoCoder>"
            COMMENT "Running windeployqt for AutoCoder"
        )
        # CLI shares the GUI's Debug/ output dir, so its DLLs are already deployed
        # by the AutoCoder windeployqt step. Nothing extra to do for autocoder_cli.
    endif()
endif()
