#include "NavigateGesture.hpp"

#include <algorithm>

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/output/Monitor.hpp>

// finger travel along both axes for this event, in logical px. the overview picks the axis that
// matches its layout, so both are passed through.
static Vector2D swipeDelta(const IPointer::SSwipeUpdateEvent* swipe, float scale) {
    if (!swipe)
        return {};

    return swipe->delta * scale;
}

void COverviewNavigateGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);

    m_overview.reset();
    m_fallbackActive = false;

    const auto MONITOR  = Desktop::focusState()->monitor();
    auto       overview = MONITOR ? scrollOverviewForMonitor(MONITOR) : SP<IOverview>{};

    // no overview on the focused monitor: behave exactly like a native workspace swipe
    if (!overview) {
        m_fallbackActive = true;
        m_fallback.begin(e);
        return;
    }

    m_overview = overview;
    overview->onNavigateSwipeBegin();
    overview->onNavigateSwipeUpdate(swipeDelta(e.swipe, e.scale));
}

void COverviewNavigateGesture::update(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (m_fallbackActive) {
        m_fallback.update(e);
        return;
    }

    const auto OVERVIEW = m_overview.lock();
    if (!OVERVIEW)
        return;

    OVERVIEW->onNavigateSwipeUpdate(swipeDelta(e.swipe, e.scale));
}

void COverviewNavigateGesture::end(const ITrackpadGesture::STrackpadGestureEnd& e) {
    if (m_fallbackActive) {
        m_fallbackActive = false;
        m_fallback.end(e);
        return;
    }

    const auto OVERVIEW = m_overview.lock();
    m_overview.reset();

    if (!OVERVIEW)
        return;

    // the overview may already be gone (closed mid-swipe); the registry is authoritative
    if (std::ranges::find(scrollOverviews(), OVERVIEW) == scrollOverviews().end())
        return;

    OVERVIEW->onNavigateSwipeEnd();
}

bool COverviewNavigateGesture::isDirectionSensitive() {
    // same as the native workspace swipe: bidirectional along the configured axis
    return true;
}
