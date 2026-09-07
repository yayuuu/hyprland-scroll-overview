#include "IOverview.hpp"

#include <algorithm>

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/output/Monitor.hpp>

static std::vector<SP<IOverview>> g_scrollOverviews;
static std::vector<WP<IOverview>> g_crossMonitorDragSession;

static void pruneCrossMonitorDragSession() {
    std::erase_if(g_crossMonitorDragSession, [](const auto& ref) {
        const auto overview = ref.lock();
        return !overview || std::ranges::find(g_scrollOverviews, overview) == g_scrollOverviews.end();
    });

    if (g_crossMonitorDragSession.size() < 2)
        g_crossMonitorDragSession.clear();
}

const std::vector<SP<IOverview>>& scrollOverviews() {
    return g_scrollOverviews;
}

SP<IOverview> scrollOverviewForMonitor(PHLMONITOR monitor) {
    if (!monitor)
        return {};

    const auto IT = std::ranges::find_if(g_scrollOverviews, [&monitor](const auto& overview) {
        return overview && overview->pMonitor.lock() == monitor;
    });
    return IT == g_scrollOverviews.end() ? SP<IOverview>{} : *IT;
}

SP<IOverview> scrollOverviewAt(const Vector2D& point) {
    const auto IT = std::ranges::find_if(g_scrollOverviews, [&point](const auto& overview) {
        const auto monitor = overview ? overview->pMonitor.lock() : PHLMONITOR{};
        return monitor && monitor->logicalBox().containsPoint(point);
    });
    return IT == g_scrollOverviews.end() ? SP<IOverview>{} : *IT;
}

SP<IOverview> activeScrollOverview() {
    if (const auto monitor = Desktop::focusState()->monitor()) {
        if (const auto overview = scrollOverviewForMonitor(monitor))
            return overview;
    }

    if (g_pScrollOverview && std::ranges::find(g_scrollOverviews, g_pScrollOverview) != g_scrollOverviews.end())
        return g_pScrollOverview;

    return g_scrollOverviews.empty() ? SP<IOverview>{} : g_scrollOverviews.front();
}

void closeAll() {
    const auto overviews = g_scrollOverviews;
    for (const auto& overview : overviews) {
        if (overview)
            overview->close();
    }
}

void registerScrollOverview(const SP<IOverview>& overview) {
    if (!overview || std::ranges::find(g_scrollOverviews, overview) != g_scrollOverviews.end())
        return;

    g_scrollOverviews.emplace_back(overview);
    g_pScrollOverview = overview;
}

void unregisterScrollOverview(IOverview* overview) {
    if (!overview)
        return;

    removeFromCrossMonitorDragSession(overview);
    std::erase_if(g_scrollOverviews, [overview](const auto& candidate) { return candidate.get() == overview; });
    g_pScrollOverview = activeScrollOverview();
}

void clearScrollOverviews() {
    g_crossMonitorDragSession.clear();
    auto overviews = std::move(g_scrollOverviews);
    g_scrollOverviews.clear();
    g_pScrollOverview.reset();
    overviews.clear();
}

void markCrossMonitorDragSession(IOverview* source, const SP<IOverview>& destination) {
    if (!source || !destination || source == destination.get())
        return;

    pruneCrossMonitorDragSession();

    const auto SOURCE = std::ranges::find_if(g_scrollOverviews, [source](const auto& overview) { return overview.get() == source; });
    if (SOURCE == g_scrollOverviews.end() || std::ranges::find(g_scrollOverviews, destination) == g_scrollOverviews.end())
        return;

    const bool SOURCEINSESSION = std::ranges::any_of(g_crossMonitorDragSession, [source](const auto& ref) { return ref.lock().get() == source; });
    if (!SOURCEINSESSION)
        g_crossMonitorDragSession = {WP<IOverview>{*SOURCE}};

    const bool DESTINATIONINSESSION = std::ranges::any_of(g_crossMonitorDragSession, [&destination](const auto& ref) { return ref.lock() == destination; });
    if (!DESTINATIONINSESSION)
        g_crossMonitorDragSession.emplace_back(destination);
}

bool closeCrossMonitorDragSession() {
    pruneCrossMonitorDragSession();
    if (g_crossMonitorDragSession.empty())
        return false;

    std::vector<SP<IOverview>> members;
    members.reserve(g_crossMonitorDragSession.size());
    for (const auto& ref : g_crossMonitorDragSession) {
        if (const auto overview = ref.lock())
            members.emplace_back(overview);
    }

    for (const auto& overview : members)
        overview->close();

    pruneCrossMonitorDragSession();
    return true;
}

void removeFromCrossMonitorDragSession(IOverview* overview) {
    if (!overview)
        return;

    std::erase_if(g_crossMonitorDragSession, [overview](const auto& ref) {
        const auto member = ref.lock();
        return !member || member.get() == overview || std::ranges::find(g_scrollOverviews, member) == g_scrollOverviews.end();
    });
    if (g_crossMonitorDragSession.size() < 2)
        g_crossMonitorDragSession.clear();
}
