#define WLR_USE_UNSTABLE

#include <unistd.h>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/input/trackpad/GestureTypes.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>

#include <hyprutils/string/ConstVarList.hpp>
using namespace Hyprutils::String;

#include "globals.hpp"
#include "scrollOverview.hpp"
#include "OverviewGesture.hpp"

// Methods
inline CFunctionHook* g_pRenderWorkspaceHook = nullptr;
inline CFunctionHook* g_pAddDamageHookA      = nullptr;
inline CFunctionHook* g_pAddDamageHookB      = nullptr;
inline CFunctionHook* g_pDamageSurfaceHook   = nullptr;
inline CFunctionHook* g_pScheduleFrameHook   = nullptr;
inline CFunctionHook* g_pSendFrameEventsHook = nullptr;
inline CFunctionHook* g_pSurfaceFrameHook     = nullptr;
typedef void (*origRenderWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, timespec*, const CBox&);
typedef void (*origAddDamageA)(void*, const CBox&);
typedef void (*origAddDamageB)(void*, const pixman_region32_t*);
typedef void (*origDamageSurface)(void*, SP<CWLSurfaceResource>, double, double, double);
typedef void (*origScheduleFrameForMonitor)(void*, PHLMONITOR, Aquamarine::IOutput::scheduleFrameReason);
typedef void (*origSendFrameEventsToWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);
typedef void (*origSurfaceFrame)(void*, const Time::steady_tp&);

static bool g_unloading = false;

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static bool renderingOverview = false;
static bool damageFromSurface = false;

static void hkScheduleFrameForMonitor(void* thisptr, PHLMONITOR monitor, Aquamarine::IOutput::scheduleFrameReason reason) {
    if (g_pOverview && g_pOverview->pMonitor == monitor) {
        using enum Aquamarine::IOutput::scheduleFrameReason;

        const bool THROTTLEDREASON =
            reason == AQ_SCHEDULE_UNKNOWN || reason == AQ_SCHEDULE_CLIENT_UNKNOWN || reason == AQ_SCHEDULE_NEEDS_FRAME || reason == AQ_SCHEDULE_RENDER_MONITOR ||
            reason == AQ_SCHEDULE_DAMAGE;

        if (THROTTLEDREASON && !g_pOverview->blockDamageReporting && !g_pOverview->shouldAllowRealtimePreviewSchedule())
            return;
    }

    ((origScheduleFrameForMonitor)g_pScheduleFrameHook->m_original)(thisptr, monitor, reason);
}

//
static void hkRenderWorkspace(void* thisptr, PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, timespec* now, const CBox& geometry) {
    if (!g_pOverview || renderingOverview || g_pOverview->blockOverviewRendering || g_pOverview->pMonitor != pMonitor)
        ((origRenderWorkspace)(g_pRenderWorkspaceHook->m_original))(thisptr, pMonitor, pWorkspace, now, geometry);
    else {
        const bool PREVRENDERINGOVERVIEW = renderingOverview;
        renderingOverview                = true;
        g_pOverview->render();
        renderingOverview = PREVRENDERINGOVERVIEW;
    }
}

static void hkDamageSurface(void* thisptr, SP<CWLSurfaceResource> surface, double x, double y, double scale) {
    if (!g_pOverview || g_pOverview->blockDamageReporting || g_pOverview->shouldHandleSurfaceDamage(surface)) {
        const bool PREVDAMAGEFROMSURFACE = damageFromSurface;
        damageFromSurface                = !!g_pOverview;
        ((origDamageSurface)g_pDamageSurfaceHook->m_original)(thisptr, surface, x, y, scale);
        damageFromSurface = PREVDAMAGEFROMSURFACE;
    }
}

static void hkSendFrameEventsToWorkspace(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
    if (g_pOverview && g_pOverview->pMonitor == monitor)
        return;

    ((origSendFrameEventsToWorkspace)g_pSendFrameEventsHook->m_original)(thisptr, monitor, workspace, now);
}

static void hkSurfaceFrame(void* thisptr, const Time::steady_tp& now) {
    const auto SURFACE = sc<CWLSurfaceResource*>(thisptr)->m_self.lock();

    if (g_pOverview && !g_pOverview->shouldAllowSurfaceFrame(SURFACE, now))
        return;

    ((origSurfaceFrame)g_pSurfaceFrameHook->m_original)(thisptr, now);
}

static void hkAddDamageA(void* thisptr, const CBox& box) {
    const auto PMONITOR = (CMonitor*)thisptr;

    if (g_pOverview && g_pOverview->pMonitor == PMONITOR->m_self && renderingOverview && !damageFromSurface && g_pOverview->shouldSuppressRenderDamage()) {
        return;
    }

    if (!g_pOverview || g_pOverview->pMonitor != PMONITOR->m_self || g_pOverview->blockDamageReporting || damageFromSurface) {
        ((origAddDamageA)g_pAddDamageHookA->m_original)(thisptr, box);
        return;
    }

    g_pOverview->onDamageReported();
    ((origAddDamageA)g_pAddDamageHookA->m_original)(thisptr, box);
}

static void hkAddDamageB(void* thisptr, const pixman_region32_t* rg) {
    const auto PMONITOR = (CMonitor*)thisptr;

    if (g_pOverview && g_pOverview->pMonitor == PMONITOR->m_self && renderingOverview && !damageFromSurface && g_pOverview->shouldSuppressRenderDamage()) {
        return;
    }

    if (!g_pOverview || g_pOverview->pMonitor != PMONITOR->m_self || g_pOverview->blockDamageReporting || damageFromSurface) {
        ((origAddDamageB)g_pAddDamageHookB->m_original)(thisptr, rg);
        return;
    }

    g_pOverview->onDamageReported();
    ((origAddDamageB)g_pAddDamageHookB->m_original)(thisptr, rg);
}

static SDispatchResult onOverviewDispatcher(std::string arg) {
    if (g_pOverview && g_pOverview->m_isSwiping)
        return {.success = false, .error = "already swiping"};

    if (arg == "select") {
        if (g_pOverview) {
            g_pOverview->selectHoveredWorkspace();
            g_pOverview->close();
        }
        return {};
    }
    if (arg == "toggle") {
        if (g_pOverview)
            g_pOverview->close();
        else {
            renderingOverview = true;
            g_pOverview = makeShared<CScrollOverview>(Desktop::focusState()->monitor()->m_activeWorkspace);
            renderingOverview = false;
        }
        return {};
    }

    if (arg == "off" || arg == "close" || arg == "disable") {
        if (g_pOverview)
            g_pOverview->close();
        return {};
    }

    if (g_pOverview)
        return {};

    renderingOverview = true;
    g_pOverview = makeShared<CScrollOverview>(Desktop::focusState()->monitor()->m_activeWorkspace);
    renderingOverview = false;
    return {};
}

static void failNotif(const std::string& reason) {
    HyprlandAPI::addNotification(PHANDLE, "[scrolloverview] Failure in initialization: " + reason, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
}

static Hyprlang::CParseResult overviewGestureKeyword(const char* LHS, const char* RHS) {
    Hyprlang::CParseResult result;

    if (g_unloading)
        return result;

    CConstVarList             data(RHS);

    size_t                    fingerCount = 0;
    eTrackpadGestureDirection direction   = TRACKPAD_GESTURE_DIR_NONE;

    try {
        fingerCount = std::stoul(std::string{data[0]});
    } catch (...) {
        result.setError(std::format("Invalid value {} for finger count", data[0]).c_str());
        return result;
    }

    if (fingerCount <= 1 || fingerCount >= 10) {
        result.setError(std::format("Invalid value {} for finger count", data[0]).c_str());
        return result;
    }

    direction = g_pTrackpadGestures->dirForString(data[1]);

    if (direction == TRACKPAD_GESTURE_DIR_NONE) {
        result.setError(std::format("Invalid direction: {}", data[1]).c_str());
        return result;
    }

    int      startDataIdx = 2;
    uint32_t modMask      = 0;
    float    deltaScale   = 1.F;

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
        }

        break;
    }

    std::expected<void, std::string> resultFromGesture;

    if (data[startDataIdx] == "overview")
        resultFromGesture = g_pTrackpadGestures->addGesture(makeUnique<COverviewGesture>(), fingerCount, direction, modMask, deltaScale, false);
    else if (data[startDataIdx] == "unset")
        resultFromGesture = g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, false);
    else {
        result.setError(std::format("Invalid gesture: {}", data[startDataIdx]).c_str());
        return result;
    }

    if (!resultFromGesture) {
        result.setError(resultFromGesture.error().c_str());
        return result;
    }

    return result;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        failNotif("Version mismatch (headers ver is not equal to running hyprland ver)");
        throw std::runtime_error("[he] Version mismatch");
    }

    auto FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "renderWorkspace");
    if (FNS.empty()) {
        failNotif("no fns for hook renderWorkspace");
        throw std::runtime_error("[he] No fns for hook renderWorkspace");
    }

    g_pRenderWorkspaceHook = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address, (void*)hkRenderWorkspace);

    FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "scheduleFrameForMonitor");
    if (FNS.empty()) {
        failNotif("no fns for hook scheduleFrameForMonitor");
        throw std::runtime_error("[he] No fns for hook scheduleFrameForMonitor");
    }

    g_pScheduleFrameHook = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address, (void*)hkScheduleFrameForMonitor);

    FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "damageSurface");
    if (FNS.empty()) {
        failNotif("no fns for hook damageSurface");
        throw std::runtime_error("[he] No fns for hook damageSurface");
    }

    g_pDamageSurfaceHook = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address, (void*)hkDamageSurface);

    FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "sendFrameEventsToWorkspace");
    if (FNS.empty()) {
        failNotif("no fns for hook sendFrameEventsToWorkspace");
        throw std::runtime_error("[he] No fns for hook sendFrameEventsToWorkspace");
    }

    g_pSendFrameEventsHook = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address, (void*)hkSendFrameEventsToWorkspace);

    FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "_ZN18CWLSurfaceResource5frameERKNSt6chrono10time_pointINS0_3_V212steady_clockENS0_8durationIlSt5ratioILl1ELl1000000000EEEEEE");
    if (FNS.empty()) {
        failNotif("no fns for hook CWLSurfaceResource::frame");
        throw std::runtime_error("[he] No fns for hook CWLSurfaceResource::frame");
    }

    g_pSurfaceFrameHook = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address, (void*)hkSurfaceFrame);

    FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "addDamageEPK15pixman_region32");
    if (FNS.empty()) {
        failNotif("no fns for hook addDamageEPK15pixman_region32");
        throw std::runtime_error("[he] No fns for hook addDamageEPK15pixman_region32");
    }

    g_pAddDamageHookB = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address, (void*)hkAddDamageB);

    FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "_ZN8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE");
    if (FNS.empty()) {
        failNotif("no fns for hook _ZN8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE");
        throw std::runtime_error("[he] No fns for hook _ZN8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE");
    }

    g_pAddDamageHookA = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address, (void*)hkAddDamageA);

    bool success = g_pRenderWorkspaceHook->hook();
    success      = success && g_pScheduleFrameHook->hook();
    success      = success && g_pDamageSurfaceHook->hook();
    success      = success && g_pSendFrameEventsHook->hook();
    success      = success && g_pSurfaceFrameHook->hook();
    success      = success && g_pAddDamageHookA->hook();
    success      = success && g_pAddDamageHookB->hook();

    if (!success) {
        failNotif("Failed initializing hooks");
        throw std::runtime_error("[he] Failed initializing hooks");
    }

    static auto P = Event::bus()->m_events.render.pre.listen([](PHLMONITOR monitor) {
        if (!g_pOverview || g_pOverview->pMonitor != monitor)
            return;
        g_pOverview->onPreRender();
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "scrolloverview:overview", ::onOverviewDispatcher);

    HyprlandAPI::addConfigKeyword(PHANDLE, "scrolloverview-gesture", ::overviewGestureKeyword, {});

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:scrolloverview:gesture_distance", Hyprlang::INT{200});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:scrolloverview:scale", Hyprlang::FLOAT{0.5F});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:scrolloverview:workspace_gap", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:scrolloverview:wallpaper", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:scrolloverview:blur", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:scrolloverview:shadow:enabled", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:scrolloverview:shadow:range", Hyprlang::INT{-1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:scrolloverview:shadow:render_power", Hyprlang::INT{-1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:scrolloverview:shadow:ignore_window", Hyprlang::INT{-1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:scrolloverview:shadow:color", Hyprlang::INT{-1});

    HyprlandAPI::reloadConfig();

    return {"scrolloverview", "A plugin for an overview", "Vaxry", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pHyprRenderer->m_renderPass.removeAllOfType("COverviewPassElement");

    g_unloading = true;
    g_pOverview.reset();

    g_pConfigManager->reload(); // we need to reload now to clear all the gestures
}
