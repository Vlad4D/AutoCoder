// Build timestamp — recompiled on every build via a CMake custom command
// that touches this file, so __DATE__ and __TIME__ are always fresh.
#include "build_date.h"

const char* kBuildDate = __DATE__;
const char* kBuildTime = __TIME__;
