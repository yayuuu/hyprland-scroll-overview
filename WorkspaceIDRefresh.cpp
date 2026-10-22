#include "WorkspaceIDRefresh.hpp"
#include "scrollOverview.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <algorithm>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

namespace {
std::vector<std::pair<PHLWORKSPACEREF, CHyprSignalListener>> watchers;
CHyprSignalListener created;
wl_event_source* pending = nullptr;
std::vector<std::pair<PHLWORKSPACEREF, WORKSPACEID>> changed;

void queueRefresh() {
    if (pending)
        return;
    // A swap changes three IDs synchronously. Publish only the final state,
    // after the temporary ID has disappeared, even when no overview is open.
    pending = wl_event_loop_add_idle(g_pCompositor->m_wlEventLoop, [](void*) {
        pending = nullptr;
        const auto changes = std::exchange(changed, {});
        const auto active = Desktop::focusState()->monitor();
        const auto current = active ? active->m_activeWorkspace : PHLWORKSPACE{};
        const auto previous = std::find_if(changes.begin(), changes.end(), [&current](const auto& entry) { return entry.first.lock() == current; });
        const bool swapped = changes.size() == 2 && previous != changes.end() && changes[0].first.lock() && changes[1].first.lock()
            && changes[0].first.lock()->m_id == changes[1].second && changes[1].first.lock()->m_id == changes[0].second
            && changes[0].first.lock()->m_monitor == active && changes[1].first.lock()->m_monitor == active;
        const auto scene = active ? scrollOverviewForMonitor(active) : SP<IOverview>{};
        if (swapped && current && !active->m_activeSpecialWorkspace && !g_pSessionLockManager->isSessionLocked()
            && !g_pUnifiedWorkspaceSwipe->isGestureInProgress() && ScrollOverview::Config::getValue<bool>("animations:enabled")) {
            std::unordered_map<WORKSPACEID, WORKSPACEID> oldIDs;
            for (const auto& [ref, old] : changes) oldIDs[ref.lock()->m_id] = old;
            const int direction = current->m_id < previous->second ? -1 : 1;
            if (scene && scene->isSwapFeedback())
                static_cast<CScrollOverview*>(scene.get())->startSwapFeedback(direction, oldIDs);
            else if (!scene && ensureScrollOverviewHooks()) {
                auto effect = makeShared<CScrollOverview>(current, true, active, true);
                registerScrollOverview(effect);
                effect->startSwapFeedback(direction, oldIDs);
            }
        }
        const auto overviews = scrollOverviews();
        for (const auto& overview : overviews) {
            if (auto* scroll = dynamic_cast<CScrollOverview*>(overview.get()))
                if (!scroll->isSwapFeedback()) scroll->onWorkspaceIDsChanged();
        }
        // Quickshell does not handle Hyprland 0.56's changeworkspaceid event.
        // Its existing configreloaded IPC handler re-queries workspaces,
        // monitors and toplevels. This is a model refresh, not a config reload;
        // do not emit config.reloaded or rebuild the user's Lua configuration.
        g_pEventManager->postEvent({"configreloaded", ""});
    }, nullptr);
}

void watch(PHLWORKSPACEREF ref) {
    std::erase_if(watchers, [](const auto& entry) { return entry.first.expired(); });
    const auto workspace = ref.lock();
    if (!workspace)
        return;
    watchers.emplace_back(ref, workspace->m_events.idChanged.listen([ref, previousID = workspace->m_id]() mutable {
        const auto workspace = ref.lock();
        if (!workspace) return;
        if (std::ranges::none_of(changed, [&workspace](const auto& entry) { return entry.first.lock() == workspace; }))
            changed.emplace_back(ref, previousID);
        previousID = workspace->m_id;
        queueRefresh();
    }));
}
}

void initializeWorkspaceIDRefresh() {
    for (const auto& workspace : State::workspaceState()->workspaces())
        watch(workspace);
    created = Event::bus()->m_events.workspace.created.listen([](PHLWORKSPACEREF workspace) { watch(workspace); });
}

void stopWorkspaceIDRefresh() {
    if (pending) {
        wl_event_source_remove(pending);
        pending = nullptr;
    }
    created.reset();
    watchers.clear();
    changed.clear();
}
