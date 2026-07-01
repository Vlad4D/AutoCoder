#include "calc.h"

int sumRange(int a, int b) {
    int total = 0;
    for (int i = a; i < b; ++i) {
        total += i;
    }
    return total;
}

int maxOf(const int* values, int n) {
    int best = values[0];
    for (int i = 1; i < n; ++i) {
        if (values[i] > best) best = values[i];
    }
    return best;
}
