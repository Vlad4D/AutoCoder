#include "TestHarness.h"

#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "tools/AppendTool.h"
#include "tools/CopyFileTool.h"
#include "tools/EditTool.h"
#include "tools/PathUtil.h"
#include "tools/ReadTool.h"
#include "tools/ReplaceLinesTool.h"
#include "tools/ToolContext.h"
#include "tools/WriteTool.h"

using namespace test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {
struct Env {
    fs::path root;
    std::set<fs::path> readSet;
    std::map<fs::path, std::string> snaps;
    ToolContext ctx;
    explicit Env(const fs::path& r) : root(r) {
        ctx.projectRoot = root;
        ctx.readSet = &readSet;
        ctx.fileBeforeSnapshots = &snaps;
    }
};
bool ok(const ToolResult& r) { return r.ok; }
}  // namespace

void suite_edit_tools() {
    const fs::path root = freshDir("edit_tools");
    Env e(root);
    ReadTool readTool;
    WriteTool writeTool;
    AppendTool appendTool;
    EditTool editTool;
    ReplaceLinesTool rlTool;

    // ---- write: new file (no read-before-write) ----
    CHECK(ok(writeTool.execute(json{{"path", "new.txt"}, {"content", "hello\nworld\n"}}, e.ctx)));
    CHECK(readFile(root / "new.txt") == "hello\nworld\n");

    // ---- write: overwriting an existing file requires read first ----
    writeFile(root / "exists.txt", "old");
    CHECK_MSG(!ok(writeTool.execute(json{{"path", "exists.txt"}, {"content", "new"}}, e.ctx)),
              "overwrite without prior read is rejected");
    CHECK(ok(readTool.execute(json{{"path", "exists.txt"}}, e.ctx)));  // marks file read
    CHECK(ok(writeTool.execute(json{{"path", "exists.txt"}, {"content", "new"}}, e.ctx)));
    CHECK(readFile(root / "exists.txt") == "new");

    // ---- write: path escaping the project root is rejected ----
    CHECK(!ok(writeTool.execute(json{{"path", "../escape.txt"}, {"content", "x"}}, e.ctx)));

    // ---- edit: unique match / replace_all / not-found / read-before-write ----
    writeFile(root / "edit.txt", "alpha\nTARGET\nbeta\n");
    CHECK(ok(readTool.execute(json{{"path", "edit.txt"}}, e.ctx)));
    {
        ToolResult r = editTool.execute(json{{"path", "edit.txt"}, {"old_string", "TARGET"},
                                             {"new_string", "CHANGED"}}, e.ctx);
        CHECK(ok(r));
        CHECK(readFile(root / "edit.txt") == "alpha\nCHANGED\nbeta\n");
        CHECK_MSG(r.content.find("Result (lines") != std::string::npos,
                  "edit echoes a numbered snippet of the result");
        CHECK_MSG(r.content.find("2\tCHANGED") != std::string::npos,
                  "snippet shows the new content with its line number");
        CHECK_MSG(r.content.find("1\talpha") != std::string::npos &&
                  r.content.find("3\tbeta") != std::string::npos,
                  "snippet includes surrounding context lines");
    }

    writeFile(root / "dup.txt", "X\nX\n");
    CHECK(ok(readTool.execute(json{{"path", "dup.txt"}}, e.ctx)));
    CHECK_MSG(!ok(editTool.execute(json{{"path", "dup.txt"}, {"old_string", "X"},
                                        {"new_string", "Y"}}, e.ctx)),
              "ambiguous edit rejected without replace_all");
    CHECK(ok(editTool.execute(json{{"path", "dup.txt"}, {"old_string", "X"},
                                   {"new_string", "Y"}, {"replace_all", true}}, e.ctx)));
    CHECK(readFile(root / "dup.txt") == "Y\nY\n");

    CHECK_MSG(!ok(editTool.execute(json{{"path", "edit.txt"}, {"old_string", "NOPE"},
                                        {"new_string", "z"}}, e.ctx)),
              "edit with no match is rejected");

    writeFile(root / "unread.txt", "data");
    CHECK_MSG(!ok(editTool.execute(json{{"path", "unread.txt"}, {"old_string", "data"},
                                        {"new_string", "x"}}, e.ctx)),
              "edit of an unread file is rejected");

    // ---- edit: CRLF file, LF old_string matches; CRLF preserved on write ----
    writeFile(root / "crlf.txt", "a\r\nB\r\nc\r\n");
    CHECK(ok(readTool.execute(json{{"path", "crlf.txt"}}, e.ctx)));
    CHECK(ok(editTool.execute(json{{"path", "crlf.txt"}, {"old_string", "B"},
                                   {"new_string", "BB"}}, e.ctx)));
    CHECK_MSG(readFile(root / "crlf.txt") == "a\r\nBB\r\nc\r\n",
              "edit normalises for matching but preserves CRLF endings");

    // ---- replace_lines: range replace, with surrounding context in the output ----
    writeFile(root / "lines.txt", "l1\nl2\nl3\nl4\nl5\n");
    CHECK(ok(readTool.execute(json{{"path", "lines.txt"}}, e.ctx)));
    {
        ToolResult r = rlTool.execute(json{{"path", "lines.txt"}, {"start_line", 2},
                                           {"end_line", 3}, {"new_lines", "X\nY"}}, e.ctx);
        CHECK(ok(r));
        CHECK_MSG(readFile(root / "lines.txt") == "l1\nX\nY\nl4\nl5\n",
                  "replace_lines replaces the inclusive range");
        CHECK_MSG(r.content.find("=== OLD CONTENT ===") != std::string::npos,
                  "replace_lines emits diff markers");
        CHECK_MSG(r.content.find("l1") != std::string::npos &&
                  r.content.find("l4") != std::string::npos,
                  "replace_lines includes surrounding context lines");
        CHECK_MSG(r.content.find("New content now spans lines 2-3") != std::string::npos,
                  "replace_lines reports the post-edit line range");
        CHECK_MSG(r.content.find("shifted by") == std::string::npos,
                  "no shift note when removed == inserted");
    }

    // ---- replace_lines: post-edit numbering reports the line shift ----
    writeFile(root / "shift.txt", "s1\ns2\ns3\ns4\n");
    CHECK(ok(readTool.execute(json{{"path", "shift.txt"}}, e.ctx)));
    {
        ToolResult r = rlTool.execute(json{{"path", "shift.txt"}, {"start_line", 2},
                                           {"end_line", 3}, {"new_lines", "only"}}, e.ctx);
        CHECK(ok(r));
        CHECK(readFile(root / "shift.txt") == "s1\nonly\ns4\n");
        CHECK_MSG(r.content.find("Lines below the edit shifted by -1") != std::string::npos,
                  "replace_lines reports the negative line shift");
    }

    // ---- replace_lines: insertion (end_line = start_line - 1) ----
    writeFile(root / "ins.txt", "a\nb\n");
    CHECK(ok(readTool.execute(json{{"path", "ins.txt"}}, e.ctx)));
    CHECK(ok(rlTool.execute(json{{"path", "ins.txt"}, {"start_line", 2},
                                 {"end_line", 1}, {"new_lines", "NEW"}}, e.ctx)));
    CHECK_MSG(readFile(root / "ins.txt") == "a\nNEW\nb\n",
              "replace_lines inserts before start_line");

    // ---- replace_lines: out-of-range rejected ----
    CHECK(!ok(rlTool.execute(json{{"path", "ins.txt"}, {"start_line", 99},
                                  {"end_line", 99}, {"new_lines", "z"}}, e.ctx)));

    // ---- append: new file ok; chained append ok; unread existing rejected ----
    CHECK(ok(appendTool.execute(json{{"path", "app.txt"}, {"content", "one\n"}}, e.ctx)));
    CHECK(readFile(root / "app.txt") == "one\n");
    CHECK(ok(appendTool.execute(json{{"path", "app.txt"}, {"content", "two\n"}}, e.ctx)));
    CHECK(readFile(root / "app.txt") == "one\ntwo\n");

    writeFile(root / "preexist.txt", "z");
    CHECK_MSG(!ok(appendTool.execute(json{{"path", "preexist.txt"}, {"content", "q"}}, e.ctx)),
              "append to an unread pre-existing file is rejected");

    // ---- copy_file ----
    CopyFileTool copyTool;
    const std::string binary("A\0B\0C", 5);  // binary content with NUL bytes

    // New destination (creates parent dirs), byte-exact incl NUL.
    writeFile(root / "src.bin", binary);
    CHECK(ok(copyTool.execute(json{{"source", "src.bin"},
                                   {"destination", "out/copy.bin"}}, e.ctx)));
    CHECK_MSG(readFile(root / "out" / "copy.bin") == binary,
              "copy_file copies binary content byte-for-byte");

    // Overwrite a READ-ONLY destination (Perforce-style) -- must clear it.
    writeFile(root / "ro.txt", "OLD");
    fs::permissions(root / "ro.txt", fs::perms::owner_write, fs::perm_options::remove);
    writeFile(root / "newsrc.txt", "NEW");
    CHECK_MSG(ok(copyTool.execute(json{{"source", "newsrc.txt"},
                                       {"destination", "ro.txt"}}, e.ctx)),
              "copy_file overwrites a read-only destination");
    CHECK(readFile(root / "ro.txt") == "NEW");

    // Destination snapshot recorded for revert.
    {
        std::error_code ec;
        const fs::path key = fs::weakly_canonical(root / "ro.txt", ec);
        CHECK_MSG(e.snaps.count(key) == 1, "copy_file snapshots the destination for revert");
    }

    // Path escapes rejected for both source and destination.
    CHECK(!ok(copyTool.execute(json{{"source", "newsrc.txt"}, {"destination", "../esc.txt"}}, e.ctx)));
    CHECK(!ok(copyTool.execute(json{{"source", "../outside"}, {"destination", "x.txt"}}, e.ctx)));
    CHECK(!ok(copyTool.execute(json{{"source", "does_not_exist"}, {"destination", "y.txt"}}, e.ctx)));
}
