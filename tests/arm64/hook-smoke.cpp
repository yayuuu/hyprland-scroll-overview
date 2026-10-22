#include "Arm64Hook.hpp"
#include <cstdio>
#include <stdexcept>

namespace {
volatile int global_value = 9;
Arm64Hook* integer_hook;
Arm64Hook* float_hook;

__attribute__((noinline)) int target(int x) { return x * 3 + global_value; }
__attribute__((noinline)) int replacement(int x) {
    return reinterpret_cast<int (*)(int)>(integer_hook->m_original)(x) + 100;
}
__attribute__((noinline)) double float_target(double x, double y) { return x * y + global_value; }
__attribute__((noinline)) double float_replacement(double x, double y) {
    return reinterpret_cast<double (*)(double, double)>(float_hook->m_original)(x, y) + 0.5;
}

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    int (*volatile call)(int) = target;
    double (*volatile float_call)(double, double) = float_target;
    check(call(7) == 30, "baseline");
    for (int i = 0; i < 100; ++i) {
        Arm64Hook h(reinterpret_cast<void*>(target), reinterpret_cast<void*>(replacement));
        integer_hook = &h;
        check(h.hook() && h.hook(), "install/idempotent install");
        Arm64Hook duplicate(reinterpret_cast<void*>(target), reinterpret_cast<void*>(replacement));
        check(!duplicate.hook(), "duplicate must not own or remove another hook");
        check(call(7) == 130, "hook/original with PC-relative global load");
        check(h.unhook() && h.unhook(), "remove/idempotent remove");
        check(call(7) == 30, "restored machine code");
    }
    {
        Arm64Hook h(reinterpret_cast<void*>(float_target), reinterpret_cast<void*>(float_replacement));
        float_hook = &h;
        check(h.hook(), "floating-point install");
        check(float_call(2.5, 4) == 19.5, "floating-point arguments and return");
    }
    check(float_call(2.5, 4) == 19, "RAII cleanup");
    // The same cleanup happens when initialization throws after installing.
    try {
        Arm64Hook h(reinterpret_cast<void*>(target), reinterpret_cast<void*>(replacement));
        integer_hook = &h;
        check(h.hook(), "failure-path install");
        throw 1;
    } catch (int) {}
    check(call(7) == 30, "exception cleanup");
    std::puts("ARM64 hook lifecycle smoke test passed");
}
