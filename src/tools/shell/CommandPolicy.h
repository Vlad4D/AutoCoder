#pragma once

#include <set>
#include <string>

namespace autocoder::shell {

// Result of classifying a shell command for the safety gate.
//   Allow     -- known-safe, in-folder, reversible: run silently.
//   Prompt    -- pause and ask the user (fail-closed default for anything unknown,
//                plus capability/destructive commands even if in-folder).
//   HardBlock -- never run, even with user consent (catastrophic/irreversible).
enum class Verdict { Allow, Prompt, HardBlock };

struct PolicyResult {
    Verdict verdict = Verdict::Prompt;  // fail closed
    std::string reason;                 // shown in UI + returned to LLM on deny/block
    std::string offendingSegment;       // the command segment that forced the verdict
};

// Pure classifier. `userAllowlist` is the set of FULL exact command strings the
// user previously chose "Always allow" for. Matching is on the entire command
// string (never a prefix/segment) so that, e.g., "build.sh && rm -rf ~" can
// never be whitelisted by approving "build.sh".
PolicyResult classifyCommand(const std::string& command,
                             const std::set<std::string>& userAllowlist);

// ----- QSettings-backed persistence (thin wrappers; tested via the app) -----

// All FULL exact commands the user has chosen to "always allow".
std::set<std::string> loadUserAllowlist();

// Persist one FULL exact command to the always-allow list.
void addToUserAllowlist(const std::string& exactCommand);

// First-run consent: true once the user has accepted the autonomy disclaimer.
bool hasShellConsent();

// Record consent (app version + UTC ISO-8601 timestamp).
void recordShellConsent();

}  // namespace autocoder::shell
