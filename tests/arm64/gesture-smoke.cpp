#include "GestureProgress.hpp"
#include <cassert>
#include <cstdio>
int main() {
    GestureProgress p;
    p.update(100, 200);
    assert(!p.finishesClosed(false, false, 200)); // Opening completes.
    assert(p.finishesClosed(true, false, 200)); // Closing completes.
    assert(p.finishesClosed(false, true, 200)); // Cancelled open returns closed.
    assert(!p.finishesClosed(true, true, 200)); // Cancelled close returns open.
    p.update(-90, 200);
    assert(p.finishesClosed(false, false, 200)); // Reversal cancels opening.
    assert(!p.finishesClosed(true, false, 200));
    p.update(-100, 200);
    assert(p.distance == 0);
    p.update(1000, 200);
    assert(p.distance == 200);
    std::puts("Gesture progress/reversal/cancellation tests passed");
}
