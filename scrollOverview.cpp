#include "scrollOverview.hpp"
#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <linux/input-event-codes.h>
#define private public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/managers/animation/DesktopAnimationManager.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/managers/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/config/ConfigDataValues.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/PreBlurElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/decorations/CHyprGroupBarDecoration.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#undef private
#include "OverviewPassElement.hpp"

static void damageMonitor(WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) {
    g_pOverview->damage();
}

static void removeOverview(WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) {
    const auto PMONITOR = g_pOverview ? g_pOverview->pMonitor.lock() : nullptr;
    g_pOverview.reset();

    if (PMONITOR) {
        PMONITOR->recheckSolitary();
        g_pHyprRenderer->damageMonitor(PMONITOR);
    }
}

static xkb_keysym_t getOverviewKeysym(const IKeyboard::SKeyEvent& event) {
    const auto PKEYBOARD = g_pSeatManager->m_keyboard.lock();

    if (!PKEYBOARD)
        return XKB_KEY_NoSymbol;

    xkb_state* const STATE = PKEYBOARD->m_resolveBindsBySym && PKEYBOARD->m_xkbSymState ? PKEYBOARD->m_xkbSymState : PKEYBOARD->m_xkbState;

    if (!STATE)
        return XKB_KEY_NoSymbol;

    return xkb_state_key_get_one_sym(STATE, event.keycode + 8);
}

static bool isTopLayerFocused(PHLMONITOR monitor) {
    const auto FOCUSEDSURFACE = g_pSeatManager->m_state.keyboardFocus.lock();

    if (!FOCUSEDSURFACE)
        return false;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(FOCUSEDSURFACE);
    if (!HLSURFACE)
        return false;

    const auto VIEW = HLSURFACE->view();
    if (!VIEW)
        return false;

    auto layerOwner = Desktop::View::CLayerSurface::fromView(VIEW);

    if (!layerOwner) {
        const auto POPUP = Desktop::View::CPopup::fromView(VIEW);
        if (POPUP) {
            const auto T1OWNER = POPUP->getT1Owner();
            if (T1OWNER)
                layerOwner = Desktop::View::CLayerSurface::fromView(T1OWNER->view());
        }
    }

    return layerOwner && layerOwner->m_monitor == monitor && layerOwner->m_layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP;
}

static bool isPointerOnTopLayer(PHLMONITOR monitor) {
    if (!monitor)
        return false;

    const auto MOUSECOORDS = g_pInputManager->getMouseCoordsInternal();
    Vector2D   surfaceCoords;
    PHLLS      layerSurface;

    if (g_pCompositor->vectorToLayerSurface(MOUSECOORDS, &monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords, &layerSurface))
        return true;

    return !!g_pCompositor->vectorToLayerSurface(MOUSECOORDS, &monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &layerSurface);
}

static PHLWINDOW getOverviewWindowToShow(const PHLWINDOW& window) {
    if (!window)
        return nullptr;

    if (window->m_group)
        return window->m_group->current();

    return window;
}

static bool shouldShowOverviewWindow(const PHLWINDOW& window) {
    const auto WINDOW = getOverviewWindowToShow(window);

    if (!validMapped(WINDOW))
        return false;

    if (WINDOW->m_pinned && WINDOW->m_isFloating)
        return false;

    return true;
}

static bool shouldShowPinnedFloatingOverviewWindow(const PHLWINDOW& window) {
    const auto WINDOW = getOverviewWindowToShow(window);

    if (!validMapped(WINDOW))
        return false;

    if (!WINDOW->m_pinned || !WINDOW->m_isFloating)
        return false;

    return true;
}

static bool surfaceTreeHasFrameCallbacks(SP<CWLSurfaceResource> surface) {
    if (!surface)
        return false;

    bool hasCallbacks = false;
    surface->breadthfirst(
        [&hasCallbacks](SP<CWLSurfaceResource> child, const Vector2D&, void*) {
            if (!child || child->m_current.callbacks.empty())
                return;

            hasCallbacks = true;
        },
        nullptr);

    return hasCallbacks;
}

static void surfaceTreePresent(SP<CWLSurfaceResource> surface, PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!surface)
        return;

    std::pair<PHLMONITOR, Time::steady_tp> data = {monitor, now};
    surface->breadthfirst([](SP<CWLSurfaceResource> child, const Vector2D&, void* data) {
        if (!child)
            return;

        const auto [MONITOR, NOW] = *sc<std::pair<PHLMONITOR, Time::steady_tp>*>(data);
        child->presentFeedback(NOW, MONITOR, false);
    }, &data);
}

static bool windowHasOverviewAnimation(const PHLWINDOW& window) {
    if (!window)
        return false;

    return window->m_realPosition->isBeingAnimated() || window->m_realSize->isBeingAnimated() || window->m_alpha->isBeingAnimated() ||
        window->m_activeInactiveAlpha->isBeingAnimated() || window->m_movingFromWorkspaceAlpha->isBeingAnimated() || window->m_movingToWorkspaceAlpha->isBeingAnimated() ||
        window->m_borderFadeAnimationProgress->isBeingAnimated() || window->m_borderAngleAnimationProgress->isBeingAnimated() || window->m_dimPercent->isBeingAnimated() ||
        window->m_realShadowColor->isBeingAnimated();
}

static bool layerHasOverviewAnimation(const PHLLS& layer) {
    if (!Desktop::View::validMapped(layer))
        return false;

    return layer->m_realPosition->isBeingAnimated() || layer->m_realSize->isBeingAnimated() || layer->m_alpha->isBeingAnimated();
}

struct SSurfaceOpacityOverride {
    WP<CWLSurfaceResource> surface;
    float                  opacity = 1.F;
};

static void overrideSurfaceOpacity(std::vector<SSurfaceOpacityOverride>& overrides, SP<CWLSurfaceResource> surface, float opacity) {
    if (!surface)
        return;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
    if (!HLSURFACE)
        return;

    for (auto& entry : overrides) {
        if (entry.surface.lock() == surface) {
            HLSURFACE->m_overallOpacity = opacity;
            return;
        }
    }

    overrides.push_back({surface, HLSURFACE->m_overallOpacity});
    HLSURFACE->m_overallOpacity = opacity;
}

static void overrideWindowSurfaceOpacity(const PHLWINDOW& window, std::vector<SSurfaceOpacityOverride>& overrides, float opacity) {
    if (!window || !window->wlSurface() || !window->wlSurface()->resource())
        return;

    window->wlSurface()->resource()->breadthfirst(
        [&overrides, opacity](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { overrideSurfaceOpacity(overrides, surface, opacity); }, nullptr);

    if (window->m_isX11 || !window->m_popupHead)
        return;

    window->m_popupHead->breadthfirst([&overrides, opacity](WP<Desktop::View::CPopup> popup, void*) {
        if (!popup || !popup->aliveAndVisible() || !popup->wlSurface() || !popup->wlSurface()->resource())
            return;

        popup->wlSurface()->resource()->breadthfirst(
            [&overrides, opacity](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { overrideSurfaceOpacity(overrides, surface, opacity); }, nullptr);
    }, nullptr);
}

static void restoreSurfaceOpacityOverrides(std::vector<SSurfaceOpacityOverride>& overrides) {
    for (auto& entry : overrides) {
        const auto SURFACE = entry.surface.lock();
        if (!SURFACE)
            continue;

        const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(SURFACE);
        if (!HLSURFACE)
            continue;

        HLSURFACE->m_overallOpacity = entry.opacity;
    }

    overrides.clear();
}

static float getOverviewWindowTargetOpacity(const PHLWINDOW& window) {
    if (!window)
        return 1.F;

    static auto* const* PACTIVEOPACITY     = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "decoration:active_opacity")->getDataStaticPtr();
    static auto* const* PINACTIVEOPACITY   = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "decoration:inactive_opacity")->getDataStaticPtr();
    static auto* const* PFULLSCREENOPACITY = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "decoration:fullscreen_opacity")->getDataStaticPtr();

    const bool  fullscreen     = window->isFullscreen();
    const bool  active         = Desktop::focusState()->window() == window;
    float       targetOpacity  = fullscreen ? **PFULLSCREENOPACITY : active ? **PACTIVEOPACITY : **PINACTIVEOPACITY;
    const auto& ruleOpacityVar = fullscreen ? window->m_ruleApplicator->alphaFullscreen() : active ? window->m_ruleApplicator->alpha() : window->m_ruleApplicator->alphaInactive();

    targetOpacity = ruleOpacityVar.valueOr(Desktop::Types::SAlphaValue{}).applyAlpha(targetOpacity);

    return std::clamp(targetOpacity, 0.F, 1.F);
}

static CCssGapData getOverviewWindowHitboxGap() {
    static auto PGAPSIN = CConfigValue<Hyprlang::CUSTOMTYPE>("general:gaps_in");
    return *sc<CCssGapData*>((PGAPSIN.ptr())->getData());
}

static CBox getOverviewWindowBoxLogical(const PHLWINDOW& window, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float yoff) {
    const auto VIEWPORT_CENTER = CBox{{}, monitor->m_size}.middle();

    CBox       box            = {window->m_realPosition->value() - monitor->m_position, window->m_realSize->value()};
    box.translate(-VIEWPORT_CENTER).scale(scale).translate(VIEWPORT_CENTER).translate(-viewOffset * scale).translate({0.F, yoff});

    return box;
}

static CBox expandOverviewWindowHitbox(CBox box, float scale) {
    const auto GAPS = getOverviewWindowHitboxGap();
    constexpr float GAP_MULTIPLIER = 2.F;

    box.x -= sc<float>(std::max<int64_t>(0, GAPS.m_left)) * scale * GAP_MULTIPLIER;
    box.y -= sc<float>(std::max<int64_t>(0, GAPS.m_top)) * scale * GAP_MULTIPLIER;
    box.width += sc<float>(std::max<int64_t>(0, GAPS.m_left) + std::max<int64_t>(0, GAPS.m_right)) * scale * GAP_MULTIPLIER;
    box.height += sc<float>(std::max<int64_t>(0, GAPS.m_top) + std::max<int64_t>(0, GAPS.m_bottom)) * scale * GAP_MULTIPLIER;

    return box;
}

static float overviewPointDistanceSqToBox(const Vector2D& point, const CBox& box) {
    const float dx = point.x < box.x ? box.x - point.x : point.x > box.x + box.width ? point.x - (box.x + box.width) : 0.F;
    const float dy = point.y < box.y ? box.y - point.y : point.y > box.y + box.height ? point.y - (box.y + box.height) : 0.F;

    return dx * dx + dy * dy;
}

static CBox getOverviewWindowBox(const PHLWINDOW& window, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float yoff) {
    CBox box = getOverviewWindowBoxLogical(window, monitor, scale, viewOffset, yoff);
    box.scale(monitor->m_scale).round();

    return box;
}

static CBox getOverviewWorkspaceBoxLogical(PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float yoff) {
    const auto VIEWPORT_CENTER = CBox{{}, monitor->m_size}.middle();

    CBox       box            = {{}, monitor->m_size};
    box.translate(-VIEWPORT_CENTER).scale(scale).translate(VIEWPORT_CENTER).translate(-viewOffset * scale).translate({0.F, yoff});

    return box;
}

static CBox getOverviewWorkspaceBox(PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float yoff) {
    CBox box = getOverviewWorkspaceBoxLogical(monitor, scale, viewOffset, yoff);
    box.scale(monitor->m_scale).round();

    return box;
}

static CBox getWorkspaceGlobalBox(PHLWORKSPACE workspace, PHLMONITOR fallbackMonitor) {
    const auto MONITOR = workspace && workspace->m_monitor ? workspace->m_monitor.lock() : fallbackMonitor;
    if (!MONITOR)
        return {};

    return {MONITOR->m_position, MONITOR->m_size};
}

static CBox centerBoxInWorkspace(CBox box, PHLWORKSPACE workspace, PHLMONITOR fallbackMonitor) {
    const auto WORKSPACEBOX = getWorkspaceGlobalBox(workspace, fallbackMonitor);
    if (WORKSPACEBOX.width <= 0 || WORKSPACEBOX.height <= 0)
        return box;

    box.x = WORKSPACEBOX.x + std::max(0.F, sc<float>(WORKSPACEBOX.width - box.width)) / 2.F;
    box.y = WORKSPACEBOX.y + std::max(0.F, sc<float>(WORKSPACEBOX.height - box.height)) / 2.F;

    return box;
}

static CBox clampBoxToWorkspace(CBox box, PHLWORKSPACE workspace, PHLMONITOR fallbackMonitor) {
    const auto WORKSPACEBOX = getWorkspaceGlobalBox(workspace, fallbackMonitor);
    if (WORKSPACEBOX.width <= 0 || WORKSPACEBOX.height <= 0)
        return box;

    const float MINX = WORKSPACEBOX.x;
    const float MINY = WORKSPACEBOX.y;
    const float MAXX = WORKSPACEBOX.x + std::max(0.F, sc<float>(WORKSPACEBOX.width - box.width));
    const float MAXY = WORKSPACEBOX.y + std::max(0.F, sc<float>(WORKSPACEBOX.height - box.height));

    box.x = std::clamp(sc<float>(box.x), MINX, std::max(MINX, MAXX));
    box.y = std::clamp(sc<float>(box.y), MINY, std::max(MINY, MAXY));

    return box;
}

static bool overviewBoxIntersectsMonitor(const CBox& box, PHLMONITOR monitor) {
    if (!monitor || box.width <= 0 || box.height <= 0)
        return false;

    const auto RENDERSIZE = monitor->m_size * monitor->m_scale;

    return box.x < RENDERSIZE.x && box.x + box.width > 0 && box.y < RENDERSIZE.y && box.y + box.height > 0;
}

static int getWorkspaceGap() {
    static auto* const* PGAP = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:workspace_gap")->getDataStaticPtr();
    return std::max<int>(0, **PGAP);
}

static double overviewBoxIntersectionArea(const CBox& a, const CBox& b) {
    const auto INTERSECTION = a.intersection(b);
    return std::max(0.0, INTERSECTION.width) * std::max(0.0, INTERSECTION.height);
}

static CBox getPinnedFloatingOverviewWindowBox(PHLMONITOR monitor, const PHLWINDOW& window, float targetOverviewScale, float animationProgress, float* renderScale) {
    if (!monitor || !window) {
        if (renderScale)
            *renderScale = 1.F;
        return {};
    }

    const auto WINDOWSIZE = window->m_realSize->value();
    if (WINDOWSIZE.x <= 0 || WINDOWSIZE.y <= 0) {
        if (renderScale)
            *renderScale = 1.F;
        return {};
    }

    const CBox WINDOWBOX = {window->m_realPosition->value() - monitor->m_position, WINDOWSIZE};
    const auto MONITORW  = sc<float>(monitor->m_size.x);
    const auto MONITORH  = sc<float>(monitor->m_size.y);

    const std::array<CBox, 4> QUADRANTS = {
        CBox{0.F, 0.F, MONITORW / 2.F, MONITORH / 2.F},
        CBox{MONITORW / 2.F, 0.F, MONITORW / 2.F, MONITORH / 2.F},
        CBox{0.F, MONITORH / 2.F, MONITORW / 2.F, MONITORH / 2.F},
        CBox{MONITORW / 2.F, MONITORH / 2.F, MONITORW / 2.F, MONITORH / 2.F},
    };

    size_t bestQuadrant = 0;
    double bestArea     = -1.0;
    for (size_t i = 0; i < QUADRANTS.size(); ++i) {
        const auto AREA = overviewBoxIntersectionArea(WINDOWBOX, QUADRANTS[i]);
        if (AREA <= bestArea)
            continue;

        bestQuadrant = i;
        bestArea     = AREA;
    }

    const bool RIGHT  = bestQuadrant == 1 || bestQuadrant == 3;
    const bool BOTTOM = bestQuadrant == 2 || bestQuadrant == 3;

    const auto FULLBOX = monitor->logicalBox();
    const auto WORKBOX = monitor->logicalBoxMinusReserved();

    const float RESERVEDLEFT   = std::max(0.F, sc<float>(WORKBOX.x - FULLBOX.x));
    const float RESERVEDTOP    = std::max(0.F, sc<float>(WORKBOX.y - FULLBOX.y));
    const float RESERVEDRIGHT  = std::max(0.F, sc<float>((FULLBOX.x + FULLBOX.width) - (WORKBOX.x + WORKBOX.width)));
    const float RESERVEDBOTTOM = std::max(0.F, sc<float>((FULLBOX.y + FULLBOX.height) - (WORKBOX.y + WORKBOX.height)));

    const auto WORKSPACEGAP      = sc<float>(getWorkspaceGap());
    const auto RESERVEDWIDTH     = RIGHT ? RESERVEDRIGHT : RESERVEDLEFT;
    const auto CALCULATEDWIDTH   = std::max(1.F, sc<float>((MONITORW - MONITORW * targetOverviewScale) / 2.F - 2.F * WORKSPACEGAP - RESERVEDWIDTH));
    const auto CALCULATEDSCALE   = CALCULATEDWIDTH / sc<float>(WINDOWSIZE.x);
    const auto WINDOWRENDERSCALE = std::min(1.F, std::max(CALCULATEDSCALE, targetOverviewScale));
    const auto PROGRESS          = std::clamp(animationProgress, 0.F, 1.F);
    const auto CURRENTRENDERSCALE = 1.F + (WINDOWRENDERSCALE - 1.F) * PROGRESS;
    const auto TARGETWIDTH       = sc<float>(WINDOWSIZE.x) * CURRENTRENDERSCALE;
    const auto TARGETHEIGHT      = sc<float>(WINDOWSIZE.y) * CURRENTRENDERSCALE;

    if (renderScale)
        *renderScale = CURRENTRENDERSCALE;

    const float X = RIGHT ? MONITORW - TARGETWIDTH - WORKSPACEGAP - RESERVEDRIGHT : WORKSPACEGAP + RESERVEDLEFT;
    const float Y = BOTTOM ? MONITORH - TARGETHEIGHT - WORKSPACEGAP - RESERVEDBOTTOM : WORKSPACEGAP + RESERVEDTOP;

    CBox box = {{X, Y}, {TARGETWIDTH, TARGETHEIGHT}};
    box.scale(monitor->m_scale);

    const CBox ORIGINALBOX = {WINDOWBOX.pos() * monitor->m_scale, WINDOWBOX.size() * monitor->m_scale};
    box.x                  = ORIGINALBOX.x + (box.x - ORIGINALBOX.x) * PROGRESS;
    box.y                  = ORIGINALBOX.y + (box.y - ORIGINALBOX.y) * PROGRESS;
    box.round();

    return box;
}

static int getWallpaperMode() {
    static auto* const* PMODE = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:wallpaper")->getDataStaticPtr();
    return std::clamp<int>(**PMODE, 0, 2);
}

static bool getOverviewBlur() {
    static auto* const* PBLUR = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:blur")->getDataStaticPtr();
    return **PBLUR;
}

static std::chrono::milliseconds getOverviewIdleFrameInterval() {
    static auto* const* PFPS = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "misc:render_unfocused_fps")->getDataStaticPtr();

    const int fps = std::clamp<int>(**PFPS, 1, 240);
    return std::chrono::milliseconds(std::max(1, 1000 / fps));
}

static constexpr std::chrono::milliseconds OVERVIEW_WINDOW_FRAME_INTERVAL = std::chrono::milliseconds(33);

static bool getHyprlandBlurNewOptimizations() {
    static auto* const* PNEWOPTIMIZATIONS = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "decoration:blur:new_optimizations")->getDataStaticPtr();
    return **PNEWOPTIMIZATIONS;
}

static int getHyprlandDecorationRounding() {
    static auto* const* PROUNDING = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "decoration:rounding")->getDataStaticPtr();
    return std::max<int>(0, **PROUNDING);
}

static float getHyprlandDecorationRoundingPower() {
    static auto* const* PROUNDINGPOWER = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "decoration:rounding_power")->getDataStaticPtr();
    return **PROUNDINGPOWER;
}

static float getOverviewConfiguredScale() {
    static auto* const* PSCALE = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:scale")->getDataStaticPtr();

    const float configuredScale = **PSCALE;

    return std::clamp(configuredScale, 0.1F, 0.9F);
}

struct SOverviewShadowConfig {
    bool       enabled     = false;
    int        range       = 0;
    int        renderPower = 1;
    bool       ignoreWindow = true;
    CHyprColor color       = CHyprColor{0, 0, 0, 0};
};

static SOverviewShadowConfig getOverviewShadowConfig() {
    static auto* const* PENABLED     = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:shadow:enabled")->getDataStaticPtr();
    static auto* const* PRANGE       = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:shadow:range")->getDataStaticPtr();
    static auto* const* PRENDERPOWER = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:shadow:render_power")->getDataStaticPtr();
    static auto* const* PIGNORE      = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:shadow:ignore_window")->getDataStaticPtr();
    static auto* const* PCOLOR       = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:shadow:color")->getDataStaticPtr();

    static auto* const* PGLOBALRANGE       = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "decoration:shadow:range")->getDataStaticPtr();
    static auto* const* PGLOBALRENDERPOWER = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "decoration:shadow:render_power")->getDataStaticPtr();
    static auto* const* PGLOBALIGNORE      = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "decoration:shadow:ignore_window")->getDataStaticPtr();
    static auto* const* PGLOBALCOLOR       = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "decoration:shadow:color")->getDataStaticPtr();

    const int range       = **PRANGE >= 0 ? **PRANGE : **PGLOBALRANGE;
    const int renderPower = **PRENDERPOWER >= 0 ? **PRENDERPOWER : **PGLOBALRENDERPOWER;
    const int ignore      = **PIGNORE >= 0 ? **PIGNORE : **PGLOBALIGNORE;
    const auto color      = **PCOLOR >= 0 ? **PCOLOR : **PGLOBALCOLOR;

    return {
        .enabled      = !!**PENABLED,
        .range        = std::max(0, range),
        .renderPower  = std::clamp(renderPower, 1, 4),
        .ignoreWindow = !!ignore,
        .color        = CHyprColor(color),
    };
}

static float getWorkspaceRenderedPitch(PHLMONITOR monitor, float scale) {
    return monitor->m_size.y * scale + sc<float>(getWorkspaceGap());
}

static float getWorkspaceLogicalPitch(PHLMONITOR monitor, float scale) {
    const auto safeScale = std::max(scale, 0.01F);
    return monitor->m_size.y + sc<float>(getWorkspaceGap()) / safeScale;
}

static float getOverviewVerticalOverlap(const PHLWINDOW& a, const PHLWINDOW& b) {
    if (!a || !b)
        return 0.F;

    const auto APOS  = a->m_realPosition->value();
    const auto ASIZE = a->m_realSize->value();
    const auto BPOS  = b->m_realPosition->value();
    const auto BSIZE = b->m_realSize->value();

    const double overlap = std::min(APOS.y + ASIZE.y, BPOS.y + BSIZE.y) - std::max(APOS.y, BPOS.y);

    return std::max(0.F, sc<float>(overlap));
}

static float getOverviewHorizontalOverlap(const PHLWINDOW& a, const PHLWINDOW& b) {
    if (!a || !b)
        return 0.F;

    const auto APOS  = a->m_realPosition->value();
    const auto ASIZE = a->m_realSize->value();
    const auto BPOS  = b->m_realPosition->value();
    const auto BSIZE = b->m_realSize->value();

    const double overlap = std::min(APOS.x + ASIZE.x, BPOS.x + BSIZE.x) - std::max(APOS.x, BPOS.x);

    return std::max(0.F, sc<float>(overlap));
}

static bool overviewBoxesEqual(const CBox& a, const CBox& b) {
    return std::abs(a.x - b.x) < 0.5 && std::abs(a.y - b.y) < 0.5 && std::abs(a.width - b.width) < 0.5 && std::abs(a.height - b.height) < 0.5;
}

static bool moveOverviewScrollingTargetToHorizontalEdge(const SP<Layout::ITarget>& target, int side);
static bool moveOverviewScrollingTargetNextToWindow(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction);

static void moveOverviewTargetToHorizontalEdge(const SP<Layout::ITarget>& target, int side) {
    if (!target || side == 0 || !target->space())
        return;

    if (moveOverviewScrollingTargetToHorizontalEdge(target, side))
        return;

    static auto* const* PFALLBACK = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "binds:window_direction_monitor_fallback")->getDataStaticPtr();

    const int PREVFALLBACK = **PFALLBACK;
    **PFALLBACK            = 0;
    auto restoreFallback   = Hyprutils::Utils::CScopeGuard([PREVFALLBACK] {
        static auto* const* PFALLBACK = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "binds:window_direction_monitor_fallback")->getDataStaticPtr();
        **PFALLBACK                  = PREVFALLBACK;
    });

    const std::string DIRECTION = side < 0 ? "l" : "r";

    for (size_t i = 0; i < 64; ++i) {
        const auto WORKSPACE = target->workspace();
        const auto BEFORE    = target->position();

        g_layoutManager->moveInDirection(target, DIRECTION, true);

        if (target->workspace() != WORKSPACE || overviewBoxesEqual(BEFORE, target->position()))
            break;
    }
}

static double overviewTargetDirectionDelta(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction) {
    if (!target || !anchor || !anchor->layoutTarget())
        return 0.0;

    const auto TARGETCENTER = target->position().middle();
    const auto ANCHORCENTER = anchor->layoutTarget()->position().middle();

    if (direction == "l" || direction == "r")
        return TARGETCENTER.x - ANCHORCENTER.x;

    return TARGETCENTER.y - ANCHORCENTER.y;
}

static void moveOverviewTargetOneStep(const SP<Layout::ITarget>& target, const std::string& direction) {
    if (!target || direction.empty())
        return;

    g_layoutManager->moveInDirection(target, direction, true);
}

static Layout::Tiled::CScrollingAlgorithm* overviewScrollingAlgorithmForTarget(const SP<Layout::ITarget>& target) {
    if (!target || !target->space() || !target->space()->algorithm())
        return nullptr;

    return dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(target->space()->algorithm()->m_tiled.get());
}

static bool moveOverviewScrollingTargetToHorizontalEdge(const SP<Layout::ITarget>& target, int side) {
    if (!target || side == 0)
        return false;

    const auto ALGO = overviewScrollingAlgorithmForTarget(target);
    if (!ALGO || !ALGO->m_scrollingData)
        return false;

    const auto TDATA = ALGO->dataFor(target);
    if (!TDATA)
        return false;

    const auto SRC_COL = TDATA->column.lock();
    if (!SRC_COL)
        return false;

    SRC_COL->remove(target);

    const int64_t INSERT_AFTER = side < 0 ? -1 : sc<int64_t>(ALGO->m_scrollingData->columns.size()) - 1;
    const auto    NEW_COL      = ALGO->m_scrollingData->add(INSERT_AFTER);
    NEW_COL->add(TDATA);
    ALGO->m_scrollingData->centerOrFitCol(NEW_COL);
    ALGO->m_scrollingData->recalculate();
    ALGO->focusTargetUpdate(target);

    return true;
}

static bool moveOverviewScrollingTargetNextToWindow(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction) {
    if (!target || !anchor || !anchor->layoutTarget() || direction.empty())
        return false;

    const auto ALGO = overviewScrollingAlgorithmForTarget(target);
    if (!ALGO || !ALGO->m_scrollingData)
        return false;

    const auto TDATA      = ALGO->dataFor(target);
    const auto ANCHORDATA = ALGO->dataFor(anchor->layoutTarget());
    if (!TDATA || !ANCHORDATA)
        return false;

    const auto SRC_COL    = TDATA->column.lock();
    const auto ANCHOR_COL = ANCHORDATA->column.lock();
    if (!SRC_COL || !ANCHOR_COL)
        return false;

    if (direction == "l" || direction == "r") {
        SRC_COL->remove(target);

        const auto ANCHOR_COL_IDX = ALGO->m_scrollingData->idx(ANCHOR_COL);
        if (ANCHOR_COL_IDX < 0)
            return false;

        const int64_t INSERT_AFTER = direction == "l" ? ANCHOR_COL_IDX - 1 : ANCHOR_COL_IDX;
        const auto    NEW_COL      = ALGO->m_scrollingData->add(INSERT_AFTER);
        NEW_COL->add(TDATA);
        ALGO->m_scrollingData->centerOrFitCol(NEW_COL);
        ALGO->m_scrollingData->recalculate();
        ALGO->focusTargetUpdate(target);

        return true;
    }

    if (direction != "u" && direction != "d")
        return false;

    SRC_COL->remove(target);

    const auto ANCHOR_IDX = ANCHOR_COL->idx(anchor->layoutTarget());
    const int  INSERT_AFTER = direction == "u" ? sc<int>(ANCHOR_IDX) - 1 : sc<int>(ANCHOR_IDX);
    ANCHOR_COL->add(TDATA, INSERT_AFTER);
    ALGO->m_scrollingData->centerOrFitCol(ANCHOR_COL);
    ALGO->m_scrollingData->recalculate();
    ALGO->focusTargetUpdate(target);

    return true;
}

static void moveOverviewTargetNextToWindow(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction) {
    if (!target || !anchor || direction.empty())
        return;

    if (moveOverviewScrollingTargetNextToWindow(target, anchor, direction))
        return;

    static auto* const* PFALLBACK = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "binds:window_direction_monitor_fallback")->getDataStaticPtr();

    const int PREVFALLBACK = **PFALLBACK;
    **PFALLBACK            = 0;
    auto restoreFallback   = Hyprutils::Utils::CScopeGuard([PREVFALLBACK] {
        static auto* const* PFALLBACK = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "binds:window_direction_monitor_fallback")->getDataStaticPtr();
        **PFALLBACK                  = PREVFALLBACK;
    });

    const bool WANT_NEGATIVE = direction == "l" || direction == "u";
    const auto FORWARD       = direction;
    const auto BACKWARD      = direction == "l" ? "r" : direction == "r" ? "l" : direction == "u" ? "d" : "u";

    auto isDesiredSide = [&] {
        const auto DELTA = overviewTargetDirectionDelta(target, anchor, direction);
        return WANT_NEGATIVE ? DELTA < 0.0 : DELTA > 0.0;
    };

    for (size_t i = 0; i < 64 && isDesiredSide(); ++i) {
        const auto WORKSPACE = target->workspace();
        const auto BEFORE    = target->position();

        moveOverviewTargetOneStep(target, BACKWARD);

        if (target->workspace() != WORKSPACE || overviewBoxesEqual(BEFORE, target->position()))
            break;

        if (!isDesiredSide()) {
            moveOverviewTargetOneStep(target, FORWARD);
            return;
        }
    }

    for (size_t i = 0; i < 64 && !isDesiredSide(); ++i) {
        const auto WORKSPACE = target->workspace();
        const auto BEFORE    = target->position();

        moveOverviewTargetOneStep(target, FORWARD);

        if (target->workspace() != WORKSPACE || overviewBoxesEqual(BEFORE, target->position()))
            break;
    }
}

static void renderOverviewPass(PHLMONITOR monitor) {
    if (!monitor || g_pHyprRenderer->m_renderPass.empty())
        return;

    g_pHyprRenderer->m_renderPass.render(CRegion{CBox{{}, monitor->m_size}});
    g_pHyprRenderer->m_renderPass.clear();
}

static CRegion roundedRectRegion(const CBox& box, int rounding, float roundingPower) {
    const auto ROUNDEDBOX = box.copy().round();
    const int  x          = sc<int>(ROUNDEDBOX.x);
    const int  y          = sc<int>(ROUNDEDBOX.y);
    const int  w          = sc<int>(ROUNDEDBOX.width);
    const int  h          = sc<int>(ROUNDEDBOX.height);

    if (w <= 0 || h <= 0)
        return {};

    const int radius = std::clamp(rounding, 0, std::min(w, h) / 2);
    if (radius <= 0)
        return CRegion{ROUNDEDBOX};

    CRegion    region;
    const auto power = std::max(roundingPower, 0.1F);

    auto insetForRow = [radius, h, power](int row) {
        const double rowCenter = row + 0.5;
        double       dy        = 0.0;

        if (rowCenter < radius)
            dy = radius - rowCenter;
        else if (rowCenter > h - radius)
            dy = rowCenter - (h - radius);

        if (dy <= 0.0)
            return 0;

        const double radiusPow = std::pow(radius, power);
        const double inner     = std::pow(std::max(0.0, radiusPow - std::pow(dy, power)), 1.0 / power);
        return std::clamp(sc<int>(std::ceil(radius - inner)), 0, radius);
    };

    int runStart = 0;
    int runInset = insetForRow(0);

    auto addRun = [&](int from, int to, int inset) {
        if (to <= from)
            return;

        const int width = w - inset * 2;
        if (width <= 0)
            return;

        region.add(CBox{x + inset, y + from, width, to - from});
    };

    for (int row = 1; row < h; ++row) {
        const int inset = insetForRow(row);
        if (inset == runInset)
            continue;

        addRun(runStart, row, runInset);
        runStart = row;
        runInset = inset;
    }

    addRun(runStart, h, runInset);

    return region;
}

class COverviewWindowShadowPassElement : public IPassElement {
  public:
    struct SData {
        PHLMONITORREF monitor;
        CBox          fullBox;
        CBox          cutoutBox;
        int           rounding      = 0;
        float         roundingPower = 2.F;
        int           range         = 0;
        int           renderPower   = 0;
        CHyprColor    color;
        float         alpha        = 1.F;
        bool          ignoreWindow = true;
        bool          sharp        = false;
    };

    COverviewWindowShadowPassElement(const SData& data_) : data(data_) {
        ;
    }

    virtual void draw(const CRegion& damage) {
        if (!data.monitor || data.fullBox.width < 1 || data.fullBox.height < 1 || data.range <= 0 || data.color.a == 0.F || data.alpha <= 0.F)
            return;

        CRegion shadowDamage = damage.copy().intersect(data.fullBox);
        if (data.ignoreWindow)
            shadowDamage.subtract(roundedRectRegion(data.cutoutBox, data.rounding + 1, data.roundingPower));

        if (shadowDamage.empty())
            return;

        const auto SAVEDDAMAGE = g_pHyprOpenGL->m_renderData.damage;
        g_pHyprOpenGL->m_renderData.damage = shadowDamage;
        auto restoreDamage = Hyprutils::Utils::CScopeGuard([SAVEDDAMAGE] { g_pHyprOpenGL->m_renderData.damage = SAVEDDAMAGE; });

        auto color = data.color;
        color.a *= data.alpha;

        static auto* const* PGLOBALRENDERPOWER = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "decoration:shadow:render_power")->getDataStaticPtr();

        const auto PREVRENDERPOWER = data.renderPower > 0 ? std::optional<int>(**PGLOBALRENDERPOWER) : std::nullopt;
        if (data.renderPower > 0)
            **PGLOBALRENDERPOWER = data.renderPower;
        auto restoreRenderPower = Hyprutils::Utils::CScopeGuard([PREVRENDERPOWER] {
            if (PREVRENDERPOWER)
                **PGLOBALRENDERPOWER = *PREVRENDERPOWER;
        });

        if (data.sharp)
            g_pHyprOpenGL->renderRect(data.fullBox, color, {.damage = &shadowDamage, .round = data.rounding, .roundingPower = data.roundingPower});
        else
            g_pHyprOpenGL->renderRoundedShadow(data.fullBox, data.rounding, data.roundingPower, data.range, color, 1.F);
    }

    virtual bool needsLiveBlur() {
        return false;
    }

    virtual bool needsPrecomputeBlur() {
        return false;
    }

    virtual std::optional<CBox> boundingBox() {
        const auto MONITOR = data.monitor.lock();
        if (!MONITOR)
            return std::nullopt;

        return data.fullBox.copy().scale(1.F / MONITOR->m_scale).round();
    }

    virtual CRegion opaqueRegion() {
        return {};
    }

    virtual const char* passName() {
        return "COverviewWindowShadowPassElement";
    }

  private:
    SData data;
};

static void roundStandaloneWindowPassElements(const PHLWINDOW& window, PHLMONITOR monitor, float renderScale, size_t firstElement) {
    if (!window || !monitor)
        return;

    const int   rounding      = sc<int>(std::round(window->rounding() * monitor->m_scale * renderScale));
    const float roundingPower = window->roundingPower();

    if (rounding <= 0)
        return;

    auto& passElements = g_pHyprRenderer->m_renderPass.m_passElements;
    for (size_t i = firstElement; i < passElements.size(); ++i) {
        const auto& passElement = passElements[i];
        if (!passElement || !passElement->element)
            continue;

        auto* surfacePassElement = dynamic_cast<CSurfacePassElement*>(passElement->element.get());
        if (!surfacePassElement || surfacePassElement->m_data.pWindow != window || surfacePassElement->m_data.popup)
            continue;

        surfacePassElement->m_data.dontRound     = false;
        surfacePassElement->m_data.rounding      = rounding;
        surfacePassElement->m_data.roundingPower = roundingPower;
    }
}

static void renderOverviewWindowShadow(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, float renderScale, bool selected) {
    if (!monitor || !window || (!window->m_isMapped && !window->m_fadingOut))
        return;

    static auto PSHADOWS            = CConfigValue<Hyprlang::INT>("decoration:shadow:enabled");
    static auto PSHADOWSIZE         = CConfigValue<Hyprlang::INT>("decoration:shadow:range");
    static auto PSHADOWSHARP        = CConfigValue<Hyprlang::INT>("decoration:shadow:sharp");
    static auto PSHADOWIGNOREWINDOW = CConfigValue<Hyprlang::INT>("decoration:shadow:ignore_window");
    static auto PSHADOWSCALE        = CConfigValue<Hyprlang::FLOAT>("decoration:shadow:scale");
    static auto PSHADOWOFFSET       = CConfigValue<Hyprlang::VEC2>("decoration:shadow:offset");
    static auto PSHADOWCOL          = CConfigValue<Hyprlang::INT>("decoration:shadow:color");
    static auto PSHADOWCOLINACTIVE  = CConfigValue<Hyprlang::INT>("decoration:shadow:color_inactive");

    if (*PSHADOWS != 1 || *PSHADOWSIZE <= 0)
        return;

    if (window->isX11OverrideRedirect() || window->m_X11DoesntWantBorders || !window->m_ruleApplicator->decorate().valueOrDefault() ||
        window->m_ruleApplicator->noShadow().valueOrDefault())
        return;

    const auto  borderSize       = window->getRealBorderSize();
    const auto  roundingBase     = window->rounding();
    const auto  roundingPower    = window->roundingPower();
    const auto  correctionOffset = (borderSize * (M_SQRT2 - 1) * std::max(2.0 - roundingPower, 0.0));
    const auto  outerRound       = roundingBase > 0 ? (roundingBase + borderSize) - correctionOffset : 0;
    const int   borderPx         = sc<int>(std::round(borderSize * monitor->m_scale));
    const int   rangePx          = sc<int>(std::round(*PSHADOWSIZE * monitor->m_scale * renderScale));
    const int   roundingPx       = sc<int>(std::round(outerRound * monitor->m_scale * renderScale));
    const float shadowScale      = std::clamp(*PSHADOWSCALE, 0.F, 1.F);
    const auto  shadowOffset     = Vector2D{(*PSHADOWOFFSET).x, (*PSHADOWOFFSET).y} * monitor->m_scale * renderScale;

    if (rangePx <= 0)
        return;

    CBox outerBorderBox = windowBox.copy().expand(borderPx);
    CBox shadowBox      = outerBorderBox.copy().expand(rangePx).scaleFromCenter(shadowScale).translate(shadowOffset);
    shadowBox.round();

    if (shadowBox.width < 1 || shadowBox.height < 1)
        return;

    const auto shadowColor = CHyprColor(selected ? *PSHADOWCOL : *PSHADOWCOLINACTIVE != -1 ? *PSHADOWCOLINACTIVE : *PSHADOWCOL);
    if (shadowColor.a == 0.F)
        return;

    COverviewWindowShadowPassElement::SData data;
    data.monitor       = monitor;
    data.fullBox       = shadowBox;
    data.cutoutBox     = outerBorderBox;
    data.rounding      = roundingPx;
    data.roundingPower = roundingPower;
    data.range         = rangePx;
    data.color         = shadowColor;
    data.alpha         = getOverviewWindowTargetOpacity(window);
    data.ignoreWindow  = *PSHADOWIGNOREWINDOW;
    data.sharp         = *PSHADOWSHARP;
    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewWindowShadowPassElement>(data));
}

static void renderOverviewWindowBorder(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, float renderScale, bool selected) {
    if (!monitor || !window || (!window->m_isMapped && !window->m_fadingOut))
        return;

    const auto borderSize = window->getRealBorderSize();
    if (borderSize <= 0)
        return;

    static auto PACTIVECOL   = CConfigValue<Hyprlang::CUSTOMTYPE>("general:col.active_border");
    static auto PINACTIVECOL = CConfigValue<Hyprlang::CUSTOMTYPE>("general:col.inactive_border");
    auto* const ACTIVECOL    = reinterpret_cast<CGradientValueData*>((PACTIVECOL.ptr())->getData());
    auto* const INACTIVECOL  = reinterpret_cast<CGradientValueData*>((PINACTIVECOL.ptr())->getData());

    const float targetOpacity    = getOverviewWindowTargetOpacity(window);
    const auto& grad             = selected ? window->m_ruleApplicator->activeBorderColor().valueOr(*ACTIVECOL) : window->m_ruleApplicator->inactiveBorderColor().valueOr(*INACTIVECOL);
    const auto  roundingBase     = window->rounding();
    const auto  roundingPower    = window->roundingPower();
    const auto  correctionOffset = (borderSize * (M_SQRT2 - 1) * std::max(2.0 - roundingPower, 0.0));
    const auto  outerRound       = ((roundingBase + borderSize) - correctionOffset) * monitor->m_scale * renderScale;
    const auto  rounding         = roundingBase * monitor->m_scale * renderScale;

    CBorderPassElement::SBorderData data;
    data.box           = windowBox;
    data.grad1         = grad;
    data.round         = sc<int>(std::round(rounding));
    data.outerRound    = sc<int>(std::round(outerRound));
    data.roundingPower = roundingPower;
    data.a             = targetOpacity;
    data.borderSize    = borderSize;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(data));
}

static void renderOverviewGroupTabIndicators(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, float renderScale, float alpha) {
    if (!monitor || !window || !window->m_group || window->m_group->size() < 1)
        return;

    static auto PINDICATORHEIGHT        = CConfigValue<Hyprlang::INT>("group:groupbar:indicator_height");
    static auto PINDICATORGAP           = CConfigValue<Hyprlang::INT>("group:groupbar:indicator_gap");
    static auto PHEIGHT                 = CConfigValue<Hyprlang::INT>("group:groupbar:height");
    static auto PGRADIENTS              = CConfigValue<Hyprlang::INT>("group:groupbar:gradients");
    static auto PRENDERTITLES           = CConfigValue<Hyprlang::INT>("group:groupbar:render_titles");
    static auto PSTACKED                = CConfigValue<Hyprlang::INT>("group:groupbar:stacked");
    static auto PROUNDING               = CConfigValue<Hyprlang::INT>("group:groupbar:rounding");
    static auto PROUNDINGPOWER          = CConfigValue<Hyprlang::FLOAT>("group:groupbar:rounding_power");
    static auto POUTERGAP               = CConfigValue<Hyprlang::INT>("group:groupbar:gaps_out");
    static auto PINNERGAP               = CConfigValue<Hyprlang::INT>("group:groupbar:gaps_in");
    static auto PGROUPCOLACTIVE         = CConfigValue<Hyprlang::CUSTOMTYPE>("group:groupbar:col.active");
    static auto PGROUPCOLINACTIVE       = CConfigValue<Hyprlang::CUSTOMTYPE>("group:groupbar:col.inactive");
    static auto PGROUPCOLACTIVELOCKED   = CConfigValue<Hyprlang::CUSTOMTYPE>("group:groupbar:col.locked_active");
    static auto PGROUPCOLINACTIVELOCKED = CConfigValue<Hyprlang::CUSTOMTYPE>("group:groupbar:col.locked_inactive");

    if (*PINDICATORHEIGHT <= 0)
        return;

    auto* const GROUPCOLACTIVE         = sc<CGradientValueData*>((PGROUPCOLACTIVE.ptr())->getData());
    auto* const GROUPCOLINACTIVE       = sc<CGradientValueData*>((PGROUPCOLINACTIVE.ptr())->getData());
    auto* const GROUPCOLACTIVELOCKED   = sc<CGradientValueData*>((PGROUPCOLACTIVELOCKED.ptr())->getData());
    auto* const GROUPCOLINACTIVELOCKED = sc<CGradientValueData*>((PGROUPCOLINACTIVELOCKED.ptr())->getData());

    const bool  groupLocked  = window->m_group->locked() || g_pKeybindManager->m_groupsLocked;
    const auto* colActive    = groupLocked ? GROUPCOLACTIVELOCKED : GROUPCOLACTIVE;
    const auto* colInactive  = groupLocked ? GROUPCOLINACTIVELOCKED : GROUPCOLINACTIVE;
    const auto  groupWindows = window->m_group->windows();
    const auto  groupCurrent = window->m_group->current();
    const float pxScale      = monitor->m_scale * renderScale;
    const float indicatorH   = sc<float>(*PINDICATORHEIGHT) * pxScale;
    const float outerGap     = sc<float>(*POUTERGAP) * pxScale;
    const float innerGap     = sc<float>(*PINNERGAP) * pxScale;
    const float oneBarHeight = sc<float>(*POUTERGAP + *PINDICATORHEIGHT + *PINDICATORGAP + (*PGRADIENTS || *PRENDERTITLES ? *PHEIGHT : 0)) * pxScale;
    const float borderPx     = sc<float>(window->getRealBorderSize()) * pxScale;
    const int   rounding     = sc<int>(std::round(*PROUNDING * pxScale));
    const CBox   indicatorArea = windowBox.copy().expand(borderPx);

    float xoff = 0.F;
    float yoff = 0.F;

    for (size_t i = 0; i < groupWindows.size(); ++i) {
        const size_t windowIdx = *PSTACKED ? groupWindows.size() - i - 1 : i;
        const auto   member    = groupWindows[windowIdx].lock();
        if (!member)
            continue;

        CHyprColor color = member == groupCurrent ? colActive->m_colors[0] : colInactive->m_colors[0];
        color.a *= alpha;
        if (color.a <= 0.F)
            continue;

        CBox box;
        if (*PSTACKED) {
            box = {indicatorArea.x, indicatorArea.y - yoff - outerGap - indicatorH, indicatorArea.width, indicatorH};
            yoff += oneBarHeight;
        } else {
            const float barWidth = (indicatorArea.width - innerGap * (groupWindows.size() - 1)) / groupWindows.size();
            box                  = {indicatorArea.x + xoff, indicatorArea.y - outerGap - indicatorH, barWidth, indicatorH};
            xoff += innerGap + barWidth;
        }

        box.round();
        if (box.empty())
            continue;

        CRectPassElement::SRectData data;
        data.box           = box;
        data.color         = color;
        data.round         = rounding;
        data.roundingPower = *PROUNDINGPOWER;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
    }
}

static void renderOverviewGroupTabs(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const CBox& workspaceBox, float renderScale) {
    if (!monitor || !window || !window->m_group || window->m_group->size() < 1)
        return;

    auto* const GROUPBAR = dynamic_cast<CHyprGroupBarDecoration*>(window->getDecorationByType(DECORATION_GROUPBAR));
    if (!GROUPBAR)
        return;

    static auto PHEIGHT          = CConfigValue<Hyprlang::INT>("group:groupbar:height");
    static auto PINDICATORGAP    = CConfigValue<Hyprlang::INT>("group:groupbar:indicator_gap");
    static auto PINDICATORHEIGHT = CConfigValue<Hyprlang::INT>("group:groupbar:indicator_height");
    static auto PRENDERTITLES    = CConfigValue<Hyprlang::INT>("group:groupbar:render_titles");
    static auto PGRADIENTS       = CConfigValue<Hyprlang::INT>("group:groupbar:gradients");
    static auto PSTACKED         = CConfigValue<Hyprlang::INT>("group:groupbar:stacked");
    static auto POUTERGAP        = CConfigValue<Hyprlang::INT>("group:groupbar:gaps_out");
    static auto PKEEPUPPERGAP    = CConfigValue<Hyprlang::INT>("group:groupbar:keep_upper_gap");

    const auto  ONEBARHEIGHT  = *POUTERGAP + *PINDICATORHEIGHT + *PINDICATORGAP + (*PGRADIENTS || *PRENDERTITLES ? *PHEIGHT : 0);
    const auto  DESIREDHEIGHT = *PSTACKED ? (ONEBARHEIGHT * window->m_group->size()) + *POUTERGAP * *PKEEPUPPERGAP : *POUTERGAP * (1 + *PKEEPUPPERGAP) + ONEBARHEIGHT;
    const auto  EDGEPOINT     = g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, window);
    CBox        assignedBox   = {window->m_realPosition->value() - Vector2D{0.F, sc<float>(DESIREDHEIGHT)}, {window->m_realSize->value().x, sc<float>(DESIREDHEIGHT)}};
    assignedBox.translate(-EDGEPOINT);

    if (window->m_workspace && !window->m_pinned)
        assignedBox.translate(-window->m_workspace->m_renderOffset->value());

    const auto PREVASSIGNEDBOX = GROUPBAR->m_assignedBox;
    GROUPBAR->m_assignedBox    = assignedBox;
    auto restoreAssignedBox    = Hyprutils::Utils::CScopeGuard([GROUPBAR, PREVASSIGNEDBOX] { GROUPBAR->m_assignedBox = PREVASSIGNEDBOX; });

    SRenderModifData modif;
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_SCALE, renderScale);
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, Vector2D{workspaceBox.x / monitor->m_scale, workspaceBox.y / monitor->m_scale});

    GROUPBAR->updateWindow(window);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
    const float opacity = getOverviewWindowTargetOpacity(window);
    GROUPBAR->draw(monitor, opacity);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = SRenderModifData{}}));
    renderOverviewGroupTabIndicators(monitor, window, windowBox, renderScale, opacity);
}

CScrollOverview::~CScrollOverview() {
    g_pHyprRenderer->makeEGLCurrent();
    if (realtimePreviewTimer) {
        wl_event_source_remove(realtimePreviewTimer);
        realtimePreviewTimer = nullptr;
    }
    emitFullscreenVisibilityState(Desktop::focusState()->window(), false);
    restoreInputConfigOverrides();
    restoreForcedSurfaceVisibility();
    restoreForcedWindowVisibility();
    restoreForcedLayerVisibility();
    images.clear(); // otherwise we get a vram leak
    Cursor::overrideController->unsetOverride(Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
    g_pHyprOpenGL->markBlurDirtyForMonitor(pMonitor.lock());
}

CScrollOverview::CScrollOverview(PHLWORKSPACE startedOn_, bool swipe_) : startedOn(startedOn_), swipe(swipe_) {
    const auto          PMONITOR = Desktop::focusState()->monitor();
    pMonitor                     = PMONITOR;

    applyInputConfigOverrides();
    realtimePreviewTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, realtimePreviewTimerCallback, this);
    scheduleMinimumPreviewFrame();

    g_pAnimationManager->createAnimation(1.F, scale, g_pConfigManager->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
    g_pAnimationManager->createAnimation({}, viewOffset, g_pConfigManager->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);

    scale->setUpdateCallback(damageMonitor);
    viewOffset->setUpdateCallback(damageMonitor);

    if (!swipe)
        *scale = getOverviewConfiguredScale();

    const auto initialFullscreenWindow =
        PMONITOR && PMONITOR->m_activeWorkspace ? getOverviewWindowToShow(PMONITOR->m_activeWorkspace->getFullscreenWindow()) : PHLWINDOW{};
    emitFullscreenVisibilityState(initialFullscreenWindow ? initialFullscreenWindow : Desktop::focusState()->window(), true);

    lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;

    auto onMouseMove = [this](Vector2D, Event::SCallbackInfo& info) {
        if (closing)
            return;

        lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;

        if (!dragPointerDown && !dragActiveWindow && isPointerOnTopLayer(pMonitor.lock()))
            return;

        info.cancelled = true;

        if (dragPointerDown && dragPendingWindow) {
            static auto PDRAGTHRESHOLD = CConfigValue<Hyprlang::INT>("binds:drag_threshold");

            if (!dragActiveWindow && dragStartMouseLocal.distanceSq(lastMousePosLocal) > std::pow(*PDRAGTHRESHOLD, 2))
                beginWindowDrag();

            if (dragActiveWindow)
                updateWindowDrag();
        }

        //  highlightHoverDebug();
    };

    auto onTouchMove = [this](ITouch::SMotionEvent, Event::SCallbackInfo& info) {
        if (closing)
            return;

        info.cancelled    = true;
        lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;
    };

    auto onMouseButton = [this](IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
        if (closing)
            return;

        if (!dragPointerDown && !dragActiveWindow && isPointerOnTopLayer(pMonitor.lock()))
            return;

        info.cancelled = true;

        if (event.button != BTN_LEFT) {
            if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                selectHoveredWorkspace();
                close();
            }
            return;
        }

        lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;

        if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
            size_t dragWorkspaceIdx = 0;

            dragPointerDown      = true;
            dragStartMouseLocal  = lastMousePosLocal;
            dragPendingWindow    = windowAtOverviewCursor(&dragWorkspaceIdx);
            return;
        }

        constexpr float CLICK_MAX_DRAG_DISTANCE = 10.F;

        if (dragActiveWindow && dragStartMouseLocal.distanceSq(lastMousePosLocal) < CLICK_MAX_DRAG_DISTANCE * CLICK_MAX_DRAG_DISTANCE) {
            dragPointerDown = false;
            dragPendingWindow.reset();
            dragActiveWindow.reset();
            dragOriginalWorkspace.reset();
            dragStartedTiled      = false;
            dragOriginalFloatSize = Vector2D{};
            dragOriginalBox       = CBox{};

            selectHoveredWorkspace();
            close();
            return;
        }

        if (dragActiveWindow) {
            endWindowDrag();
            return;
        }

        dragPointerDown = false;
        dragPendingWindow.reset();

        selectHoveredWorkspace();

        close();
    };

    auto onCursorSelect = [this](auto, Event::SCallbackInfo& info) {
        if (closing)
            return;

        if (isPointerOnTopLayer(pMonitor.lock()))
            return;

        info.cancelled = true;

        selectHoveredWorkspace();

        close();
    };

    auto onMouseAxis = [this](IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
        if (closing)
            return;

        info.cancelled = true;
        moveViewportWorkspace(e.delta > 0);
    };

    auto onWindowOpen = [this](PHLWINDOW) {
        if (closing)
            return;

        rebuildPending = true;
        damage();
    };

    auto onWindowClose = [this](PHLWINDOW) {
        if (closing)
            return;

        rebuildPending = true;
        damage();
    };

    auto onWindowMove = [this](PHLWINDOW, PHLWORKSPACE) {
        if (closing)
            return;

        rebuildPending = true;
        damage();
    };

    auto onWindowActive = [this](PHLWINDOW window, Desktop::eFocusReason) {
        if (closing)
            return;

        const auto overviewWindow = getOverviewWindowToShow(window);
        const auto fullscreenWindow = overviewWindow && overviewWindow->m_workspace ? getOverviewWindowToShow(overviewWindow->m_workspace->getFullscreenWindow()) : PHLWINDOW{};

        if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == overviewWindow->m_workspace && overviewWindow->m_isFloating)
            emitFullscreenVisibilityState(fullscreenWindow, true);
        else
            emitFullscreenVisibilityState(overviewWindow, true);

        if (shouldShowOverviewWindow(overviewWindow) && overviewWindow->m_monitor == pMonitor) {
            rebuildPending = true;
            closeOnWindow  = overviewWindow;
            rememberSelection(overviewWindow);

            for (size_t i = 0; i < images.size(); ++i) {
                if (images[i]->pWorkspace == overviewWindow->m_workspace) {
                    viewportCurrentWorkspace = i;
                    break;
                }
            }
        }

        damage();
    };

    auto onWindowFullscreen = [this](PHLWINDOW window) {
        if (closing || emittingFullscreenVisibilityState)
            return;

        window = getOverviewWindowToShow(window);
        if (!window || window->m_monitor != pMonitor || !window->isFullscreen())
            return;

        emitFullscreenVisibilityState(window, true);
    };

    auto onWorkspaceLifecycle = [this](auto) {
        if (closing)
            return;

        rebuildPending = true;
        damage();
    };

    auto onWorkspaceActive = [this](PHLWORKSPACE workspace) {
        if (closing || !workspace || workspace->m_monitor != pMonitor)
            return;

        workspaceSyncPending = true;
        damage();
    };

    auto onKeyboardKey = [this](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
        if (closing || event.state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;

        if (isTopLayerFocused(pMonitor.lock()))
            return;

        const auto KEYSYM = getOverviewKeysym(event);
        const auto MODS   = g_pInputManager->getModsFromAllKBs() & ~(HL_MODIFIER_CAPS | HL_MODIFIER_MOD2);

        if ((KEYSYM == XKB_KEY_Return || KEYSYM == XKB_KEY_KP_Enter || KEYSYM == XKB_KEY_Left || KEYSYM == XKB_KEY_KP_Left || KEYSYM == XKB_KEY_Right ||
             KEYSYM == XKB_KEY_KP_Right || KEYSYM == XKB_KEY_Up || KEYSYM == XKB_KEY_KP_Up || KEYSYM == XKB_KEY_Down || KEYSYM == XKB_KEY_KP_Down) &&
            MODS != 0)
            return;

        switch (KEYSYM) {
            case XKB_KEY_Left:
            case XKB_KEY_KP_Left: moveWindowSelection("l"); break;
            case XKB_KEY_Right:
            case XKB_KEY_KP_Right: moveWindowSelection("r"); break;
            case XKB_KEY_Up:
            case XKB_KEY_KP_Up:
                if (!moveWindowSelection("u"))
                    moveViewportWorkspace(false);
                break;
            case XKB_KEY_Down:
            case XKB_KEY_KP_Down:
                if (!moveWindowSelection("d"))
                    moveViewportWorkspace(true);
                break;
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter: close(); break;
            default: return;
        }

        info.cancelled = true;
    };

    mouseMoveHook = Event::bus()->m_events.input.mouse.move.listen(onMouseMove);
    touchMoveHook = Event::bus()->m_events.input.touch.motion.listen(onTouchMove);
    mouseAxisHook = Event::bus()->m_events.input.mouse.axis.listen(onMouseAxis);

    mouseButtonHook = Event::bus()->m_events.input.mouse.button.listen(onMouseButton);
    touchDownHook   = Event::bus()->m_events.input.touch.down.listen(onCursorSelect);

    windowOpenHook      = Event::bus()->m_events.window.open.listen(onWindowOpen);
    windowCloseHook     = Event::bus()->m_events.window.close.listen(onWindowClose);
    windowMoveHook      = Event::bus()->m_events.window.moveToWorkspace.listen(onWindowMove);
    windowActiveHook    = Event::bus()->m_events.window.active.listen(onWindowActive);
    windowFullscreenHook = Event::bus()->m_events.window.fullscreen.listen(onWindowFullscreen);
    workspaceCreatedHook = Event::bus()->m_events.workspace.created.listen(onWorkspaceLifecycle);
    workspaceRemovedHook = Event::bus()->m_events.workspace.removed.listen(onWorkspaceLifecycle);
    workspaceActiveHook  = Event::bus()->m_events.workspace.active.listen(onWorkspaceActive);
    keyboardKeyHook     = Event::bus()->m_events.input.keyboard.key.listen(onKeyboardKey);

    Cursor::overrideController->setOverride("left_ptr", Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);

    redrawAll();

    rememberSelection(Desktop::focusState()->window());
    viewportCurrentWorkspace = activeWorkspaceIndex();
    syncSelectionToViewport();
}

static void renderOverviewLayerLevel(PHLMONITOR monitor, uint32_t layer, const CBox& workspaceBox, float renderScale, const Time::steady_tp& now) {
    if (!monitor)
        return;

    bool pushedRenderHints = false;

    for (auto const& ls : monitor->m_layerSurfaceLayers[layer]) {
        if (!Desktop::View::validMapped(ls.lock()))
            continue;

        if (!pushedRenderHints) {
            SRenderModifData modif;
            modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_SCALE, renderScale);
            modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, Vector2D{workspaceBox.x / monitor->m_scale, workspaceBox.y / monitor->m_scale});

            g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
            pushedRenderHints = true;
        }

        g_pHyprRenderer->renderLayer(ls.lock(), monitor, now);
    }

    if (pushedRenderHints)
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = SRenderModifData{}}));
}

void CScrollOverview::renderWallpaperLayers(PHLMONITOR monitor, const CBox& workspaceBox, float renderScale, const Time::steady_tp& now) {
    if (!monitor)
        return;

    renderOverviewLayerLevel(monitor, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, workspaceBox, renderScale, now);
}

static void renderOverviewWorkspaceShadow(PHLMONITOR monitor, const CBox& workspaceBox, float overviewScale, bool cutoutCenter) {
    if (!monitor)
        return;

    const auto SHADOW = getOverviewShadowConfig();
    if (!SHADOW.enabled || SHADOW.range <= 0 || SHADOW.color.a == 0.F)
        return;

    const int RANGE = sc<int>(std::round(SHADOW.range * monitor->m_scale * overviewScale));
    if (RANGE <= 0)
        return;

    const auto FULLBOX = workspaceBox.copy().expand(RANGE);
    if (FULLBOX.width < 1 || FULLBOX.height < 1)
        return;

    COverviewWindowShadowPassElement::SData data;
    data.monitor       = monitor;
    data.fullBox       = FULLBOX;
    data.cutoutBox     = workspaceBox;
    data.rounding      = 0;
    data.roundingPower = 2.F;
    data.range         = RANGE;
    data.renderPower   = SHADOW.renderPower;
    data.color         = SHADOW.color;
    data.alpha         = 1.F;
    data.ignoreWindow  = cutoutCenter;
    data.sharp         = false;
    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewWindowShadowPassElement>(data));
}

static void focusOverviewFullscreenWindowIfActiveWorkspace(const PHLWINDOW& fullscreenWindow_, const PHLWORKSPACE& workspace, PHLMONITOR monitor) {
    const auto FULLSCREENWINDOW = getOverviewWindowToShow(fullscreenWindow_);

    if (!monitor || !workspace || workspace != monitor->m_activeWorkspace || !validMapped(FULLSCREENWINDOW) || FULLSCREENWINDOW->m_workspace != workspace)
        return;

    if (Desktop::focusState()->window() == FULLSCREENWINDOW)
        return;

    Desktop::focusState()->fullWindowFocus(FULLSCREENWINDOW, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE, nullptr, true);
}

size_t CScrollOverview::activeWorkspaceIndex() const {
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn)
            return i;
    }

    return 0;
}

void CScrollOverview::rebuildWorkspaceImages() {
    const auto selectedWorkspace = closeOnWindow ? closeOnWindow->m_workspace : startedOn;
    const auto selectedWindow    = closeOnWindow;
    const auto viewportWorkspace = viewportCurrentWorkspace < images.size() ? images[viewportCurrentWorkspace]->pWorkspace : startedOn;

    images.clear();

    for (const auto& w : g_pCompositor->getWorkspaces()) {
        if (w && w->m_monitor == pMonitor && !w->m_isSpecialWorkspace)
            images.emplace_back(makeShared<SWorkspaceImage>(w.lock()));
    }

    std::sort(images.begin(), images.end(), [](const auto& a, const auto& b) { return a->pWorkspace->m_id < b->pWorkspace->m_id; });

    if (images.empty()) {
        viewportCurrentWorkspace = 0;
        closeOnWindow.reset();
        return;
    }

    viewportCurrentWorkspace = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == viewportWorkspace || images[i]->pWorkspace == selectedWorkspace) {
            viewportCurrentWorkspace = i;
            break;
        }
    }

    closeOnWindow = selectedWindow;
}

void CScrollOverview::seedRememberedSelections() {
    for (const auto& img : images) {
        if (!img->pWorkspace)
            continue;

        const auto WORKSPACEID = img->pWorkspace->m_id;

        if (const auto it = rememberedSelection.find(WORKSPACEID); it != rememberedSelection.end()) {
            const auto rememberedWindow = getOverviewWindowToShow(it->second.lock());
            if (rememberedWindow && rememberedWindow->m_workspace == img->pWorkspace && shouldShowOverviewWindow(rememberedWindow))
                continue;
        }

        const auto lastFocusedWindow = getOverviewWindowToShow(img->pWorkspace->getLastFocusedWindow());
        if (!lastFocusedWindow || lastFocusedWindow->m_workspace != img->pWorkspace || !shouldShowOverviewWindow(lastFocusedWindow))
            continue;

        rememberedSelection[WORKSPACEID] = lastFocusedWindow;
    }
}

void CScrollOverview::rememberSelection(PHLWINDOW window) {
    window = getOverviewWindowToShow(window);

    if (!window || !window->m_workspace)
        return;

    rememberedSelection[window->m_workspace->m_id] = window;
}

PHLWINDOW CScrollOverview::windowAtOverviewCursor(size_t* hoveredWorkspaceIdx) {
    size_t activeIdx = activeWorkspaceIndex();

    const auto VIEWPORT_CENTER = CBox{{}, pMonitor->m_size}.middle();

    const auto WORKSPACEPITCH = getWorkspaceRenderedPitch(pMonitor.lock(), scale->value());
    float      yoff           = -(float)activeIdx * WORKSPACEPITCH;
    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];

        const auto selectWindow = [&](const PHLWINDOW& window) -> PHLWINDOW {
            if (hoveredWorkspaceIdx)
                *hoveredWorkspaceIdx = workspaceIdx;

            return window;
        };

        const auto fullscreenWindow = wimg->pWorkspace ? getOverviewWindowToShow(wimg->pWorkspace->getFullscreenWindow()) : PHLWINDOW{};

        if (shouldShowOverviewWindow(fullscreenWindow)) {
            for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
                const auto window = getOverviewWindowToShow(it->lock());
                if (!shouldShowOverviewWindow(window) || !window->m_isFloating)
                    continue;

                const auto texbox = getOverviewWindowBoxLogical(window, pMonitor.lock(), scale->value(), viewOffset->value(), yoff);

                if (texbox.containsPoint(lastMousePosLocal))
                    return selectWindow(window);
            }

            CBox texbox = {fullscreenWindow->m_realPosition->value() - pMonitor->m_position, fullscreenWindow->m_realSize->value()};

            texbox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(-viewOffset->value() * scale->value());
            texbox.translate({0.F, yoff});

            if (texbox.containsPoint(lastMousePosLocal))
                return selectWindow(fullscreenWindow);

            yoff += WORKSPACEPITCH;
            continue;
        }

        for (const bool floating : {true, false}) {
            for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
                const auto window = getOverviewWindowToShow(it->lock());
                if (!shouldShowOverviewWindow(window) || window->m_isFloating != floating)
                    continue;

                const auto texbox = getOverviewWindowBoxLogical(window, pMonitor.lock(), scale->value(), viewOffset->value(), yoff);

                if (texbox.containsPoint(lastMousePosLocal))
                    return selectWindow(window);
            }
        }

        yoff += WORKSPACEPITCH;
    }

    return nullptr;
}

PHLWINDOW CScrollOverview::windowAtOverviewCursorOnWorkspace(size_t workspaceIdx, const PHLWINDOW& ignoredWindow, CBox* windowBox) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || workspaceIdx >= images.size() || !images[workspaceIdx])
        return nullptr;

    const auto ACTIVEIDX         = activeWorkspaceIndex();
    const auto WORKSPACE_YOFFSET = (sc<long>(workspaceIdx) - sc<long>(ACTIVEIDX)) * getWorkspaceRenderedPitch(MONITOR, scale->value());

    PHLWINDOW bestWindow;
    CBox      bestBox;
    float     bestDistanceSq = std::numeric_limits<float>::max();

    for (const bool floating : {true, false}) {
        for (auto it = images[workspaceIdx]->windows.rbegin(); it != images[workspaceIdx]->windows.rend(); ++it) {
            const auto WINDOW = getOverviewWindowToShow(it->lock());
            if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating != floating)
                continue;

            const auto box    = getOverviewWindowBoxLogical(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACE_YOFFSET);
            const auto hitbox = expandOverviewWindowHitbox(box, scale->value());
            if (box.containsPoint(lastMousePosLocal)) {
                if (windowBox)
                    *windowBox = box;

                return WINDOW;
            }

            if (!hitbox.containsPoint(lastMousePosLocal))
                continue;

            const auto distanceSq = overviewPointDistanceSqToBox(lastMousePosLocal, box);
            if (distanceSq >= bestDistanceSq)
                continue;

            bestWindow     = WINDOW;
            bestBox        = box;
            bestDistanceSq = distanceSq;
        }

        if (bestWindow)
            break;
    }

    if (bestWindow && windowBox)
        *windowBox = bestBox;

    return bestWindow;
}

PHLWORKSPACE CScrollOverview::workspaceAtOverviewCursor(size_t* hoveredWorkspaceIdx) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return nullptr;

    const auto VIEWPORT_CENTER = CBox{{}, MONITOR->m_size}.middle();
    const auto WORKSPACEPITCH  = getWorkspaceRenderedPitch(MONITOR, scale->value());
    const auto ACTIVEIDX       = activeWorkspaceIndex();

    float yoff = -(float)ACTIVEIDX * WORKSPACEPITCH;
    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace) {
            yoff += WORKSPACEPITCH;
            continue;
        }

        const auto workspaceBox = getOverviewWorkspaceBoxLogical(MONITOR, scale->value(), viewOffset->value(), yoff);

        if (lastMousePosLocal.y >= workspaceBox.y && lastMousePosLocal.y <= workspaceBox.y + workspaceBox.height) {
            if (hoveredWorkspaceIdx)
                *hoveredWorkspaceIdx = workspaceIdx;

            return wimg->pWorkspace;
        }

        yoff += WORKSPACEPITCH;
    }

    return nullptr;
}

void CScrollOverview::selectHoveredWorkspace() {
    size_t    workspaceIdx = 0;
    PHLWINDOW window       = windowAtOverviewCursor(&workspaceIdx);

    if (window) {
        closeOnWindow            = window;
        viewportCurrentWorkspace = workspaceIdx;
        rememberSelection(window);
        return;
    }

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, scale->value());

    for (size_t i = 0; i < images.size(); ++i) {
        const auto& wimg = images[i];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto yoff = (sc<long>(i) - sc<long>(ACTIVEIDX)) * PITCH;
        const auto box  = getOverviewWorkspaceBoxLogical(MONITOR, scale->value(), viewOffset->value(), yoff);
        if (!box.containsPoint(lastMousePosLocal))
            continue;

        closeOnWindow.reset();
        viewportCurrentWorkspace = i;
        return;
    }
}

Vector2D CScrollOverview::overviewPointToGlobal(size_t workspaceIdx, const Vector2D& pointLocal) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return pointLocal;

    const auto  SAFE_SCALE       = std::max(scale->value(), 0.01F);
    const auto  VIEWPORT_CENTER  = CBox{{}, MONITOR->m_size}.middle();
    const auto  WORKSPACE_YOFF   = (sc<long>(workspaceIdx) - sc<long>(activeWorkspaceIndex())) * getWorkspaceRenderedPitch(MONITOR, scale->value());

    return ((pointLocal - Vector2D{0.F, WORKSPACE_YOFF} + viewOffset->value() * scale->value() - VIEWPORT_CENTER) * (1.F / SAFE_SCALE)) + VIEWPORT_CENTER + MONITOR->m_position;
}

CBox CScrollOverview::draggedWindowBoxLogical(size_t workspaceIdx) const {
    const auto WINDOW  = getOverviewWindowToShow(dragActiveWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!WINDOW || !MONITOR || workspaceIdx >= images.size())
        return {};

    const auto WORKSPACE_YOFFSET = (sc<long>(workspaceIdx) - sc<long>(activeWorkspaceIndex())) * getWorkspaceRenderedPitch(MONITOR, scale->value());
    auto       box               = getOverviewWindowBoxLogical(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACE_YOFFSET);
    box.translate(lastMousePosLocal - dragStartMouseLocal);

    return box;
}

void CScrollOverview::beginWindowDrag() {
    const auto WINDOW = getOverviewWindowToShow(dragPendingWindow.lock());
    if (!shouldShowOverviewWindow(WINDOW) || !WINDOW->layoutTarget())
        return;

    closeOnWindow = WINDOW;
    rememberSelection(WINDOW);

    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == WINDOW->m_workspace) {
            viewportCurrentWorkspace = i;
            break;
        }
    }

    const auto TARGET       = WINDOW->layoutTarget();
    dragStartedTiled        = !TARGET->floating();
    dragOriginalFloatSize   = TARGET->lastFloatingSize();
    dragOriginalWorkspace   = WINDOW->m_workspace;
    dragOriginalBox          = TARGET->position();
    dragActiveWindow         = WINDOW;

    updateWindowDrag();
}

void CScrollOverview::updateWindowDrag() {
    if (!dragActiveWindow)
        return;

    damage();
}

void CScrollOverview::endWindowDrag() {
    const auto WINDOW = getOverviewWindowToShow(dragActiveWindow.lock());
    const auto TARGET = WINDOW ? WINDOW->layoutTarget() : nullptr;
    const auto SPACE  = TARGET ? TARGET->space() : nullptr;
    const auto ALGO   = SPACE ? SPACE->algorithm() : nullptr;

    const bool          RETILEONEND      = dragStartedTiled && TARGET && SPACE && ALGO;
    size_t              dropWorkspaceIdx = 0;
    const auto          DROPWORKSPACE    = workspaceAtOverviewCursor(&dropWorkspaceIdx);
    const auto          ORIGINALWORKSPACE = dragOriginalWorkspace.lock();
    const bool          MOVEWORKSPACE    = DROPWORKSPACE && DROPWORKSPACE != ORIGINALWORKSPACE;
    const auto          DRAGBOX          = DROPWORKSPACE ? draggedWindowBoxLogical(dropWorkspaceIdx) : CBox{};
    int                 horizontalDropSide = 0;
    CBox                dropAnchorOverviewBox;
    const auto          DROPANCHOR = DROPWORKSPACE ? windowAtOverviewCursorOnWorkspace(dropWorkspaceIdx, WINDOW, &dropAnchorOverviewBox) : PHLWINDOW{};
    std::string         dropDirection;

    if (!DROPWORKSPACE) {
        dragPointerDown = false;
        dragPendingWindow.reset();
        dragActiveWindow.reset();
        dragOriginalWorkspace.reset();
        dragStartedTiled      = false;
        dragOriginalFloatSize = Vector2D{};
        dragOriginalBox       = CBox{};
        damage();
        return;
    }

    const auto SOURCEFULLSCREENWINDOW = ORIGINALWORKSPACE ? getOverviewWindowToShow(ORIGINALWORKSPACE->getFullscreenWindow()) : PHLWINDOW{};
    const bool RESTORESOURCEFULLSCREENFOCUS = WINDOW && WINDOW->m_isFloating && MOVEWORKSPACE && shouldShowOverviewWindow(SOURCEFULLSCREENWINDOW) &&
        SOURCEFULLSCREENWINDOW != WINDOW && SOURCEFULLSCREENWINDOW->m_workspace == ORIGINALWORKSPACE && SOURCEFULLSCREENWINDOW->isFullscreen();

    const auto MONITOR = pMonitor.lock();
    if (MONITOR) {
        const auto WORKSPACEBOX = getOverviewWorkspaceBoxLogical(
            MONITOR, scale->value(), viewOffset->value(), (sc<long>(dropWorkspaceIdx) - sc<long>(activeWorkspaceIndex())) * getWorkspaceRenderedPitch(MONITOR, scale->value()));

        if (lastMousePosLocal.x < WORKSPACEBOX.x)
            horizontalDropSide = -1;
        else if (lastMousePosLocal.x > WORKSPACEBOX.x + WORKSPACEBOX.width)
            horizontalDropSide = 1;
    }

    if (!DROPANCHOR && horizontalDropSide == 0 && MONITOR && dropWorkspaceIdx < images.size() && images[dropWorkspaceIdx]) {
        const auto WORKSPACE_YOFFSET = (sc<long>(dropWorkspaceIdx) - sc<long>(activeWorkspaceIndex())) * getWorkspaceRenderedPitch(MONITOR, scale->value());

        float minWindowX = std::numeric_limits<float>::max();
        float maxWindowX = std::numeric_limits<float>::lowest();
        bool  foundWindow = false;

        for (const auto& windowRef : images[dropWorkspaceIdx]->windows) {
            const auto OTHERWINDOW = getOverviewWindowToShow(windowRef.lock());
            if (!shouldShowOverviewWindow(OTHERWINDOW) || OTHERWINDOW == WINDOW)
                continue;

            const auto BOX = getOverviewWindowBoxLogical(OTHERWINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACE_YOFFSET);
            minWindowX     = std::min(minWindowX, sc<float>(BOX.x));
            maxWindowX     = std::max(maxWindowX, sc<float>(BOX.x + BOX.width));
            foundWindow    = true;
        }

        if (foundWindow) {
            if (lastMousePosLocal.x < minWindowX)
                horizontalDropSide = -1;
            else if (lastMousePosLocal.x > maxWindowX)
                horizontalDropSide = 1;
        }
    }

    const bool SCROLLINGLAYOUT = overviewScrollingAlgorithmForTarget(TARGET) != nullptr;

    if (DROPANCHOR && SCROLLINGLAYOUT) {
        const auto LOCAL_X = lastMousePosLocal.x - dropAnchorOverviewBox.x;
        const auto LOCAL_Y = lastMousePosLocal.y - dropAnchorOverviewBox.y;

        if (LOCAL_X < dropAnchorOverviewBox.width / 3.F)
            dropDirection = "l";
        else if (LOCAL_X > dropAnchorOverviewBox.width * 2.F / 3.F)
            dropDirection = "r";
        else
            dropDirection = LOCAL_Y < dropAnchorOverviewBox.height / 2.F ? "u" : "d";

    }

    if (RETILEONEND && MOVEWORKSPACE) {
        g_pCompositor->moveWindowToWorkspaceSafe(WINDOW, DROPWORKSPACE);
        TARGET->rememberFloatingSize(dragOriginalFloatSize);
    } else if (RETILEONEND) {
        TARGET->damageEntire();

        if (DROPANCHOR && !SCROLLINGLAYOUT && DROPANCHOR->layoutTarget()) {
            DROPANCHOR->layoutTarget()->damageEntire();
            g_layoutManager->switchTargets(TARGET, DROPANCHOR->layoutTarget(), true);
            DROPANCHOR->layoutTarget()->damageEntire();
        } else if (DROPANCHOR && !dropDirection.empty())
            moveOverviewTargetNextToWindow(TARGET, DROPANCHOR, dropDirection);
        else
            moveOverviewTargetToHorizontalEdge(TARGET, horizontalDropSide);

        TARGET->rememberFloatingSize(dragOriginalFloatSize);
        TARGET->warpPositionSize();
        TARGET->damageEntire();

        Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

        if (const auto WORKSPACE = SPACE->workspace())
            WORKSPACE->updateWindows();
    } else if (WINDOW && MOVEWORKSPACE) {
        g_pCompositor->moveWindowToWorkspaceSafe(WINDOW, DROPWORKSPACE);
        if (TARGET) {
            const auto GLOBALSIZE = DRAGBOX.size() * (1.F / std::max(scale->value(), 0.01F));
            const auto GLOBALBOX  = centerBoxInWorkspace(CBox{Vector2D{}, GLOBALSIZE}, DROPWORKSPACE, MONITOR);
            TARGET->setPositionGlobal(GLOBALBOX);
            TARGET->warpPositionSize();
        }
    } else if (TARGET && !dragStartedTiled) {
        size_t workspaceIdx = 0;
        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i] && images[i]->pWorkspace == WINDOW->m_workspace) {
                workspaceIdx = i;
                break;
            }
        }

        const auto FLOATBOX   = draggedWindowBoxLogical(workspaceIdx);
        const auto GLOBALPOS  = overviewPointToGlobal(workspaceIdx, FLOATBOX.pos());
        const auto GLOBALSIZE = FLOATBOX.size() * (1.F / std::max(scale->value(), 0.01F));
        auto       GLOBALBOX  = CBox{GLOBALPOS, GLOBALSIZE};

        GLOBALBOX = clampBoxToWorkspace(GLOBALBOX, WINDOW->m_workspace, MONITOR);

        TARGET->damageEntire();
        TARGET->setPositionGlobal(GLOBALBOX);
        TARGET->warpPositionSize();
        TARGET->damageEntire();
    }

    if (RESTORESOURCEFULLSCREENFOCUS) {
        const bool POSTHIDDENEVENT = MONITOR && ORIGINALWORKSPACE == MONITOR->m_activeWorkspace;

        if (POSTHIDDENEVENT) {
            closeOnWindow = SOURCEFULLSCREENWINDOW;
            rememberSelection(SOURCEFULLSCREENWINDOW);
            focusOverviewFullscreenWindowIfActiveWorkspace(SOURCEFULLSCREENWINDOW, ORIGINALWORKSPACE, MONITOR);
            emitFullscreenVisibilityState(SOURCEFULLSCREENWINDOW, true);
        }
    }

    if (DROPWORKSPACE && MONITOR && DROPWORKSPACE == MONITOR->m_activeWorkspace) {
        const auto FULLSCREENWINDOW = getOverviewWindowToShow(DROPWORKSPACE->getFullscreenWindow());
        if (shouldShowOverviewWindow(FULLSCREENWINDOW) && FULLSCREENWINDOW->m_workspace == DROPWORKSPACE)
            emitFullscreenVisibilityState(FULLSCREENWINDOW, true);
    }

    dragPointerDown = false;
    dragPendingWindow.reset();
    dragActiveWindow.reset();
    dragOriginalWorkspace.reset();
    dragStartedTiled              = false;
    dragOriginalFloatSize         = Vector2D{};
    dragOriginalBox               = CBox{};
    rebuildPending                = true;
    damage();
}

void CScrollOverview::moveViewportWorkspace(bool up) {
    if (images.empty())
        return;

    if (viewportCurrentWorkspace == 0 && !up)
        return;
    if (viewportCurrentWorkspace == images.size() - 1 && up)
        return;

    if (up)
        viewportCurrentWorkspace++;
    else
        viewportCurrentWorkspace--;

    const auto& TARGETWORKSPACEIMAGE = images[viewportCurrentWorkspace];
    if (!TARGETWORKSPACEIMAGE || !TARGETWORKSPACEIMAGE->pWorkspace)
        return;

    closeOnWindow.reset();

    if (const auto it = rememberedSelection.find(TARGETWORKSPACEIMAGE->pWorkspace->m_id); it != rememberedSelection.end()) {
        const auto rememberedWindow = getOverviewWindowToShow(it->second.lock());
        if (rememberedWindow && rememberedWindow->m_workspace == TARGETWORKSPACEIMAGE->pWorkspace && shouldShowOverviewWindow(rememberedWindow))
            closeOnWindow = rememberedWindow;
    }

    if (!closeOnWindow) {
        for (const auto& windowRef : TARGETWORKSPACEIMAGE->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!shouldShowOverviewWindow(window))
                continue;

            closeOnWindow = window;
            break;
        }
    }

    if (pMonitor && pMonitor->m_activeWorkspace != TARGETWORKSPACEIMAGE->pWorkspace)
        pMonitor->changeWorkspace(TARGETWORKSPACEIMAGE->pWorkspace, false, true, true);

    damage();
}

void CScrollOverview::syncSelectionToViewport() {
    if (images.empty() || viewportCurrentWorkspace >= images.size()) {
        closeOnWindow.reset();
        return;
    }

    const auto& WSPACE = images[viewportCurrentWorkspace];

    if (closeOnWindow && closeOnWindow->m_workspace == WSPACE->pWorkspace) {
        const auto selectedWindow = getOverviewWindowToShow(closeOnWindow.lock());
        for (const auto& windowRef : WSPACE->windows) {
            if (getOverviewWindowToShow(windowRef.lock()) == selectedWindow) {
                closeOnWindow = selectedWindow;
                rememberSelection(selectedWindow);
                syncFocusedSelection();
                return;
            }
        }
    }

    if (const auto it = rememberedSelection.find(WSPACE->pWorkspace->m_id); it != rememberedSelection.end()) {
        const auto rememberedWindow = getOverviewWindowToShow(it->second.lock());
        if (rememberedWindow && rememberedWindow->m_workspace == WSPACE->pWorkspace && shouldShowOverviewWindow(rememberedWindow)) {
            for (const auto& windowRef : WSPACE->windows) {
                if (getOverviewWindowToShow(windowRef.lock()) == rememberedWindow) {
                    closeOnWindow = rememberedWindow;
                    syncFocusedSelection();
                    return;
                }
            }
        }
    }

    const auto focusedWindow = Desktop::focusState()->window();
    if (shouldShowOverviewWindow(focusedWindow) && focusedWindow->m_workspace == WSPACE->pWorkspace) {
        closeOnWindow = focusedWindow;
        rememberSelection(focusedWindow);
        syncFocusedSelection();
        return;
    }

    for (const auto& windowRef : WSPACE->windows) {
        const auto window = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(window))
            continue;

        closeOnWindow = window;
        rememberSelection(window);
        syncFocusedSelection();
        return;
    }

    closeOnWindow.reset();
}

void CScrollOverview::syncFocusedSelection() {
    const auto window = getOverviewWindowToShow(closeOnWindow.lock());
    if (!shouldShowOverviewWindow(window))
        return;

    closeOnWindow = window;

    if (Desktop::focusState()->window() == window && window->m_workspace == pMonitor->m_activeWorkspace)
        return;

    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_KEYBIND);
}

bool CScrollOverview::moveWindowSelection(const std::string& direction) {
    if (images.empty() || viewportCurrentWorkspace >= images.size() || direction.empty())
        return false;

    const bool MOVINGLEFT  = direction == "l";
    const bool MOVINGRIGHT = direction == "r";
    const bool MOVINGUP    = direction == "u";
    const bool MOVINGDOWN  = direction == "d";

    if (!MOVINGLEFT && !MOVINGRIGHT && !MOVINGUP && !MOVINGDOWN)
        return false;

    const auto& WORKSPACEIMAGE = images[viewportCurrentWorkspace];
    if (!WORKSPACEIMAGE || !WORKSPACEIMAGE->pWorkspace)
        return false;

    if (!closeOnWindow || closeOnWindow->m_workspace != WORKSPACEIMAGE->pWorkspace || !shouldShowOverviewWindow(closeOnWindow.lock()) || closeOnWindow->m_isFloating) {
        syncSelectionToViewport();
        if (!closeOnWindow || closeOnWindow->m_workspace != WORKSPACEIMAGE->pWorkspace || !shouldShowOverviewWindow(closeOnWindow.lock()) || closeOnWindow->m_isFloating)
            return false;
    }

    const auto CURRENT = getOverviewWindowToShow(closeOnWindow.lock());
    if (!CURRENT)
        return false;

    closeOnWindow = CURRENT;

    const auto CURRENTCENTER = CURRENT->middle();

    PHLWINDOW bestCandidate;
    float     bestPrimaryDistance   = std::numeric_limits<float>::max();
    float     bestSecondaryDistance = std::numeric_limits<float>::max();
    float     bestOverlap           = -1.F;
    bool      bestHasOverlap         = false;

    for (const auto& windowRef : WORKSPACEIMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW == CURRENT || WINDOW->m_isFloating)
            continue;

        if (WINDOW->m_workspace != WORKSPACEIMAGE->pWorkspace || WINDOW->m_monitor != pMonitor)
            continue;

        const auto WINDOWCENTER = WINDOW->middle();

        const float PRIMARYDISTANCE =
            MOVINGRIGHT ? WINDOWCENTER.x - CURRENTCENTER.x : MOVINGLEFT ? CURRENTCENTER.x - WINDOWCENTER.x : MOVINGDOWN ? WINDOWCENTER.y - CURRENTCENTER.y : CURRENTCENTER.y - WINDOWCENTER.y;

        if (PRIMARYDISTANCE <= 0.F)
            continue;

        const float OVERLAP = MOVINGLEFT || MOVINGRIGHT ? getOverviewVerticalOverlap(CURRENT, WINDOW) : getOverviewHorizontalOverlap(CURRENT, WINDOW);
        const bool  HASOVERLAP       = OVERLAP > 0.F;
        const float SECONDARYDISTANCE =
            MOVINGLEFT || MOVINGRIGHT ? std::abs(WINDOWCENTER.y - CURRENTCENTER.y) : std::abs(WINDOWCENTER.x - CURRENTCENTER.x);

        if ((MOVINGUP || MOVINGDOWN) && !HASOVERLAP)
            continue;

        if (!bestCandidate) {
            bestCandidate         = WINDOW;
            bestPrimaryDistance   = PRIMARYDISTANCE;
            bestSecondaryDistance = SECONDARYDISTANCE;
            bestOverlap           = OVERLAP;
            bestHasOverlap        = HASOVERLAP;
            continue;
        }

        if (HASOVERLAP != bestHasOverlap) {
            if (HASOVERLAP) {
                bestCandidate         = WINDOW;
                bestPrimaryDistance   = PRIMARYDISTANCE;
                bestSecondaryDistance = SECONDARYDISTANCE;
                bestOverlap           = OVERLAP;
                bestHasOverlap        = true;
            }

            continue;
        }

        if (PRIMARYDISTANCE < bestPrimaryDistance - 0.5F) {
            bestCandidate         = WINDOW;
            bestPrimaryDistance   = PRIMARYDISTANCE;
            bestSecondaryDistance = SECONDARYDISTANCE;
            bestOverlap           = OVERLAP;
            continue;
        }

        if (std::abs(PRIMARYDISTANCE - bestPrimaryDistance) <= 0.5F) {
            if ((HASOVERLAP && OVERLAP > bestOverlap + 0.5F) || (!HASOVERLAP && SECONDARYDISTANCE < bestSecondaryDistance - 0.5F)) {
                bestCandidate         = WINDOW;
                bestPrimaryDistance   = PRIMARYDISTANCE;
                bestSecondaryDistance = SECONDARYDISTANCE;
                bestOverlap           = OVERLAP;
            }
        }
    }

    if (!bestCandidate)
        return false;

    closeOnWindow = bestCandidate;
    rememberSelection(bestCandidate);
    syncFocusedSelection();
    damage();

    return true;
}

void CScrollOverview::forceSurfaceVisibility(SP<CWLSurfaceResource> surface) {
    if (!surface)
        return;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
    if (!HLSURFACE)
        return;

    for (auto& entry : forcedSurfaceVisibility) {
        if (entry.surface.lock() == surface) {
            HLSURFACE->m_visibleRegion = {};
            return;
        }
    }

    forcedSurfaceVisibility.push_back({surface, HLSURFACE->m_visibleRegion});
    HLSURFACE->m_visibleRegion = {};
}

void CScrollOverview::forceWindowSurfaceVisibility(PHLWINDOW window) {
    if (!window || !window->wlSurface() || !window->wlSurface()->resource())
        return;

    window->wlSurface()->resource()->breadthfirst([this](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { forceSurfaceVisibility(surface); }, nullptr);

    if (window->m_isX11 || !window->m_popupHead)
        return;

    window->m_popupHead->breadthfirst([this](WP<Desktop::View::CPopup> popup, void*) {
        if (!popup || !popup->aliveAndVisible() || !popup->wlSurface() || !popup->wlSurface()->resource())
            return;

        popup->wlSurface()->resource()->breadthfirst([this](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { forceSurfaceVisibility(surface); }, nullptr);
    }, nullptr);
}

void CScrollOverview::forceWindowVisible(PHLWINDOW window) {
    if (!window)
        return;

    for (auto& entry : forcedWindowVisibility) {
        if (entry.window == window) {
            window->m_hidden = false;
            return;
        }
    }

    forcedWindowVisibility.push_back({window, window->m_hidden});
    window->m_hidden = false;
}

void CScrollOverview::forceLayersAboveFullscreen() {
    if (!pMonitor)
        return;

    for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
        for (const auto& ls : pMonitor->m_layerSurfaceLayers[LAYER]) {
            if (!ls)
                continue;

            bool known = false;
            for (auto& entry : forcedLayerVisibility) {
                if (entry.layer == ls) {
                    known = true;
                    break;
                }
            }

            if (!known)
                forcedLayerVisibility.push_back({ls, ls->m_aboveFullscreen, ls->m_alpha->value()});

            if (!ls->m_aboveFullscreen)
                ls->m_aboveFullscreen = true;

            if (ls->m_alpha->value() != 1.F || ls->m_alpha->goal() != 1.F || ls->m_alpha->isBeingAnimated())
                ls->m_alpha->setValueAndWarp(1.F);
        }
    }
}

void CScrollOverview::restoreForcedSurfaceVisibility() {
    for (auto& entry : forcedSurfaceVisibility) {
        const auto SURFACE = entry.surface.lock();
        if (!SURFACE)
            continue;

        const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(SURFACE);
        if (!HLSURFACE)
            continue;

        HLSURFACE->m_visibleRegion = entry.visibleRegion;
    }

    forcedSurfaceVisibility.clear();
}

void CScrollOverview::restoreForcedWindowVisibility() {
    std::vector<SP<Desktop::View::CGroup>> groupsToRefresh;

    for (auto& entry : forcedWindowVisibility) {
        const auto WINDOW = entry.window.lock();
        if (!WINDOW)
            continue;

        if (WINDOW->m_group) {
            if (std::ranges::find(groupsToRefresh, WINDOW->m_group) == groupsToRefresh.end())
                groupsToRefresh.emplace_back(WINDOW->m_group);
            continue;
        }

        WINDOW->m_hidden = entry.hidden;
    }

    for (const auto& group : groupsToRefresh) {
        if (group)
            group->updateWindowVisibility();
    }

    forcedWindowVisibility.clear();
}

void CScrollOverview::restoreForcedLayerVisibility() {
    for (auto& entry : forcedLayerVisibility) {
        if (!entry.layer)
            continue;

        entry.layer->m_aboveFullscreen = entry.aboveFullscreen;

        const auto MONITOR = entry.layer->m_monitor.lock();
        if (!MONITOR) {
            entry.layer->m_alpha->setValueAndWarp(entry.alpha);
            continue;
        }

        const bool fullscreen = MONITOR->inFullscreenMode();
        const bool visible    = !fullscreen || entry.layer->m_layer >= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY || entry.layer->m_aboveFullscreen;
        entry.layer->m_alpha->setValueAndWarp(visible ? 1.F : 0.F);
    }

    forcedLayerVisibility.clear();
}

void CScrollOverview::applyInputConfigOverrides() {
    if (inputConfigOverridden)
        return;

    static auto* const* PNOWARPS                = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "cursor:no_warps")->getDataStaticPtr();
    static auto* const* PWARPONCHANGEWORKSPACE = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "cursor:warp_on_change_workspace")->getDataStaticPtr();
    static auto* const* PWARPONTOGGLESPECIAL   = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "cursor:warp_on_toggle_special")->getDataStaticPtr();
    static auto* const* PWARPBACKAFTERINPUT    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "cursor:warp_back_after_non_mouse_input")->getDataStaticPtr();
    static auto* const* PFOLLOWMOUSE           = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "input:follow_mouse")->getDataStaticPtr();

    previousNoWarps                    = **PNOWARPS;
    previousWarpOnChangeWorkspace      = **PWARPONCHANGEWORKSPACE;
    previousWarpOnToggleSpecial        = **PWARPONTOGGLESPECIAL;
    previousWarpBackAfterNonMouseInput = **PWARPBACKAFTERINPUT;
    previousFollowMouse   = **PFOLLOWMOUSE;
    inputConfigOverridden = true;

    **PNOWARPS                = 1;
    **PWARPONCHANGEWORKSPACE = 0;
    **PWARPONTOGGLESPECIAL   = 0;
    **PWARPBACKAFTERINPUT    = 0;
    **PFOLLOWMOUSE           = 0;
}

void CScrollOverview::restoreInputConfigOverrides() {
    if (!inputConfigOverridden)
        return;

    static auto* const* PNOWARPS                = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "cursor:no_warps")->getDataStaticPtr();
    static auto* const* PWARPONCHANGEWORKSPACE = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "cursor:warp_on_change_workspace")->getDataStaticPtr();
    static auto* const* PWARPONTOGGLESPECIAL   = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "cursor:warp_on_toggle_special")->getDataStaticPtr();
    static auto* const* PWARPBACKAFTERINPUT    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "cursor:warp_back_after_non_mouse_input")->getDataStaticPtr();
    static auto* const* PFOLLOWMOUSE           = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "input:follow_mouse")->getDataStaticPtr();

    **PNOWARPS                = previousNoWarps;
    **PWARPONCHANGEWORKSPACE = previousWarpOnChangeWorkspace;
    **PWARPONTOGGLESPECIAL   = previousWarpOnToggleSpecial;
    **PWARPBACKAFTERINPUT    = previousWarpBackAfterNonMouseInput;
    **PFOLLOWMOUSE           = previousFollowMouse;

    inputConfigOverridden = false;
}

void CScrollOverview::emitFullscreenVisibilityState(PHLWINDOW window, bool hideFullscreen) {
    if (emittingFullscreenVisibilityState)
        return;

    window = getOverviewWindowToShow(window);

    if (!validMapped(window) || !window->m_workspace || window->m_monitor != pMonitor) {
        if (g_pEventManager)
            g_pEventManager->postEvent(SHyprIPCEvent{.event = "fullscreen", .data = "0"});
        return;
    }

    if (!hideFullscreen || !window->isFullscreen()) {
        emittingFullscreenVisibilityState = true;
        Event::bus()->m_events.window.fullscreen.emit(window);
        emittingFullscreenVisibilityState = false;

        if (g_pEventManager)
            g_pEventManager->postEvent(SHyprIPCEvent{.event = "fullscreen", .data = window->isFullscreen() ? "1" : "0"});

        return;
    }

    const auto INTERNALFULLSCREEN = window->m_fullscreenState.internal;
    const auto CLIENTFULLSCREEN   = window->m_fullscreenState.client;
    const bool WORKSPACEFULL      = window->m_workspace->m_hasFullscreenWindow;
    const auto WORKSPACEMODE      = window->m_workspace->m_fullscreenMode;

    window->m_fullscreenState.internal         = FSMODE_NONE;
    window->m_fullscreenState.client           = FSMODE_NONE;
    window->m_workspace->m_hasFullscreenWindow = false;
    window->m_workspace->m_fullscreenMode      = FSMODE_NONE;

    emittingFullscreenVisibilityState = true;
    Event::bus()->m_events.window.fullscreen.emit(window);
    emittingFullscreenVisibilityState = false;

    if (g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{.event = "fullscreen", .data = "0"});

    window->m_fullscreenState.internal         = INTERNALFULLSCREEN;
    window->m_fullscreenState.client           = CLIENTFULLSCREEN;
    window->m_workspace->m_hasFullscreenWindow = WORKSPACEFULL;
    window->m_workspace->m_fullscreenMode      = WORKSPACEMODE;
}

static bool shouldBlurOverviewWindowBackground(const PHLWINDOW& window) {
    return window && g_pHyprRenderer->shouldBlur(window);
}

static bool shouldUseOverviewPrecomputedBlur(const PHLWINDOW& window) {
    return getHyprlandBlurNewOptimizations() && shouldShowOverviewWindow(window) && !window->m_isFloating && shouldBlurOverviewWindowBackground(window);
}

static bool shouldUseOverviewBlurFramebuffer(const PHLWINDOW& window) {
    return shouldUseOverviewPrecomputedBlur(window) ||
        (shouldShowOverviewWindow(window) && shouldBlurOverviewWindowBackground(window) && window->m_ruleApplicator->xray().valueOr(false));
}

static void renderOverviewWindowBlur(PHLMONITOR monitor, const CBox& windowBox, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur) {
    if (!monitor || alpha <= 0.F)
        return;

    CRegion blurDamage{windowBox};
    if (blurDamage.empty())
        return;

    CRegion drawDamage{CBox{{}, monitor->m_transformedSize}};

    auto* const SAVEDFB  = g_pHyprOpenGL->m_renderData.currentFB;
    CFramebuffer* const BLURREDFB = usePrecomputedBlur && g_pHyprOpenGL->m_renderData.pCurrentMonData ? &g_pHyprOpenGL->m_renderData.pCurrentMonData->blurFB :
                                                                                                         g_pHyprOpenGL->blurMainFramebufferWithDamage(alpha, &blurDamage);

    if (SAVEDFB)
        SAVEDFB->bind();

    if (!BLURREDFB)
        return;

    const auto BLURREDTEXTURE = BLURREDFB->getTexture();
    if (!BLURREDTEXTURE)
        return;

    CBox transformedBox = windowBox;
    transformedBox.transform(Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform)), monitor->m_transformedSize.x, monitor->m_transformedSize.y);

    const CBox monitorSpaceBox = {transformedBox.pos().x / monitor->m_pixelSize.x * monitor->m_transformedSize.x,
                                  transformedBox.pos().y / monitor->m_pixelSize.y * monitor->m_transformedSize.y,
                                  transformedBox.width / monitor->m_pixelSize.x * monitor->m_transformedSize.x,
                                  transformedBox.height / monitor->m_pixelSize.y * monitor->m_transformedSize.y};

    CHyprOpenGLImpl::STextureRenderData renderData;
    renderData.damage                     = &drawDamage;
    renderData.a                          = alpha;
    renderData.round                      = rounding;
    renderData.roundingPower              = roundingPower;
    renderData.allowCustomUV              = true;
    renderData.allowDim                   = false;

    g_pHyprOpenGL->pushMonitorTransformEnabled(true);
    const auto SAVEDRENDERMODIF                  = g_pHyprOpenGL->m_renderData.renderModif;
    const auto SAVEDUVTOPLEFT                    = g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft;
    const auto SAVEDUVBOTTOMRIGHT                = g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight;
    g_pHyprOpenGL->m_renderData.renderModif      = {};
    g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = monitorSpaceBox.pos() / monitor->m_transformedSize;
    g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = (monitorSpaceBox.pos() + monitorSpaceBox.size()) / monitor->m_transformedSize;
    g_pHyprOpenGL->renderTexture(BLURREDTEXTURE, windowBox, renderData);
    g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = SAVEDUVTOPLEFT;
    g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = SAVEDUVBOTTOMRIGHT;
    g_pHyprOpenGL->m_renderData.renderModif                 = SAVEDRENDERMODIF;
    g_pHyprOpenGL->popMonitorTransformEnabled();
}

void CScrollOverview::renderWorkspaceBackground(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode,
                                                const Time::steady_tp& now) {
    const auto& workspaceImage = images[workspaceIdx];
    if (!workspaceImage || !workspaceImage->pWorkspace)
        return;

    const auto WORKSPACEYOFFSET = (sc<long>(workspaceIdx) - sc<long>(activeIdx)) * workspacePitch;
    const auto WORKSPACEBOX     = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEYOFFSET);

    if (!overviewBoxIntersectsMonitor(WORKSPACEBOX, monitor))
        return;

    const auto workspace         = workspaceImage->pWorkspace;
    const bool WASVISIBLE        = workspace->m_visible;
    const bool WASFORCERENDERING = workspace->m_forceRendering;
    workspace->m_visible         = true;
    workspace->m_forceRendering  = true;

    auto restoreWorkspaceState = Hyprutils::Utils::CScopeGuard([workspace, WASVISIBLE, WASFORCERENDERING] {
        workspace->m_visible        = WASVISIBLE;
        workspace->m_forceRendering = WASFORCERENDERING;
    });

    renderOverviewWorkspaceShadow(monitor, WORKSPACEBOX, renderScale, wallpaperMode == 0);

    if (wallpaperMode != 0)
        renderWallpaperLayers(monitor, WORKSPACEBOX, renderScale, now);

    renderOverviewLayerLevel(monitor, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, WORKSPACEBOX, renderScale, now);
}

void CScrollOverview::renderWorkspaceLive(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now) {
    const auto& workspaceImage = images[workspaceIdx];
    if (!workspaceImage || !workspaceImage->pWorkspace)
        return;

    const auto WORKSPACEYOFFSET = (sc<long>(workspaceIdx) - sc<long>(activeIdx)) * workspacePitch;
    const auto WORKSPACEBOX     = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEYOFFSET);

    if (!overviewBoxIntersectsMonitor(WORKSPACEBOX, monitor))
        return;

    const auto workspace         = workspaceImage->pWorkspace;
    const bool WASVISIBLE        = workspace->m_visible;
    const bool WASFORCERENDERING = workspace->m_forceRendering;
    workspace->m_visible         = true;
    workspace->m_forceRendering  = true;

    auto restoreWorkspaceState = Hyprutils::Utils::CScopeGuard([workspace, WASVISIBLE, WASFORCERENDERING] {
        workspace->m_visible        = WASVISIBLE;
        workspace->m_forceRendering = WASFORCERENDERING;
    });

    const auto renderOverviewWindow = [&](const PHLWINDOW& window) {
        if (!shouldShowOverviewWindow(window))
            return;
        if (dragActiveWindow && window == getOverviewWindowToShow(dragActiveWindow.lock()))
            return;

        const auto windowBox = getOverviewWindowBox(window, monitor, renderScale, viewOffset->value(), WORKSPACEYOFFSET);
        if (!overviewBoxIntersectsMonitor(windowBox, monitor))
            return;

        renderWindowLive(monitor, window, windowBox, renderScale, now, &WORKSPACEBOX, shouldUseOverviewPrecomputedBlur(window));
    };

    const auto fullscreenWindow = getOverviewWindowToShow(workspace->getFullscreenWindow());
    if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace) {
        renderOverviewWindow(fullscreenWindow);
        renderOverviewPass(monitor);
        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!shouldShowOverviewWindow(window) || !window->m_isFloating || window == fullscreenWindow)
                continue;

            renderOverviewWindow(window);
        }
        return;
    }

    const auto renderWindowsByState = [&](bool fullscreen, bool floating) {
        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!window || window->isFullscreen() != fullscreen || window->m_isFloating != floating)
                continue;

            renderOverviewWindow(window);
        }
    };

    renderWindowsByState(false, false);
    renderWindowsByState(false, true);
    renderWindowsByState(true, false);
    renderWindowsByState(true, true);
}

void CScrollOverview::renderDraggedWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale, const Time::steady_tp& now) {
    const auto WINDOW = getOverviewWindowToShow(dragActiveWindow.lock());
    if (!shouldShowOverviewWindow(WINDOW) || !WINDOW->m_workspace)
        return;

    size_t workspaceIdx = 0;
    bool   found        = false;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace == WINDOW->m_workspace) {
            workspaceIdx = i;
            found        = true;
            break;
        }
    }

    if (!found)
        return;

    const auto WORKSPACEYOFFSET = (sc<long>(workspaceIdx) - sc<long>(activeIdx)) * workspacePitch;
    auto       windowBox        = getOverviewWindowBox(WINDOW, monitor, renderScale, viewOffset->value(), WORKSPACEYOFFSET);
    windowBox.translate((lastMousePosLocal - dragStartMouseLocal) * monitor->m_scale);

    if (!overviewBoxIntersectsMonitor(windowBox, monitor))
        return;

    renderWindowLive(monitor, WINDOW, windowBox, renderScale, now);
}

bool CScrollOverview::hasVisiblePrecomputedBlurWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale) const {
    if (!monitor)
        return false;

    const auto DRAGGEDWINDOW = getOverviewWindowToShow(dragActiveWindow.lock());

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || !workspaceImage->pWorkspace)
            continue;

        const auto WORKSPACEYOFFSET = (sc<long>(workspaceIdx) - sc<long>(activeIdx)) * workspacePitch;
        const auto WORKSPACEBOX     = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEYOFFSET);
        if (!overviewBoxIntersectsMonitor(WORKSPACEBOX, monitor))
            continue;

        const auto workspace = workspaceImage->pWorkspace;

        const auto isVisiblePrecomputedBlurWindow = [&](const PHLWINDOW& window) {
            if (window == DRAGGEDWINDOW || !shouldUseOverviewBlurFramebuffer(window))
                return false;

            const auto windowBox = getOverviewWindowBox(window, monitor, renderScale, viewOffset->value(), WORKSPACEYOFFSET);
            return overviewBoxIntersectsMonitor(windowBox, monitor);
        };

        const auto fullscreenWindow = getOverviewWindowToShow(workspace->getFullscreenWindow());
        if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace) {
            if (isVisiblePrecomputedBlurWindow(fullscreenWindow))
                return true;

            continue;
        }

        for (const auto& windowRef : workspaceImage->windows) {
            if (isVisiblePrecomputedBlurWindow(getOverviewWindowToShow(windowRef.lock())))
                return true;
        }
    }

    return false;
}

void CScrollOverview::renderPinnedFloatingWindows(PHLMONITOR monitor, float overviewScale, const Time::steady_tp& now) {
    if (!monitor)
        return;

    const auto TARGETOVERVIEWSCALE = getOverviewConfiguredScale();
    const auto ANIMATIONPROGRESS   = (1.F - TARGETOVERVIEWSCALE) > 0.001F ? (1.F - overviewScale) / (1.F - TARGETOVERVIEWSCALE) : 1.F;

    for (const auto& windowRef : pinnedFloatingWindows) {
        const auto window = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowPinnedFloatingOverviewWindow(window))
            continue;

        if (window->m_monitor != monitor)
            continue;

        float renderScale = 1.F;
        CBox  windowBox   = getPinnedFloatingOverviewWindowBox(monitor, window, TARGETOVERVIEWSCALE, ANIMATIONPROGRESS, &renderScale);

        if (!overviewBoxIntersectsMonitor(windowBox, monitor))
            continue;

        renderWindowLive(monitor, window, windowBox, renderScale, now);
    }
}

void CScrollOverview::renderWindowLive(PHLMONITOR monitor, PHLWINDOW window, const CBox& windowBox, float renderScale, const Time::steady_tp& now, const CBox* workspaceBox,
                                       bool usePrecomputedBlur) {
    if (!window)
        return;

    forceWindowVisible(window);
    forceWindowSurfaceVisibility(window);

    const bool  FULLSCREEN     = window->isFullscreen();
    const float TARGETOPACITY  = getOverviewWindowTargetOpacity(window);
    const bool  SHOULD_BLUR_BG = shouldBlurOverviewWindowBackground(window);

    if (!FULLSCREEN)
        renderOverviewWindowShadow(monitor, window, windowBox, renderScale, closeOnWindow == window);

    if (SHOULD_BLUR_BG) {
        renderOverviewPass(monitor);

        const float BLURALPHA     = std::sqrt(window->m_alpha->value());
        const int   BLURROUNDING  = FULLSCREEN ? 0 : sc<int>(std::round(getHyprlandDecorationRounding() * monitor->m_scale * renderScale));
        const float ROUNDINGPOWER = FULLSCREEN ? 2.F : getHyprlandDecorationRoundingPower();
        renderOverviewWindowBlur(monitor, windowBox, BLURROUNDING, ROUNDINGPOWER, BLURALPHA, usePrecomputedBlur || window->m_ruleApplicator->xray().valueOr(false));
    }

    SRenderModifData modif;
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_SCALE, renderScale);
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, Vector2D{windowBox.x / monitor->m_scale, windowBox.y / monitor->m_scale});

    std::vector<SSurfaceOpacityOverride> surfaceOpacityOverrides;
    surfaceOpacityOverrides.reserve(4);
    overrideWindowSurfaceOpacity(window, surfaceOpacityOverrides, TARGETOPACITY);
    auto restoreSurfaceOpacities = Hyprutils::Utils::CScopeGuard([&surfaceOpacityOverrides] { restoreSurfaceOpacityOverrides(surfaceOpacityOverrides); });

    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
    const auto firstWindowPassElement = g_pHyprRenderer->m_renderPass.m_passElements.size();
    g_pHyprRenderer->renderWindow(window, monitor, now, true, RENDER_PASS_ALL, true, true);
    if (!FULLSCREEN)
        roundStandaloneWindowPassElements(window, monitor, renderScale, firstWindowPassElement);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = SRenderModifData{}}));
    if (!FULLSCREEN) {
        renderOverviewWindowBorder(monitor, window, windowBox, renderScale, closeOnWindow == window);
        if (workspaceBox)
            renderOverviewGroupTabs(monitor, window, windowBox, *workspaceBox, renderScale);
    }

    renderOverviewPass(monitor);
}

void CScrollOverview::redrawAll(bool forcelowres) {
    rebuildWorkspaceImages();
    seedRememberedSelections();

    for (const auto& img : images) {
        img->windows.clear();
    }
    pinnedFloatingWindows.clear();

    std::unordered_map<WORKSPACEID, SP<SWorkspaceImage>> imagesByWorkspace;
    imagesByWorkspace.reserve(images.size());

    for (const auto& img : images) {
        if (img && img->pWorkspace)
            imagesByWorkspace.emplace(img->pWorkspace->m_id, img);
    }

    std::vector<PHLWINDOW> addedWindows;
    addedWindows.reserve(g_pCompositor->m_windows.size());

    std::vector<PHLWINDOW> addedPinnedFloatingWindows;
    addedPinnedFloatingWindows.reserve(g_pCompositor->m_windows.size());

    const auto addOverviewWindow = [&](const PHLWINDOW& window) {
        const auto overviewWindow = getOverviewWindowToShow(window);
        if (!shouldShowOverviewWindow(overviewWindow) || !overviewWindow->m_workspace)
            return;

        if (std::ranges::find(addedWindows, overviewWindow) != addedWindows.end())
            return;

        const auto imageIt = imagesByWorkspace.find(overviewWindow->m_workspace->m_id);
        if (imageIt == imagesByWorkspace.end())
            return;

        addedWindows.emplace_back(overviewWindow);
        imageIt->second->windows.emplace_back(overviewWindow);
    };

    const auto addPinnedFloatingWindow = [&](const PHLWINDOW& window) {
        const auto overviewWindow = getOverviewWindowToShow(window);
        if (!shouldShowPinnedFloatingOverviewWindow(overviewWindow))
            return;

        if (std::ranges::find(addedPinnedFloatingWindows, overviewWindow) != addedPinnedFloatingWindows.end())
            return;

        addedPinnedFloatingWindows.emplace_back(overviewWindow);
        pinnedFloatingWindows.emplace_back(overviewWindow);
    };

    for (const auto& window : g_pCompositor->m_windows) {
        if (getOverviewWindowToShow(window) != window)
            continue;

        addOverviewWindow(window);
        addPinnedFloatingWindow(window);
    }

    for (const auto& window : g_pCompositor->m_windows) {
        if (getOverviewWindowToShow(window) == window)
            continue;

        addOverviewWindow(window);
        addPinnedFloatingWindow(window);
    }
}

void CScrollOverview::damage() {
    blockDamageReporting = true;
    g_pHyprRenderer->damageMonitor(pMonitor.lock());
    blockDamageReporting = false;
}

void CScrollOverview::markBlurDirty() {
    overviewBlurDirty = true;
}

void CScrollOverview::onDamageReported() {
    return;
}

bool CScrollOverview::isVisibleRealtimePreviewWindow(const PHLWINDOW& window) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !window || !window->isFullscreen() || window->m_monitor != MONITOR)
        return false;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || !workspaceImage->pWorkspace || workspaceImage->pWorkspace != window->m_workspace)
            continue;

        const auto fullscreenWindow = getOverviewWindowToShow(workspaceImage->pWorkspace->getFullscreenWindow());
        if (fullscreenWindow != window)
            return false;

        const auto WORKSPACEYOFFSET = (sc<long>(workspaceIdx) - sc<long>(ACTIVEIDX)) * PITCH;
        const auto WINDOWBOX        = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), WORKSPACEYOFFSET);
        return overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR);
    }

    return false;
}

bool CScrollOverview::shouldAllowRealtimePreviewFrame() const {
    if (lastRealtimePreviewFrame.time_since_epoch().count() == 0)
        return true;

    return Time::steadyNow() - lastRealtimePreviewFrame >= OVERVIEW_WINDOW_FRAME_INTERVAL;
}

bool CScrollOverview::shouldAllowRealtimePreviewSchedule() {
    if (closing)
        return true;

    if (scale->isBeingAnimated() || viewOffset->isBeingAnimated())
        return true;

    if (realtimePreviewFrameQueued) {
        scheduleRealtimePreviewFrame();
        return false;
    }

    if (shouldAllowRealtimePreviewFrame()) {
        realtimePreviewFrameQueued = true;
        return true;
    }

    scheduleRealtimePreviewFrame();
    return false;
}

void CScrollOverview::schedulePreviewFrameAfter(std::chrono::milliseconds delay) {
    if (!realtimePreviewTimer)
        return;

    const auto DELAY = std::max<int>(1, sc<int>(delay.count()));
    const auto DUE   = Time::steadyNow() + std::chrono::milliseconds(DELAY);

    if (realtimePreviewTimerArmed && realtimePreviewTimerDue <= DUE)
        return;

    realtimePreviewTimerArmed = true;
    realtimePreviewTimerDue   = DUE;
    wl_event_source_timer_update(realtimePreviewTimer, DELAY);
}

void CScrollOverview::scheduleMinimumPreviewFrame() {
    schedulePreviewFrameAfter(getOverviewIdleFrameInterval());
}

void CScrollOverview::scheduleRealtimePreviewFrame() {
    const auto NOW     = Time::steadyNow();
    const auto ELAPSED = lastRealtimePreviewFrame.time_since_epoch().count() == 0 ? OVERVIEW_WINDOW_FRAME_INTERVAL :
                                                                                   std::chrono::duration_cast<std::chrono::milliseconds>(NOW - lastRealtimePreviewFrame);
    const auto DELAY   = OVERVIEW_WINDOW_FRAME_INTERVAL - std::min(ELAPSED, OVERVIEW_WINDOW_FRAME_INTERVAL);
    schedulePreviewFrameAfter(DELAY);
}

int CScrollOverview::realtimePreviewTimerCallback(void* data) {
    const auto OVERVIEW = sc<CScrollOverview*>(data);
    if (!OVERVIEW)
        return 0;

    OVERVIEW->realtimePreviewTimerArmed  = false;
    OVERVIEW->realtimePreviewTimerDue    = {};
    OVERVIEW->realtimePreviewFrameQueued = false;
    OVERVIEW->damage();
    OVERVIEW->scheduleMinimumPreviewFrame();
    return 0;
}

bool CScrollOverview::shouldSuppressRenderDamage() const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing)
        return false;

    if (scale->isBeingAnimated() || viewOffset->isBeingAnimated())
        return false;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE);
    const auto DRAGGED   = getOverviewWindowToShow(dragActiveWindow.lock());

    const auto isVisibleAnimatedWindow = [&](const PHLWINDOW& window, float workspaceYOffset) {
        if (!shouldShowOverviewWindow(window) || window == DRAGGED)
            return false;

        const auto WINDOWBOX = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), workspaceYOffset);
        return overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR) && windowHasOverviewAnimation(window);
    };

    for (const auto& windowRef : pinnedFloatingWindows) {
        const auto window = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowPinnedFloatingOverviewWindow(window) || window->m_monitor != MONITOR)
            continue;

        if (windowHasOverviewAnimation(window))
            return false;
    }

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || !workspaceImage->pWorkspace)
            continue;

        const auto WORKSPACEYOFFSET = (sc<long>(workspaceIdx) - sc<long>(ACTIVEIDX)) * PITCH;
        const auto WORKSPACEBOX     = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), WORKSPACEYOFFSET);
        if (!overviewBoxIntersectsMonitor(WORKSPACEBOX, MONITOR))
            continue;

        const auto workspace        = workspaceImage->pWorkspace;
        const auto fullscreenWindow = getOverviewWindowToShow(workspace->getFullscreenWindow());
        if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace) {
            if (isVisibleAnimatedWindow(fullscreenWindow, WORKSPACEYOFFSET))
                return false;

            for (const auto& windowRef : workspaceImage->windows) {
                const auto window = getOverviewWindowToShow(windowRef.lock());
                if (window && window->m_isFloating && isVisibleAnimatedWindow(window, WORKSPACEYOFFSET))
                    return false;
            }

            continue;
        }

        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (isVisibleAnimatedWindow(window, WORKSPACEYOFFSET))
                return false;
        }
    }

    for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM}) {
        for (const auto& layerRef : MONITOR->m_layerSurfaceLayers[LAYER]) {
            if (layerHasOverviewAnimation(layerRef.lock()))
                return false;
        }
    }

    return true;
}

void CScrollOverview::sendOverviewFrameCallbacks(const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing)
        return;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE);
    const auto DRAGGED   = getOverviewWindowToShow(dragActiveWindow.lock());
    const bool CANFRAMEWINDOWS = shouldAllowRealtimePreviewFrame();
    bool       sentWindowFrame = false;

    const bool PREVSENDINGFRAMECALLBACKS = sendingOverviewFrameCallbacks;
    sendingOverviewFrameCallbacks        = CANFRAMEWINDOWS;
    auto resetSendingFrameCallbacks      = Hyprutils::Utils::CScopeGuard([this, PREVSENDINGFRAMECALLBACKS] { sendingOverviewFrameCallbacks = PREVSENDINGFRAMECALLBACKS; });

    const auto frameWindow = [&](const PHLWINDOW& window, float workspaceYOffset) {
        if (!shouldShowOverviewWindow(window) || window == DRAGGED)
            return;

        const auto WINDOWBOX = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), workspaceYOffset);
        if (!overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR))
            return;

        if (!CANFRAMEWINDOWS) {
            scheduleRealtimePreviewFrame();
            return;
        }

        surfaceTreePresent(window->wlSurface() ? window->wlSurface()->resource() : nullptr, MONITOR, now);
        sentWindowFrame = true;
    };

    for (const auto& windowRef : pinnedFloatingWindows) {
        const auto window = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowPinnedFloatingOverviewWindow(window) || window->m_monitor != MONITOR)
            continue;

        if (!CANFRAMEWINDOWS) {
            scheduleRealtimePreviewFrame();
            continue;
        }

        surfaceTreePresent(window->wlSurface() ? window->wlSurface()->resource() : nullptr, MONITOR, now);
        sentWindowFrame = true;
    }

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || !workspaceImage->pWorkspace)
            continue;

        const auto WORKSPACEYOFFSET = (sc<long>(workspaceIdx) - sc<long>(ACTIVEIDX)) * PITCH;
        const auto WORKSPACEBOX     = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), WORKSPACEYOFFSET);
        if (!overviewBoxIntersectsMonitor(WORKSPACEBOX, MONITOR))
            continue;

        const auto workspace        = workspaceImage->pWorkspace;
        const auto fullscreenWindow = getOverviewWindowToShow(workspace->getFullscreenWindow());
        if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace) {
            frameWindow(fullscreenWindow, WORKSPACEYOFFSET);
            for (const auto& windowRef : workspaceImage->windows) {
                const auto window = getOverviewWindowToShow(windowRef.lock());
                if (window && window->m_isFloating)
                    frameWindow(window, WORKSPACEYOFFSET);
            }
            continue;
        }

        for (const auto& windowRef : workspaceImage->windows) {
            frameWindow(getOverviewWindowToShow(windowRef.lock()), WORKSPACEYOFFSET);
        }
    }

    if (sentWindowFrame)
        lastRealtimePreviewFrame = now;

    realtimePreviewFrameQueued = false;

    for (const auto LAYER :
         {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
        for (const auto& layerRef : MONITOR->m_layerSurfaceLayers[LAYER]) {
            const auto layer = layerRef.lock();
            if (Desktop::View::validMapped(layer) && surfaceTreeHasFrameCallbacks(layer->wlSurface() ? layer->wlSurface()->resource() : nullptr))
                surfaceTreePresent(layer->wlSurface() ? layer->wlSurface()->resource() : nullptr, MONITOR, now);
        }
    }
}

bool CScrollOverview::shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing || !surface)
        return true;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
    if (!HLSURFACE)
        return true;

    auto view = HLSURFACE->view();
    if (!view)
        return true;

    auto layerOwner  = Desktop::View::CLayerSurface::fromView(view);
    auto windowOwner = Desktop::View::CWindow::fromView(view);

    if (!layerOwner && !windowOwner) {
        if (const auto POPUP = Desktop::View::CPopup::fromView(view)) {
            if (const auto T1OWNER = POPUP->getT1Owner(); T1OWNER && T1OWNER->view()) {
                layerOwner  = Desktop::View::CLayerSurface::fromView(T1OWNER->view());
                windowOwner = Desktop::View::CWindow::fromView(T1OWNER->view());
            }
        }
    }

    if (layerOwner)
        return true;

    auto window = getOverviewWindowToShow(windowOwner);
    if (!window || window->m_monitor != MONITOR)
        return true;

    if (shouldShowPinnedFloatingOverviewWindow(window)) {
        if (sendingOverviewFrameCallbacks)
            return true;

        scheduleRealtimePreviewFrame();
        return false;
    }

    if (!shouldShowOverviewWindow(window) || !window->m_workspace)
        return true;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || workspaceImage->pWorkspace != window->m_workspace)
            continue;

        const auto fullscreenWindow = getOverviewWindowToShow(workspaceImage->pWorkspace->getFullscreenWindow());
        if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspaceImage->pWorkspace && fullscreenWindow != window && !window->m_isFloating)
            return false;

        const auto WORKSPACEYOFFSET = (sc<long>(workspaceIdx) - sc<long>(ACTIVEIDX)) * PITCH;
        const auto WINDOWBOX        = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), WORKSPACEYOFFSET);
        if (!overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR))
            return false;

        if (sendingOverviewFrameCallbacks)
            return true;

        scheduleRealtimePreviewFrame();
        return false;
    }

    return false;
}

bool CScrollOverview::shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing || !surface)
        return true;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
    if (!HLSURFACE)
        return true;

    auto view = HLSURFACE->view();
    if (!view)
        return true;

    auto layerOwner = Desktop::View::CLayerSurface::fromView(view);
    auto windowOwner = Desktop::View::CWindow::fromView(view);

    if (!layerOwner && !windowOwner) {
        if (const auto POPUP = Desktop::View::CPopup::fromView(view)) {
            if (const auto T1OWNER = POPUP->getT1Owner(); T1OWNER && T1OWNER->view()) {
                layerOwner  = Desktop::View::CLayerSurface::fromView(T1OWNER->view());
                windowOwner = Desktop::View::CWindow::fromView(T1OWNER->view());
            }
        }
    }

    if (layerOwner) {
        if (layerOwner->m_monitor != MONITOR)
            return false;

        if (layerOwner->m_layer > ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)
            return false;

        markBlurDirty();
        return true;
    }

    if (!windowOwner)
        return true;

    auto window = getOverviewWindowToShow(windowOwner);
    if (shouldShowPinnedFloatingOverviewWindow(window)) {
        if (window->m_monitor != MONITOR)
            return false;

        if (!realtimePreviewFrameQueued && shouldAllowRealtimePreviewFrame())
            return true;

        scheduleRealtimePreviewFrame();
        return false;
    }

    if (!shouldShowOverviewWindow(window) || window->m_monitor != MONITOR || !window->m_workspace)
        return false;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || workspaceImage->pWorkspace != window->m_workspace)
            continue;

        const auto fullscreenWindow = getOverviewWindowToShow(workspaceImage->pWorkspace->getFullscreenWindow());
        if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspaceImage->pWorkspace && fullscreenWindow != window && !window->m_isFloating)
            return false;

        const auto WORKSPACEYOFFSET = (sc<long>(workspaceIdx) - sc<long>(ACTIVEIDX)) * PITCH;
        const auto WINDOWBOX        = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), WORKSPACEYOFFSET);
        if (!overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR))
            return false;

        if (!realtimePreviewFrameQueued && shouldAllowRealtimePreviewFrame())
            return true;

        scheduleRealtimePreviewFrame();
        return false;

    }

    return false;
}

void CScrollOverview::close() {
    closing = true;

    const auto SELECTEDWORKSPACE =
        viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] ? images[viewportCurrentWorkspace]->pWorkspace : PHLWORKSPACE{};

    if (!closeOnWindow && (!SELECTEDWORKSPACE || SELECTEDWORKSPACE == pMonitor->m_activeWorkspace)) {
        const auto FOCUSEDWINDOW = getOverviewWindowToShow(Desktop::focusState()->window());
        if (!SELECTEDWORKSPACE || (FOCUSEDWINDOW && FOCUSEDWINDOW->m_workspace == SELECTEDWORKSPACE))
            closeOnWindow = FOCUSEDWINDOW;
    }

    closeOnWindow = getOverviewWindowToShow(closeOnWindow.lock());

    if (!closeOnWindow) {
        const auto ACTIVEIDX = activeWorkspaceIndex();
        const auto FINALPITCH = getWorkspaceRenderedPitch(pMonitor.lock(), 1.F);
        *viewOffset = Vector2D{};

        for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
            if (!images[workspaceIdx] || images[workspaceIdx]->pWorkspace != SELECTEDWORKSPACE)
                continue;

            *viewOffset = Vector2D{0.F, (sc<long>(workspaceIdx) - sc<long>(ACTIVEIDX)) * FINALPITCH};
            break;
        }

        if (SELECTEDWORKSPACE && SELECTEDWORKSPACE != pMonitor->m_activeWorkspace)
            pMonitor->changeWorkspace(SELECTEDWORKSPACE, false, true, true);
    } else if (closeOnWindow == Desktop::focusState()->window() && closeOnWindow->m_workspace == pMonitor->m_activeWorkspace)
        *viewOffset = Vector2D{};
    else {

        if (closeOnWindow->m_workspace != pMonitor->m_activeWorkspace)
            pMonitor->changeWorkspace(closeOnWindow->m_workspace, false, true, true);

        Desktop::focusState()->fullWindowFocus(closeOnWindow.lock(), Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

        const auto ACTIVEIDX = activeWorkspaceIndex();
        const auto FINALPITCH = getWorkspaceRenderedPitch(pMonitor.lock(), 1.F);
        bool       found      = false;
        const auto selectedWindow = getOverviewWindowToShow(closeOnWindow.lock());
        for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
            const auto& wimg = images[workspaceIdx];
            for (const auto& windowRef : wimg->windows) {
                const auto window = getOverviewWindowToShow(windowRef.lock());
                if (window == selectedWindow && window) {
                    *viewOffset = Vector2D{0.F, (sc<long>(workspaceIdx) - sc<long>(ACTIVEIDX)) * FINALPITCH};
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
    }

    const auto FINALWINDOW = getOverviewWindowToShow(closeOnWindow.lock());
    emitFullscreenVisibilityState(FINALWINDOW && FINALWINDOW->m_workspace == pMonitor->m_activeWorkspace ? FINALWINDOW : PHLWINDOW{}, false);

    *scale = 1.F;

    scale->setCallbackOnEnd(removeOverview);
}

void CScrollOverview::onPreRender() {
    if (pMonitor)
        pMonitor->m_solitaryClient.reset();

    forceLayersAboveFullscreen();

    if (closing)
        return;

    if (workspaceSyncPending || (pMonitor && pMonitor->m_activeWorkspace && pMonitor->m_activeWorkspace != startedOn)) {
        workspaceSyncPending = false;
        rebuildPending       = false;
        markBlurDirty();
        onWorkspaceChange();
        emitFullscreenVisibilityState(Desktop::focusState()->window(), true);
        return;
    }

    if (rebuildPending) {
        rebuildPending = false;
        markBlurDirty();
        redrawAll();
        syncSelectionToViewport();
        damage();
        return;
    }
}

void CScrollOverview::onWorkspaceChange() {
    if (!pMonitor || !pMonitor->m_activeWorkspace)
        return;

    const auto previousActiveIdx = activeWorkspaceIndex();

    startedOn = pMonitor->m_activeWorkspace;
    redrawAll();
    viewportCurrentWorkspace = activeWorkspaceIndex();
    viewOffset->setValueAndWarp(Vector2D{0.F, (sc<long>(previousActiveIdx) - sc<long>(viewportCurrentWorkspace)) * getWorkspaceLogicalPitch(pMonitor.lock(), scale->value())});
    *viewOffset = Vector2D{};
    syncSelectionToViewport();
    markBlurDirty();
    damage();
}

void CScrollOverview::render() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    const bool PREVBLOCKSURFACEFEEDBACK       = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    g_pHyprRenderer->m_bBlockSurfaceFeedback  = true;
    auto restoreSurfaceFeedback               = Hyprutils::Utils::CScopeGuard([PREVBLOCKSURFACEFEEDBACK] { g_pHyprRenderer->m_bBlockSurfaceFeedback = PREVBLOCKSURFACEFEEDBACK; });

    const auto NOW       = Time::steadyNow();
    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE);

    const auto VIEWOFFSET = viewOffset->value();
    if (!overviewBlurStateValid || std::abs(lastOverviewBlurScale - SCALE) > 0.001F || lastOverviewBlurViewOffset.distanceSq(VIEWOFFSET) > 0.001F) {
        markBlurDirty();
        overviewBlurStateValid     = true;
        lastOverviewBlurScale      = SCALE;
        lastOverviewBlurViewOffset = VIEWOFFSET;
    }

    const auto WALLPAPERMODE = getWallpaperMode();

    if (WALLPAPERMODE == 0) {
        g_pHyprRenderer->renderBackground(MONITOR);

        for (auto const& ls : MONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]) {
            if (!Desktop::View::validMapped(ls.lock()))
                continue;

            g_pHyprRenderer->renderLayer(ls.lock(), MONITOR, NOW);
        }
    } else if (WALLPAPERMODE == 2) {
        renderWallpaperLayers(MONITOR, getOverviewWorkspaceBox(MONITOR, 1.F, Vector2D{}, 0.F), 1.F, NOW);
    } else
        g_pHyprOpenGL->clear(CHyprColor{0.F, 0.F, 0.F, 1.F});

    if (getOverviewBlur() && WALLPAPERMODE != 1) {
        CRectPassElement::SRectData blurData;
        blurData.box           = CBox{{}, MONITOR->m_size * MONITOR->m_scale};
        blurData.color         = CHyprColor{0.F, 0.F, 0.F, 0.F};
        blurData.blur          = true;
        blurData.blurA         = 1.F;
        blurData.round         = 0;
        blurData.roundingPower = 2.F;
        blurData.xray          = false;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(blurData));
        renderOverviewPass(MONITOR);
    }

    Event::bus()->m_events.render.stage.emit(RENDER_POST_WALLPAPER);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        renderWorkspaceBackground(MONITOR, workspaceIdx, ACTIVEIDX, PITCH, SCALE, WALLPAPERMODE, NOW);
    }

    const bool NEEDS_PRECOMPUTED_BLUR = hasVisiblePrecomputedBlurWindow(MONITOR, ACTIVEIDX, PITCH, SCALE);
    if (NEEDS_PRECOMPUTED_BLUR && overviewBlurDirty)
        g_pHyprRenderer->m_renderPass.add(makeUnique<CPreBlurElement>());

    renderOverviewPass(MONITOR);

    if (NEEDS_PRECOMPUTED_BLUR)
        overviewBlurDirty = false;

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        renderWorkspaceLive(MONITOR, workspaceIdx, ACTIVEIDX, PITCH, SCALE, WALLPAPERMODE, NOW);
    }

    renderDraggedWindow(MONITOR, ACTIVEIDX, PITCH, SCALE, NOW);
    renderPinnedFloatingWindows(MONITOR, SCALE, NOW);

    for (auto const& ls : MONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP]) {
        if (!Desktop::View::validMapped(ls.lock()))
            continue;

        g_pHyprRenderer->renderLayer(ls.lock(), MONITOR, NOW);
    }

    sendOverviewFrameCallbacks(NOW);
}

void CScrollOverview::fullRender() {
    return;
}

static float hyprlerp(const float& from, const float& to, const float perc) {
    return (to - from) * perc + from;
}

static Vector2D hyprlerp(const Vector2D& from, const Vector2D& to, const float perc) {
    return Vector2D{hyprlerp(from.x, to.x, perc), hyprlerp(from.y, to.y, perc)};
}

void CScrollOverview::setClosing(bool closing_) {
    closing = closing_;
}

void CScrollOverview::resetSwipe() {
    if (closing) {
        close();
        return;
    }

    (*scale)    = getOverviewConfiguredScale();
    m_isSwiping = false;
}

void CScrollOverview::onSwipeUpdate(double delta) {
    static auto* const* PDISTANCE    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:gesture_distance")->getDataStaticPtr();

    m_isSwiping = true;

    const float PERC = closing ? std::clamp(delta / (double)**PDISTANCE, 0.0, 1.0) : 1.0 - std::clamp(delta / (double)**PDISTANCE, 0.0, 1.0);

    scale->setValueAndWarp(hyprlerp(1.F, getOverviewConfiguredScale(), PERC));
}

void CScrollOverview::onSwipeEnd() {
    if (closing) {
        close();
        return;
    }

    (*scale)    = getOverviewConfiguredScale();
    m_isSwiping = false;
}
