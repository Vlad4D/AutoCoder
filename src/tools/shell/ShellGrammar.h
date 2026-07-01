#pragma once

#include <optional>
#include <string>
#include <vector>

namespace autocoder::shell {

// How a parsed segment is joined to the one before it.
enum class SegmentConnector { Always, And, Or, Pipe };

// One command in a (possibly chained) command line, after lexing/expansion.
// envAssignment prefixes (FOO=bar cmd) are stripped from argv -- argv[0] is the
// program that would actually run.
struct ParsedSegment {
    SegmentConnector connector = SegmentConnector::Always;
    std::vector<std::string> argv;
};

// Lex + parse a command line using the SAME rules InternalShell uses to execute
// it. Returns nullopt if the command uses unsupported/rejected syntax (command
// substitution, redirection, heredoc, background execution, unterminated quote),
// with a human-readable reason placed in `error`.
//
// CommandPolicy uses this so its classification sees exactly the segments that
// would run -- an independent tokenizer could diverge on quoting/operators and
// become a gating bypass.
std::optional<std::vector<ParsedSegment>>
parseForClassification(const std::string& command, std::string& error);

}  // namespace autocoder::shell
