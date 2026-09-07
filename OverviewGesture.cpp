#include "OverviewGesture.hpp"

#include "scrollOverview.hpp"
#include "OverviewOpen.hpp"

#include <algorithm>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/output/Monitor.hpp>

void COverviewGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);

    m_lastDelta   = 0.F;
    m_firstUpdate = true;

    const auto MONITOR  = Desktop::focusState()->monitor();
    if (!MONITOR)
        return;

    const auto [overview, created] = openOverview(MONITOR, true);
    if (!overview)
        return;

    if (!created) {
        overview->selectHoveredWorkspace();
        overview->setClosing(true);
    }

    m_overview          = overview;
    g_pScrollOverview = overview;
}

void COverviewGesture::update(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    const auto OVERVIEW = m_overview.lock();
    if (!OVERVIEW)
        return;

    if (m_firstUpdate) {
        m_firstUpdate = false;
        return;
    }

    m_lastDelta += distance(e);

    if (m_lastDelta <= 0.01) // plugin will crash if swipe ends at <= 0
        m_lastDelta = 0.01;

    OVERVIEW->onSwipeUpdate(m_lastDelta);
}

void COverviewGesture::end(const ITrackpadGesture::STrackpadGestureEnd& e) {
    const auto OVERVIEW = m_overview.lock();
    if (OVERVIEW)
        OVERVIEW->onSwipeEnd();

    // Re-check the same instance since onSwipeEnd can finish its close.
    if (std::ranges::find(scrollOverviews(), OVERVIEW) != scrollOverviews().end())
        OVERVIEW->resetSwipe();
    m_overview.reset();
}
