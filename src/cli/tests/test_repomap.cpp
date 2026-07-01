#include "TestHarness.h"

#include <string>

#include "agent/RepoMap.h"

using namespace test;
namespace fs = std::filesystem;

void suite_repomap() {
    const fs::path root = freshDir("repomap");

    writeFile(root / "src" / "math.cpp",
              "int add(int a, int b) {\n    return a + b;\n}\n"
              "int sub(int a, int b) {\n    return a - b;\n}\n");
    writeFile(root / "src" / "util.h",
              "#pragma once\nclass Helper {\npublic:\n    void run();\n};\n");
    writeFile(root / "README.md", "# demo\n");
    writeFile(root / "secret.pem", "KEY MATERIAL\n");
    writeFile(root / ".autocoderignore", "hidden.txt\n");
    writeFile(root / "hidden.txt", "do not list\n");

    const std::string map = repomap::build(root);

    CHECK_MSG(!map.empty(), "repo map is generated for a non-empty project");
    CHECK_MSG(map.find("src/math.cpp") != std::string::npos,
              "map lists files with forward-slash relative paths");
    CHECK_MSG(map.find("README.md") != std::string::npos,
              "non-source files are listed");
    CHECK_MSG(map.find("secret.pem") == std::string::npos,
              "built-in secret defaults are excluded");
    CHECK_MSG(map.find("hidden.txt") == std::string::npos,
              ".autocoderignore entries are excluded");

    // Tight budget: map degrades to the header + truncation marker instead of
    // blowing past the cap. With only file names (no symbols), the map is tiny,
    // so use a very small budget.
    const std::string tiny = repomap::build(root, 60);
    CHECK_MSG(tiny.size() < map.size(), "tiny budget produces a smaller map");
    CHECK_MSG(tiny.find("[project map truncated]") != std::string::npos,
              "truncation is marked, not silent");

    // Nonexistent root: empty result, no throw.
    CHECK(repomap::build(root / "does_not_exist").empty());
}
