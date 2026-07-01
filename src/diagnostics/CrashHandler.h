#pragma once

#include <QString>

namespace diagnostics {

void installCrashHandler();
QString crashLogPath();
void logCurrentException(const char* context);

// Delete the crash log file. Returns true if it was removed or did not exist.
bool clearCrashLog();

} // namespace diagnostics
