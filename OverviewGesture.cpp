#include "OverviewGesture.hpp"
#include "scrollOverview.hpp"
#include "OverviewOpen.hpp"
#include "Config.hpp"
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/output/Monitor.hpp>

void COverviewGesture::begin(const STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);
    m_entries.clear();
    m_progress = {};
    m_firstUpdate = true;
    const auto monitor = Desktop::focusState()->monitor();
    if (!monitor) return;
    const auto monitors = m_all ? State::monitorState()->monitors() : std::vector<PHLMONITOR>{monitor};
    for (const auto& target : monitors) {
        auto existing = scrollOverviewForMonitor(target);
        if (m_closeOnly && existing && existing->isSwapFeedback()) { existing->close(); continue; }
        if (m_closeOnly && !existing) continue; // A downward gesture never opens.
        auto [overview, created] = openOverview(target, true);
        if (!overview) continue;
        if (!created) {
            overview->selectHoveredWorkspace();
            overview->setClosing(true);
        }
        m_entries.push_back({overview, !created});
        g_pScrollOverview = overview;
    }
}
void COverviewGesture::update(const STrackpadGestureUpdate& e) {
    if (m_firstUpdate) { m_firstUpdate = false; return; }
    const auto monitor = Desktop::focusState()->monitor();
    const auto limit = ScrollOverview::Config::getGestureDistance(monitor);
    m_progress.update(distance(e), limit);
    for (const auto& entry : m_entries) {
        if (const auto overview = entry.overview.lock())
            overview->onSwipeUpdate(std::max(0.01, m_progress.distance));
    }
}
void COverviewGesture::end(const STrackpadGestureEnd& e) {
    const bool cancelled = (e.swipe && e.swipe->cancelled) || (e.pinch && e.pinch->cancelled);
    const auto limit = ScrollOverview::Config::getGestureDistance(Desktop::focusState()->monitor());
    for (const auto& entry : m_entries) {
        const auto overview = entry.overview.lock();
        if (!overview) continue;
        overview->m_isSwiping = false;
        overview->setClosing(m_progress.finishesClosed(entry.startedOpen, cancelled, limit));
        overview->onSwipeEnd();
        overview->damage();
        if (std::ranges::find(scrollOverviews(), overview) != scrollOverviews().end())
            overview->resetSwipe();
    }
    m_entries.clear();
}
