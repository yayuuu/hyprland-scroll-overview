#pragma once
#include <algorithm>

// Distance-driven progress, with reversal and cancellation restoring the start.
struct GestureProgress {
    double distance = 0;
    void update(double delta, double limit) {
        distance = std::clamp(distance + delta, 0.0, std::max(1.0, limit));
    }
    bool finishesClosed(bool startedOpen, bool cancelled, double limit) const {
        const bool commit = !cancelled && distance >= std::max(1.0, limit) * 0.35;
        return commit ? startedOpen : !startedOpen;
    }
};
