// Self-test for the calc library. Exits 0 when every check passes.
// The bigtest harness verifies these checks are NOT modified.

#include <cstdio>

#include "calc.h"

namespace {
int failures = 0;

void expect(const char* name, int got, int want) {
    if (got != want) {
        std::printf("FAIL %s: got %d, want %d\n", name, got, want);
        ++failures;
    } else {
        std::printf("ok   %s\n", name);
    }
}
}  // namespace

int main() {
    expect("sumRange(1,5)", sumRange(1, 5), 15);
    expect("sumRange(3,3)", sumRange(3, 3), 3);
    expect("sumRange(5,1)", sumRange(5, 1), 0);

    const int v[] = {3, 9, 4};
    expect("maxOf({3,9,4})", maxOf(v, 3), 9);

    if (failures != 0) {
        std::printf("%d test(s) failed\n", failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
