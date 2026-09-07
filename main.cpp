#define WLR_USE_UNSTABLE

#include <unistd.h>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/managers/input/trackpad/GestureTypes.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>

#include <hyprutils/string/ConstVarList.hpp>
using namespace Hyprutils::String;

#include "globals.hpp"
#include "Config.hpp"
#include "PluginVersion.hpp"
#include "scrollOverview.hpp"
#include "OverviewGesture.hpp"
#include "OverviewOpen.hpp"

// Methods
static CFunctionHook* g_pScrollRenderWorkspaceHook = nullptr;
static CFunctionHook* g_pScrollAddDamageHookA      = nullptr;
static CFunctionHook* g_pScrollAddDamageHookB      = nullptr;
static CFunctionHook* g_pScrollDamageSurfaceHook   = nullptr;
static CFunctionHook* g_pScrollScheduleFrameHook   = nullptr;
static CFunctionHook* g_pScrollSendFrameEventsHook = nullptr;
static CFunctionHook* g_pScrollSurfaceFrameHook    = nullptr;
static CFunctionHook* g_pScrollMoveMouseHook       = nullptr;
typedef void (*origRenderWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, const CBox&);
typedef void (*origAddDamageA)(void*, const CBox&);
typedef void (*origAddDamageB)(void*, const pixman_region32_t*);
typedef void (*origDamageSurface)(void*, SP<CWLSurfaceResource>, double, double, double);
typedef void (*origScheduleFrame)(void*, Aquamarine::IOutput::scheduleFrameReason);
typedef void (*origSendFrameEventsToWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);
typedef void (*origSurfaceFrame)(void*, const Time::steady_tp&);
typedef void (*origMoveMouse)(void*, const Vector2D&);

static bool g_unloading = false;

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static bool renderingOverview = false;
static bool damageFromSurface = false;
static PHLMONITORREF renderingOverviewMonitor;
static bool                g_scrollOverviewHooksActive      = false;
static bool                g_scrollMoveMouseHookActive      = false;
static bool                g_scrollMoveMouseHookUnavailable = false;
static bool                g_scrollMoveMouseHookWarned      = false;
static CHyprSignalListener g_configReloadHook;

static void failNotif(const std::string& reason);
static void warnNativeDragUnavailable();
static void disableNativeDragHook();
static void reconcileNativeDragHook();

bool ensureScrollOverviewHooks() {
    if (g_scrollOverviewHooksActive)
        return true;

    bool success = g_pScrollRenderWorkspaceHook->hook();
    success      = success && g_pScrollScheduleFrameHook->hook();
    success      = success && g_pScrollDamageSurfaceHook->hook();
    success      = success && g_pScrollSendFrameEventsHook->hook();
    success      = success && g_pScrollSurfaceFrameHook->hook();
    success      = success && g_pScrollAddDamageHookA->hook();
    success      = success && g_pScrollAddDamageHookB->hook();

    if (!success) {
        disableScrollOverviewHooks();
        failNotif("Failed enabling overview hooks (is other overview plugin enabled?)");
        return false;
    }

    g_scrollOverviewHooksActive = true;
    reconcileNativeDragHook();
    return true;
}

void disableScrollOverviewHooks() {
    disableNativeDragHook();
    if (g_pScrollAddDamageHookB)
        g_pScrollAddDamageHookB->unhook();
    if (g_pScrollAddDamageHookA)
        g_pScrollAddDamageHookA->unhook();
    if (g_pScrollSurfaceFrameHook)
        g_pScrollSurfaceFrameHook->unhook();
    if (g_pScrollSendFrameEventsHook)
        g_pScrollSendFrameEventsHook->unhook();
    if (g_pScrollDamageSurfaceHook)
        g_pScrollDamageSurfaceHook->unhook();
    if (g_pScrollScheduleFrameHook)
        g_pScrollScheduleFrameHook->unhook();
    if (g_pScrollRenderWorkspaceHook)
        g_pScrollRenderWorkspaceHook->unhook();

    g_scrollOverviewHooksActive = false;
}

static void hkMoveMouse(void* thisptr, const Vector2D& mousePos) {
    rc<origMoveMouse>(g_pScrollMoveMouseHook->m_original)(thisptr, mousePos);

    // moveMouse() updates dragThresholdReached().
    if (!g_unloading && g_scrollMoveMouseHookActive && ScrollOverview::Config::getCrossMonitorDrag()) {
        try {
            adoptNativeWindowDragIntoOverview();
        } catch (...) {
            // Never unwind through a Hyprland hook.
        }
    }
}

static void hkScheduleFrame(void* thisptr, Aquamarine::IOutput::scheduleFrameReason reason) {
    const auto OVERVIEW = scrollOverviewForMonitor(sc<Monitor::CMonitor*>(thisptr)->m_self.lock());
    if (OVERVIEW) {
        using enum Aquamarine::IOutput::scheduleFrameReason;

        const bool THROTTLEDREASON =
            reason == AQ_SCHEDULE_UNKNOWN || reason == AQ_SCHEDULE_CLIENT_UNKNOWN || reason == AQ_SCHEDULE_NEEDS_FRAME || reason == AQ_SCHEDULE_RENDER_MONITOR ||
            reason == AQ_SCHEDULE_DAMAGE;

        if (THROTTLEDREASON && !OVERVIEW->blockDamageReporting && !OVERVIEW->shouldAllowRealtimePreviewSchedule())
            return;
    }

    rc<origScheduleFrame>(g_pScrollScheduleFrameHook->m_original)(thisptr, reason);
}

//
static void hkRenderWorkspace(void* thisptr, PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, const Time::steady_tp& now, const CBox& geometry) {
    const auto OVERVIEW = scrollOverviewForMonitor(pMonitor);
    if (!OVERVIEW || renderingOverview)
        rc<origRenderWorkspace>(g_pScrollRenderWorkspaceHook->m_original)(thisptr, pMonitor, pWorkspace, now, geometry);
    else {
        const bool PREVRENDERINGOVERVIEW = renderingOverview;
        const auto PREVRENDERINGMONITOR  = renderingOverviewMonitor;
        const auto PREVOVERVIEW          = g_pScrollOverview;
        renderingOverview                = true;
        renderingOverviewMonitor         = pMonitor;
        g_pScrollOverview                = OVERVIEW;
        OVERVIEW->render();
        g_pScrollOverview = PREVOVERVIEW;
        renderingOverviewMonitor = PREVRENDERINGMONITOR;
        renderingOverview = PREVRENDERINGOVERVIEW;
    }
}

static void hkDamageSurface(void* thisptr, SP<CWLSurfaceResource> surface, double x, double y, double scale) {
    const bool HANDLED = scrollOverviews().empty() ||
        std::ranges::any_of(scrollOverviews(), [](const auto& overview) { return overview && overview->blockDamageReporting; }) ||
        std::ranges::all_of(scrollOverviews(), [&surface](const auto& overview) { return !overview || overview->shouldHandleSurfaceDamage(surface); });
    if (HANDLED) {
        const bool PREVDAMAGEFROMSURFACE = damageFromSurface;
        damageFromSurface                = !scrollOverviews().empty();
        rc<origDamageSurface>(g_pScrollDamageSurfaceHook->m_original)(thisptr, surface, x, y, scale);
        damageFromSurface = PREVDAMAGEFROMSURFACE;
    }
}

static void hkSendFrameEventsToWorkspace(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
    if (scrollOverviewForMonitor(monitor))
        return;

    rc<origSendFrameEventsToWorkspace>(g_pScrollSendFrameEventsHook->m_original)(thisptr, monitor, workspace, now);
}

static void hkSurfaceFrame(void* thisptr, const Time::steady_tp& now) {
    const auto SURFACE = sc<CWLSurfaceResource*>(thisptr)->m_self.lock();

    if (std::ranges::any_of(scrollOverviews(), [&SURFACE, &now](const auto& overview) { return overview && !overview->shouldAllowSurfaceFrame(SURFACE, now); }))
        return;

    rc<origSurfaceFrame>(g_pScrollSurfaceFrameHook->m_original)(thisptr, now);
}

static void hkAddDamageA(void* thisptr, const CBox& box) {
    const auto PMONITOR = sc<Monitor::CMonitor*>( thisptr );
    const auto OVERVIEW = scrollOverviewForMonitor(PMONITOR->m_self.lock());

    if (OVERVIEW && renderingOverviewMonitor.lock() == PMONITOR->m_self.lock() && !damageFromSurface && OVERVIEW->shouldSuppressRenderDamage()) {
        return;
    }

    if (!OVERVIEW || OVERVIEW->blockDamageReporting || damageFromSurface) {
        rc<origAddDamageA>(g_pScrollAddDamageHookA->m_original)(thisptr, box);
        return;
    }

    OVERVIEW->onDamageReported();
    rc<origAddDamageA>(g_pScrollAddDamageHookA->m_original)(thisptr, box);
}

static void hkAddDamageB(void* thisptr, const pixman_region32_t* rg) {
    const auto PMONITOR = sc<Monitor::CMonitor*>(thisptr);
    const auto OVERVIEW = scrollOverviewForMonitor(PMONITOR->m_self.lock());

    if (OVERVIEW && renderingOverviewMonitor.lock() == PMONITOR->m_self.lock() && !damageFromSurface && OVERVIEW->shouldSuppressRenderDamage()) {
        return;
    }

    if (!OVERVIEW || OVERVIEW->blockDamageReporting || damageFromSurface) {
        rc<origAddDamageB>(g_pScrollAddDamageHookB->m_original)(thisptr, rg);
        return;
    }

    OVERVIEW->onDamageReported();
    rc<origAddDamageB>(g_pScrollAddDamageHookB->m_original)(thisptr, rg);
}

static std::pair<std::string, std::string> splitOverviewArg(const std::string& arg) {
    const auto FIRST = arg.find_first_not_of(" \t");
    if (FIRST == std::string::npos)
        return {"on", ""};

    const auto SEPARATOR = arg.find_first_of(" \t", FIRST);
    if (SEPARATOR == std::string::npos)
        return {arg.substr(FIRST), ""};

    const auto TARGET = arg.find_first_not_of(" \t", SEPARATOR);
    if (TARGET == std::string::npos)
        return {arg.substr(FIRST, SEPARATOR - FIRST), ""};

    const auto LAST = arg.find_last_not_of(" \t");
    return {arg.substr(FIRST, SEPARATOR - FIRST), arg.substr(TARGET, LAST - TARGET + 1)};
}

static std::vector<PHLMONITOR> overviewTargetMonitors(const std::string& target) {
    if (target == "all")
        return State::monitorState()->monitors();

    if (!target.empty()) {
        const auto& MONITORS = State::monitorState()->monitors();
        const auto  IT       = std::ranges::find_if(MONITORS, [&target](const auto& monitor) { return monitor && monitor->m_name == target; });
        return IT == MONITORS.end() ? std::vector<PHLMONITOR>{} : std::vector<PHLMONITOR>{*IT};
    }

    const auto MONITOR = Desktop::focusState()->monitor();
    return MONITOR ? std::vector<PHLMONITOR>{MONITOR} : std::vector<PHLMONITOR>{};
}

static SP<IOverview> dispatcherOverview() {
    const auto CURRENTKEYBIND = g_pKeybindManager ? g_pKeybindManager->m_currentKeybind : SP<SKeybind>{};
    if (CURRENTKEYBIND && CURRENTKEYBIND->key.starts_with("mouse"))
        return g_pInputManager ? scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()) : SP<IOverview>{};

    return activeScrollOverview();
}

bool adoptNativeWindowDragIntoOverview() {
    if (!ScrollOverview::Config::getCrossMonitorDrag() || scrollOverviews().empty() || !g_layoutManager || !g_pInputManager)
        return false;

    const auto& DRAGCONTROLLER = g_layoutManager->dragController();
    const auto  TARGET         = DRAGCONTROLLER ? DRAGCONTROLLER->target() : nullptr;
    const auto  WINDOW         = TARGET ? TARGET->window() : PHLWINDOW{};
    if (!WINDOW || DRAGCONTROLLER->mode() != MBIND_MOVE || !DRAGCONTROLLER->dragThresholdReached())
        return false;

    const auto SOURCEWORKSPACE = TARGET->workspace();
    const auto SOURCEMONITOR   = SOURCEWORKSPACE ? SOURCEWORKSPACE->m_monitor.lock() : WINDOW->m_monitor.lock();
    if (!SOURCEMONITOR || !SOURCEMONITOR->m_enabled)
        return false;

    auto        result   = openOverview(SOURCEMONITOR);
    if (result.overview && result.overview->isClosing())
        result.overview->reopen();

    auto* const overview = result.overview ? dynamic_cast<CScrollOverview*>(result.overview.get()) : nullptr;
    if (!overview || !overview->adoptNativeWindowDrag(WINDOW)) {
        if (result.created && result.overview)
            result.overview->dismissTransient();
        return false;
    }

    result.overview->requestInputFrame();
    result.overview->damage();
    return true;
}

SOverviewOpenResult openOverview(PHLMONITOR monitor, bool swipe) {
    if (!monitor)
        return {};

    if (const auto OVERVIEW = scrollOverviewForMonitor(monitor))
        return {.overview = OVERVIEW, .created = false};

    if (!ensureScrollOverviewHooks())
        return {};

    const bool PREVRENDERINGOVERVIEW = renderingOverview;
    renderingOverview                = true;
    auto restoreRenderingOverview    = Hyprutils::Utils::CScopeGuard([PREVRENDERINGOVERVIEW] { renderingOverview = PREVRENDERINGOVERVIEW; });

    auto overview = makeShared<CScrollOverview>(monitor->m_activeWorkspace, swipe, monitor);
    registerScrollOverview(overview);
    reconcileNativeDragHook();
    return {.overview = overview, .created = true};
}

static SDispatchResult onOverviewDispatcher(std::string arg) {
    const auto [ACTION, TARGET] = splitOverviewArg(arg);

    if (ACTION == "select") {
        const auto OVERVIEW = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
        if (OVERVIEW && OVERVIEW->m_isSwiping)
            return {.success = false, .error = "already swiping"};
        if (OVERVIEW)
            OVERVIEW->selectHoveredWorkspace();
        return {};
    }

    const auto ACTIVE = dispatcherOverview();

    if (ACTIVE && ACTIVE->m_isSwiping)
        return {.success = false, .error = "already swiping"};

    if (ACTION == "off" || ACTION == "close" || ACTION == "disable") {
        if (TARGET.empty() || TARGET == "all") {
            closeAll();
            return {};
        }

        const auto MONITORS = overviewTargetMonitors(TARGET);
        if (MONITORS.empty())
            return {.success = false, .error = "monitor not found: " + TARGET};
        if (const auto overview = scrollOverviewForMonitor(MONITORS.front()))
            overview->close();
        return {};
    }

    if (ACTION != "toggle" && ACTION != "on" && ACTION != "open" && ACTION != "enable")
        return {.success = false, .error = "invalid arg. expected toggle|open|close|select [monitor|all]"};

    if (ACTION == "toggle" && TARGET.empty() && closeCrossMonitorDragSession())
        return {};

    const auto MONITORS = overviewTargetMonitors(TARGET);
    if (MONITORS.empty())
        return {.success = false, .error = TARGET.empty() ? "no active monitor" : "monitor not found: " + TARGET};

    const bool ALL_OPEN = std::ranges::all_of(MONITORS, [](const auto& monitor) { return !!scrollOverviewForMonitor(monitor); });
    if (ACTION == "toggle" && ALL_OPEN) {
        for (const auto& monitor : MONITORS) {
            if (const auto overview = scrollOverviewForMonitor(monitor)) {
                if (overview->isClosing())
                    overview->reopen();
                else
                    overview->close();
            }
        }
        return {};
    }

    for (const auto& monitor : MONITORS) {
        if (!openOverview(monitor).overview)
            return {.success = false, .error = "failed enabling overview hooks (is other overview plugin enabled?)"};
    }
    return {};
}

static SDispatchResult onNavigateDispatcher(std::string arg) {
    const auto OVERVIEW = dispatcherOverview();

    if (!OVERVIEW)
        return {};

    if (arg != "left" && arg != "right" && arg != "up" && arg != "down")
        return {.success = false, .error = "invalid arg. expected left|right|up|down"};

    OVERVIEW->moveSelection(arg);
    return {};
}

static SDispatchResult onWindowDispatcher(std::string arg) {
    const auto OVERVIEW = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
    if (!OVERVIEW)
        return {};

    if (arg != "select" && arg != "close")
        return {.success = false, .error = "invalid arg. expected select|close"};

    OVERVIEW->windowDispatcherAction(arg);
    return {};
}

static void failNotif(const std::string& reason) {
    HyprlandAPI::addNotification(SCROLLOVERVIEW_HANDLE, "[scrolloverview] Failure in initialization: " + reason, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
}

static void warnNativeDragUnavailable() {
    if (g_scrollMoveMouseHookWarned)
        return;

    g_scrollMoveMouseHookWarned = true;
    HyprlandAPI::addNotification(
        SCROLLOVERVIEW_HANDLE,
        "[scrolloverview] cross-monitor drag is enabled, but Hyprland drag adoption is unavailable; overview-origin cross-monitor dragging remains available",
        CHyprColor{1.0, 0.75, 0.2, 1.0}, 7500);
}

static void disableNativeDragHook() {
    if (!g_scrollMoveMouseHookActive)
        return;

    if (g_pScrollMoveMouseHook && g_pScrollMoveMouseHook->unhook())
        g_scrollMoveMouseHookActive = false;
}

static void* findOptionalFn(const std::string& name, const std::string_view demangledNeedle) {
    try {
        const auto FNS = HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, name);
        std::vector<SFunctionMatch> matches;
        matches.reserve(FNS.size());
        for (const auto& fn : FNS) {
            if (fn.demangled.find(demangledNeedle) != std::string::npos)
                matches.emplace_back(fn);
        }

        return matches.size() == 1 ? matches.front().address : nullptr;
    } catch (...) {
        return nullptr;
    }
}

static void reconcileNativeDragHook() {
    const bool REQUESTED = !g_unloading && g_scrollOverviewHooksActive && !scrollOverviews().empty() && ScrollOverview::Config::getCrossMonitorDrag();

    if (!REQUESTED) {
        disableNativeDragHook();
        return;
    }

    if (g_scrollMoveMouseHookActive)
        return;

    try {
        if (!g_pScrollMoveMouseHook && !g_scrollMoveMouseHookUnavailable) {
            const auto ADDRESS = findOptionalFn("moveMouse", "CLayoutManager::moveMouse(");
            if (ADDRESS)
                g_pScrollMoveMouseHook = HyprlandAPI::createFunctionHook(SCROLLOVERVIEW_HANDLE, ADDRESS, rc<void*>(hkMoveMouse));

            if (!g_pScrollMoveMouseHook)
                g_scrollMoveMouseHookUnavailable = true;
        }

        if (!g_scrollMoveMouseHookUnavailable && g_pScrollMoveMouseHook) {
            if (g_pScrollMoveMouseHook->hook()) {
                g_scrollMoveMouseHookActive = true;
                return;
            }

            g_scrollMoveMouseHookUnavailable = true;
        }
    } catch (...) {
        g_scrollMoveMouseHookUnavailable = true;
    }

    warnNativeDragUnavailable();
}

// Helper function to find a function by name and ensure it contains one of the requested substrings in its demangled name (to disambiguate overloads).
static void* findFnOrThrow(const std::string& name, std::initializer_list<std::string_view> demangledNeedles) {
    auto fns = HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, name);
    if (fns.empty()) {
        failNotif(std::format("no fns for hook {}", name));
        throw std::runtime_error(std::format("[scrolloverview] No fns for hook {}", name));
    }

    if (demangledNeedles.size() == 0 || (demangledNeedles.size() == 1 && demangledNeedles.begin()->empty()))
        return fns[0].address;

    std::vector<SFunctionMatch> matches;
    matches.reserve(fns.size());
    for (const auto& fn : fns) {
        for (const auto& needle : demangledNeedles) {
            if (needle.empty() || fn.demangled.find(needle) != std::string::npos) {
                matches.push_back(fn);
                break;
            }
        }
    }

    if (matches.empty()) {
        failNotif(std::format("no matching overload for hook {}", name));
        throw std::runtime_error(std::format("[scrolloverview] No matching overload for hook {}", name));
    }

    if (matches.size() > 1) {
        failNotif(std::format("ambiguous overload for hook {} ({} matches)", name, matches.size()));
        throw std::runtime_error(std::format("[scrolloverview] Ambiguous overload for hook {}", name));
    }

    return matches[0].address;
}

// shared core: register / unregister the overview trackpad gesture. used by both the hyprlang
// keyword and the Lua `scrolloverview.gesture` function so both configure the same gesture system.
static std::expected<void, std::string> applyOverviewGesture(size_t fingerCount, eTrackpadGestureDirection direction, const std::string& action, uint32_t modMask,
                                                             float deltaScale, bool disableInhibit) {
    if (fingerCount <= 1 || fingerCount >= 10)
        return std::unexpected(std::format("Invalid value {} for finger count", fingerCount));

    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return std::unexpected("Invalid direction");

    if (action == "overview")
        return g_pTrackpadGestures->addGesture(makeUnique<COverviewGesture>(), fingerCount, direction, modMask, deltaScale, disableInhibit);

    if (action == "unset")
        return g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);

    return std::unexpected(std::format("Invalid gesture: {}", action));
}

// Lua-facing registrar (scrolloverview.gesture). takes the direction/mods as strings and resolves
// them the same way the keyword does, then defers to applyOverviewGesture.
static SDispatchResult onRegisterOverviewGesture(size_t fingerCount, const std::string& directionStr, const std::string& action, const std::string& mods, float deltaScale,
                                                 bool disableInhibit) {
    if (g_unloading)
        return {};

    const auto direction = g_pTrackpadGestures->dirForString(directionStr);
    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return {.success = false, .error = std::format("Invalid direction: {}", directionStr)};

    uint32_t modMask = 0;
    if (!mods.empty() && g_pKeybindManager)
        modMask = g_pKeybindManager->stringToModMask(mods);

    const auto res = applyOverviewGesture(fingerCount, direction, action, modMask, std::clamp(deltaScale, 0.1F, 10.F), disableInhibit);
    if (!res)
        return {.success = false, .error = res.error()};

    return {};
}

static Hyprlang::CParseResult overviewGestureKeyword(const char* LHS, const char* RHS) {
    Hyprlang::CParseResult result;

    if (g_unloading)
        return result;

    CConstVarList             data(RHS);

    size_t                    fingerCount = 0;

    try {
        fingerCount = std::stoul(std::string{data[0]});
    } catch (...) {
        result.setError(std::format("Invalid value {} for finger count", data[0]).c_str());
        return result;
    }

    const auto direction = g_pTrackpadGestures->dirForString(data[1]);

    if (direction == TRACKPAD_GESTURE_DIR_NONE) {
        result.setError(std::format("Invalid direction: {}", data[1]).c_str());
        return result;
    }

    int      startDataIdx   = 2;
    uint32_t modMask        = 0;
    float    deltaScale     = 1.F;
    bool     disableInhibit = false;

    while (true) {

        if (data[startDataIdx].starts_with("mod:")) {
            modMask = g_pKeybindManager->stringToModMask(std::string{data[startDataIdx].substr(4)});
            startDataIdx++;
            continue;
        } else if (data[startDataIdx].starts_with("scale:")) {
            try {
                deltaScale = std::clamp(std::stof(std::string{data[startDataIdx].substr(6)}), 0.1F, 10.F);
                startDataIdx++;
                continue;
            } catch (...) {
                result.setError(std::format("Invalid delta scale: {}", std::string{data[startDataIdx].substr(6)}).c_str());
                return result;
            }
        } else if (data[startDataIdx] == "disable_inhibit") {
            disableInhibit = true;
            startDataIdx++;
            continue;
        }

        break;
    }

    const auto resultFromGesture = applyOverviewGesture(fingerCount, direction, std::string{data[startDataIdx]}, modMask, deltaScale, disableInhibit);

    if (!resultFromGesture)
        result.setError(resultFromGesture.error().c_str());

    return result;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    SCROLLOVERVIEW_HANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        failNotif("Version mismatch (headers ver is not equal to running hyprland ver)");
        throw std::runtime_error("[he] Version mismatch");
    }

    g_pScrollRenderWorkspaceHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("renderWorkspace", {"CHyprRenderer::renderWorkspace(", "IHyprRenderer::renderWorkspace("}),
        rc<void*>(hkRenderWorkspace));

    g_pScrollScheduleFrameHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE, 
        findFnOrThrow("_ZN7Monitor8CMonitor13scheduleFrameEN10Aquamarine7IOutput19scheduleFrameReasonE", {""}),
        rc<void*>(hkScheduleFrame));

    g_pScrollDamageSurfaceHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("damageSurface", {"CHyprRenderer::damageSurface(", "IHyprRenderer::damageSurface("}),
        rc<void*>(hkDamageSurface));

    g_pScrollSendFrameEventsHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("sendFrameEventsToWorkspace", {"CHyprRenderer::sendFrameEventsToWorkspace(", "IHyprRenderer::sendFrameEventsToWorkspace("}),
        rc<void*>(hkSendFrameEventsToWorkspace));

    g_pScrollSurfaceFrameHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("_ZN18CWLSurfaceResource5frameERKNSt6chrono10time_pointINS0_3_V212steady_clockENS0_8durationIlSt5ratioILl1ELl1000000000EEEEEE", {""}),
        rc<void*>(hkSurfaceFrame));

    g_pScrollAddDamageHookB = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("addDamageEPK15pixman_region32", {"CMonitor::addDamage"}),
        rc<void*>(hkAddDamageB));

    g_pScrollAddDamageHookA = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("_ZN7Monitor8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE", {""}),
        rc<void*>(hkAddDamageA));

    static auto P = Event::bus()->m_events.render.pre.listen([](PHLMONITOR monitor) {
        if (const auto overview = scrollOverviewForMonitor(monitor))
            overview->onPreRender();
    });

    g_configReloadHook = Event::bus()->m_events.config.reloaded.listen([] { reconcileNativeDragHook(); });

    ScrollOverview::Config::registerDispatcher("overview", ::onOverviewDispatcher);
    ScrollOverview::Config::registerDispatcher("navigate", ::onNavigateDispatcher);
    ScrollOverview::Config::registerDispatcher("window", ::onWindowDispatcher);
    ScrollOverview::Config::registerGesture(::onRegisterOverviewGesture, ::overviewGestureKeyword);
    ScrollOverview::Config::registerConfig();

    return {"scrolloverview", "A plugin for an overview", "Vaxry, yayuuu", SCROLLOVERVIEW_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pHyprRenderer->m_renderPass.removeAllOfType("CScrollOverviewPassElement");

    g_unloading = true;
    g_configReloadHook.reset();
    clearScrollOverviews();
    disableScrollOverviewHooks();

    if (g_pTrackpadGestures)
        g_pTrackpadGestures->clearGestures();

    HyprlandAPI::reloadConfig(); // re-adds built-in gestures cleared above
}
