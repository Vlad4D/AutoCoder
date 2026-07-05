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

    // ---- symbol enrichment ----
    CHECK_MSG(map.find("src/math.cpp: add(), sub()") != std::string::npos,
              "C++ sources carry their top-level functions");
    CHECK_MSG(map.find("src/util.h: class Helper") != std::string::npos,
              "headers carry their top-level classes");
    CHECK_MSG(map.find("run()") == std::string::npos,
              "class members are not listed individually");
    CHECK_MSG(map.find("README.md:") == std::string::npos,
              "non-C files stay plain path lines");
    CHECK_MSG(map.find("top-level symbols for C/C++ sources") != std::string::npos,
              "header text mentions the symbol enrichment");

    // Byte-stable: unchanged tree -> identical map (provider cache).
    CHECK_MSG(repomap::build(root) == map,
              "map is byte-identical across rebuilds");

    // Symbol suffix helper directly.
    CHECK_MSG(repomap::symbolSuffixForFile(root / "src" / "math.cpp")
                  == ": add(), sub()",
              "symbolSuffixForFile formats functions as name()");
    CHECK_MSG(repomap::symbolSuffixForFile(root / "README.md").empty(),
              "non-C-family files yield no suffix");
    CHECK_MSG(repomap::symbolSuffixForFile(root / "does_not_exist.cpp").empty(),
              "missing files yield no suffix");
    {
        // Per-line cap: a long symbol list is cut with an ellipsis.
        std::string many;
        for (int i = 0; i < 50; ++i)
            many += "int func_number_" + std::to_string(i) + "() {\n    return 0;\n}\n";
        writeFile(root / "src" / "many.cpp", many);
        const std::string suffix =
            repomap::symbolSuffixForFile(root / "src" / "many.cpp");
        CHECK_MSG(!suffix.empty() && suffix.size() <= 2 + 200 + 5,
                  "suffix respects the per-line character cap");
        CHECK_MSG(suffix.find(", ...") != std::string::npos,
                  "cut symbol list ends with an ellipsis");
    }

    // Budget priority: at a budget that fits every path line but not the
    // symbols, the plain listing survives whole and ALL enrichment is dropped
    // -- path coverage never regresses. The plain-map size is exactly the full
    // map minus every suffix's bytes, so build at that budget expects zero
    // suffixes.
    {
        const std::string full = repomap::build(root);
        std::size_t suffixBytes = 0;
        suffixBytes += repomap::symbolSuffixForFile(root / "src" / "math.cpp").size();
        suffixBytes += repomap::symbolSuffixForFile(root / "src" / "util.h").size();
        suffixBytes += repomap::symbolSuffixForFile(root / "src" / "many.cpp").size();
        CHECK(suffixBytes > 0);

        const std::string squeezed = repomap::build(root, full.size() - suffixBytes);
        // Every path is still present.
        CHECK_MSG(squeezed.find("src/math.cpp") != std::string::npos
                      && squeezed.find("src/util.h") != std::string::npos
                      && squeezed.find("src/many.cpp") != std::string::npos,
                  "all path lines survive a symbols-don't-fit budget");
        // No enrichment at all -- not even the shortest suffix.
        CHECK_MSG(squeezed.find(": add(), sub()") == std::string::npos
                      && squeezed.find(": class Helper") == std::string::npos,
                  "no symbols are emitted when they cannot all fit");
        CHECK_MSG(squeezed.find("[project map truncated]") == std::string::npos,
                  "dropping enrichment is not path truncation");
    }

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
