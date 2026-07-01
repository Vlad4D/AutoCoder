#include "TestHarness.h"

#include <regex>

#include "tools/PathUtil.h"
#include "tools/ToolContext.h"

using namespace test;
namespace fs = std::filesystem;

namespace {
ToolContext ctxForRoot(const fs::path& root) {
    ToolContext c;
    c.projectRoot = root;
    return c;
}
}  // namespace

void suite_paths() {
    const fs::path root = freshDir("paths");
    fs::create_directories(root / "sub" / "deep");
    writeFile(root / "sub" / "a.txt", "x");
    const ToolContext ctx = ctxForRoot(root);

    // ---- resolveSafely: inside the project root ----
    CHECK(!pathutil::resolveSafely("sub/a.txt", ctx).empty());
    CHECK(!pathutil::resolveSafely("sub/deep/../a.txt", ctx).empty());  // normalises within root
    CHECK(!pathutil::resolveSafely(pathutil::toUtf8(root / "sub" / "a.txt"), ctx).empty());

    // ---- resolveSafely: escapes must be rejected (return empty) ----
    CHECK(pathutil::resolveSafely("../outside.txt", ctx).empty());
    CHECK(pathutil::resolveSafely("sub/../../outside.txt", ctx).empty());
    CHECK(pathutil::resolveSafely("../../../../Windows/system32/drivers/etc/hosts", ctx).empty());
    CHECK(pathutil::resolveSafely(pathutil::toUtf8(root.parent_path() / "outside.txt"), ctx).empty());

    // ---- containedIn ----
    CHECK(pathutil::containedIn(root, root));                       // root contains itself
    CHECK(pathutil::containedIn(root / "sub" / "a.txt", root));
    CHECK(!pathutil::containedIn(root.parent_path(), root));        // parent not inside
    // Sibling-prefix trick: "<root>2" must NOT be considered inside "<root>".
    fs::path sibling = root;
    sibling += "2";
    CHECK_MSG(!pathutil::containedIn(sibling, root), "sibling sharing name prefix not contained");

    // ---- fromUtf8: drive aliases / URLs convert to native absolute paths ----
    {
        const fs::path wsl = pathutil::fromUtf8("/mnt/c/Users/x");
        CHECK_MSG(wsl.is_absolute() && !wsl.root_name().empty(), "WSL /mnt/c -> drive path");
        CHECK(wsl.filename() == fs::path("x"));

        const fs::path msys = pathutil::fromUtf8("/c/Users/x");
        CHECK(msys.is_absolute() && !msys.root_name().empty());

        const fs::path url = pathutil::fromUtf8("file:///C:/foo%20bar/baz.txt");
        CHECK_MSG(url.filename() == fs::path("baz.txt"), "file:// URL percent-decoded");
        CHECK(pathutil::toUtf8(url).find("foo bar") != std::string::npos);  // %20 -> space
    }
    // Round-trip of a plain native path.
    {
        const std::string s = pathutil::toUtf8(root / "sub" / "a.txt");
        CHECK(pathutil::fromUtf8(s).filename() == fs::path("a.txt"));
    }

    // ---- globToRegex ----
    auto matches = [](const std::string& glob, const std::string& rel) {
        return std::regex_match(rel, std::regex(pathutil::globToRegex(glob),
                                                std::regex::ECMAScript));
    };
    CHECK(matches("**/*.cpp", "src/foo.cpp"));
    CHECK(matches("**/*.cpp", "a/b/c/foo.cpp"));
    CHECK(matches("*.h", "foo.h"));
    CHECK(!matches("*.h", "src/foo.h"));      // single * does not cross '/'
    CHECK(!matches("**/*.cpp", "src/foo.h"));

    // ---- isExcludedDir ----
    CHECK(pathutil::isExcludedDir(".git"));
    CHECK(pathutil::isExcludedDir("node_modules"));
    CHECK(!pathutil::isExcludedDir("src"));
}
