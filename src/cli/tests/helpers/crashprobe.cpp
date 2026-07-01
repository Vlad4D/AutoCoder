// Crash-probe helper for the debug-runner tests (built as `crashprobe` next
// to autocoder_tests). Each mode exercises one behavior the runner must
// observe: clean exit, nonzero exit, OutputDebugString capture, and an
// unhandled access violation.

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "ok";

    if (std::strcmp(mode, "exit3") == 0) {
        std::printf("exiting with 3\n");
        return 3;
    }

    if (std::strcmp(mode, "ods") == 0) {
#if defined(_WIN32)
        OutputDebugStringA("D3D12 ERROR: simulated validation message");
#endif
        std::printf("ods sent\n");
        return 0;
    }

    if (std::strcmp(mode, "crash") == 0) {
        std::printf("about to crash\n");
        std::fflush(stdout);
#if defined(_WIN32)
        OutputDebugStringA("last words before the crash");
#endif
        volatile int* p = reinterpret_cast<volatile int*>(0x10);
        *p = 42;  // access violation writing address 0x10
        return 0;
    }

    std::printf("ok\n");
    return 0;
}
