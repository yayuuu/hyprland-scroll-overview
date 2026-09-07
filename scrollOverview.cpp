#include "scrollOverview.hpp"
#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <dlfcn.h>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_set>
#include <linux/input-event-codes.h>
#include <state/MonitorState.hpp>
#include <state/WorkspaceState.hpp>
#define private public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/pass/PreBlurElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/types.hpp>
#include <hyprland/src/render/decorations/CHyprGroupBarDecoration.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#undef protected
#undef private
#include "Config.hpp"
#include "DropIndicator.hpp"
#include "NativeDrag.hpp"
#include "OverviewOpen.hpp"
#include "OverviewPassElement.hpp"
#include "OverviewRender.hpp"
#include "Window.hpp"

static PHLWINDOW getOverviewFullscreenVisibilityWindow(const PHLWORKSPACE& workspace, const PHLWINDOW& fallback = {});
static constexpr const char* OVERVIEW_SUBMAP = "scrolloverview";
static constexpr auto        POST_DROP_PSEUDO_FOCUS_DURATION = std::chrono::milliseconds(100);
static CScrollOverview*             g_pointerGrabOverview = nullptr;
static bool                         g_finishingAdoptedNativeDrag    = false;
static std::unordered_set<uint32_t> g_topLayerPointerButtons;
static PHLWINDOWREF                 g_pseudoFocusedWindow;
static Time::steady_tp              g_pseudoFocusUntil = {};

static void restoreActiveWorkspaceVisibility() {
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor)
            continue;

        for (const auto& workspace : {monitor->m_activeWorkspace, monitor->m_activeSpecialWorkspace}) {
            if (!workspace)
                continue;

            workspace->m_visible = true;
            workspace->m_alpha->setValueAndWarp(1.F);
            workspace->m_renderOffset->setValueAndWarp(Vector2D{});
        }

        g_pHyprRenderer->damageMonitor(monitor);
    }
}

static void releaseTopLayerPointerButtons(uint32_t timeMs) {
    if (g_topLayerPointerButtons.empty())
        return;

    for (const auto button : g_topLayerPointerButtons)
        g_pSeatManager->sendPointerButton(timeMs, button, WL_POINTER_BUTTON_STATE_RELEASED);

    g_topLayerPointerButtons.clear();
    g_pSeatManager->sendPointerFrame();
}

static void removeOverview(CScrollOverview* overview) {
    const auto PMONITOR = overview ? overview->pMonitor.lock() : PHLMONITOR{};
    unregisterScrollOverview(overview);
    if (scrollOverviews().empty())
        disableScrollOverviewHooks();

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

static bool hasOverviewSubmap() {
    return g_pKeybindManager && std::ranges::any_of(g_pKeybindManager->m_keybinds, [](const auto& keybind) { return keybind && keybind->submap.name == OVERVIEW_SUBMAP; });
}

static bool isOverviewSubmapActive() {
    return g_pKeybindManager && g_pKeybindManager->getCurrentSubmap().name == OVERVIEW_SUBMAP;
}

static bool hasApplicableScrollKeybind(const IPointer::SAxisEvent& event) {
    if (!g_pKeybindManager || !g_pInputManager || event.source != WL_POINTER_AXIS_SOURCE_WHEEL || event.delta == 0.0)
        return false;

    std::string key;
    if (event.axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        key = event.delta > 0 ? "mouse_down" : "mouse_up";
    else if (event.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        key = event.delta < 0 ? "mouse_left" : "mouse_right";
    else
        return false;

    const auto MODS   = g_pInputManager->getModsFromAllKBs();
    const auto SUBMAP = g_pKeybindManager->getCurrentSubmap();
    return std::ranges::any_of(g_pKeybindManager->m_keybinds, [&](const auto& keybind) {
        return keybind && keybind->enabled && !keybind->shadowed && keybind->key == key && (keybind->modmask == MODS || keybind->ignoreMods) &&
            (keybind->submap.name == SUBMAP.name || keybind->submapUniversal);
    });
}

static bool scrollKeybindIsThrottled() {
    if (!g_pKeybindManager)
        return false;

    return g_pKeybindManager->m_scrollTimer.getMillis() < ScrollOverview::Config::getValue<int>("binds:scroll_event_delay");
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

static constexpr const char* OVERVIEW_INSERT_FADE_BEZIER = "scrolloverviewWorkspaceInsertFade";
static constexpr const char* OVERVIEW_REMOVE_FADE_BEZIER = "scrolloverviewWorkspaceRemoveFade";

static bool isPointerOnTopLayer(PHLMONITOR monitor) {
    if (!monitor)
        return false;

    const auto MOUSECOORDS = g_pInputManager->getMouseCoordsInternal();
    Vector2D   surfaceCoords;
    PHLLS      layerSurface;

    if (Desktop::viewState()->hitTest().layerSurfaceAt(MOUSECOORDS, &monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords, &layerSurface))
        return true;

    return !!Desktop::viewState()->hitTest().layerSurfaceAt(MOUSECOORDS, &monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &layerSurface);
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

    return window->positionAnimation()->isBeingAnimated() || window->sizeAnimation()->isBeingAnimated() || window->m_alpha.isBeingAnimated() ||
        window->m_borderFadeAnimationProgress->isBeingAnimated() || window->m_borderAngleAnimationProgress->isBeingAnimated() || window->m_dimPercent->isBeingAnimated() ||
        window->m_shadowFadeAnimationProgress->isBeingAnimated();
}

static bool layerHasOverviewAnimation(const PHLLS& layer) {
    if (!Desktop::View::validMapped(layer))
        return false;

    return layer->positionAnimation()->isBeingAnimated() || layer->sizeAnimation()->isBeingAnimated() || layer->m_alpha.isBeingAnimated();
}

static Vector2D axisOffsetVector(float offset, ScrollOverview::Config::ELayout layout);

static CBox getOverviewBox(CBox box, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout,
                           bool round = true) {
    const auto MONITORSCALE    = monitor->m_scale;
    const auto VIEWPORT_CENTER = CBox{{}, monitor->m_size * MONITORSCALE}.middle();

    box = {box.pos() * MONITORSCALE, box.size() * MONITORSCALE};
    box.translate(-VIEWPORT_CENTER).scale(scale).translate(VIEWPORT_CENTER).translate(-viewOffset * scale * MONITORSCALE).translate(axisOffsetVector(offset, layout));
    if (round)
        box.round();

    return box;
}

static CBox getOverviewGlobalBox(CBox globalBox, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout,
                                 bool round = true) {
    return getOverviewBox({globalBox.pos() - monitor->m_position, globalBox.size()}, monitor, scale, viewOffset, offset, layout, round);
}

static CBox getOverviewWindowBox(const PHLWINDOW& window, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout,
                                 bool round = true) {
    if (!window)
        return {};

    CBox box = window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    if (!window->m_isFloating) {
        const auto GAPS = ScrollOverview::Config::getCssGapData("general:gaps_in");
        box.x -= std::max<int64_t>(0, GAPS.m_left);
        box.y -= std::max<int64_t>(0, GAPS.m_top);
        box.width += std::max<int64_t>(0, GAPS.m_left) + std::max<int64_t>(0, GAPS.m_right);
        box.height += std::max<int64_t>(0, GAPS.m_top) + std::max<int64_t>(0, GAPS.m_bottom);
    }

    return getOverviewGlobalBox(box, monitor, scale, viewOffset, offset, layout, round);
}

static CBox getOverviewDragWindowBox(const PHLWINDOW& window, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout,
                                     bool round = true) {
    if (!window)
        return {};

    const auto TARGET = window->layoutTarget();
    if (window->m_group && TARGET)
        return getOverviewGlobalBox(TARGET->position(), monitor, scale, viewOffset, offset, layout, round);

    return getOverviewWindowBox(window, monitor, scale, viewOffset, offset, layout, round);
}

static CBox expandOverviewWindowHitbox(CBox box, float scale, float monitorScale) {
    const auto GAPS = ScrollOverview::Config::getCssGapData("general:gaps_in");
    constexpr float GAP_MULTIPLIER = 2.F;
    const float     TOTALSCALE     = scale * monitorScale;

    box.x -= sc<float>(std::max<int64_t>(0, GAPS.m_left)) * TOTALSCALE * GAP_MULTIPLIER;
    box.y -= sc<float>(std::max<int64_t>(0, GAPS.m_top)) * TOTALSCALE * GAP_MULTIPLIER;
    box.width += sc<float>(std::max<int64_t>(0, GAPS.m_left) + std::max<int64_t>(0, GAPS.m_right)) * TOTALSCALE * GAP_MULTIPLIER;
    box.height += sc<float>(std::max<int64_t>(0, GAPS.m_top) + std::max<int64_t>(0, GAPS.m_bottom)) * TOTALSCALE * GAP_MULTIPLIER;

    return box;
}

static float overviewPointDistanceSqToBox(const Vector2D& point, const CBox& box) {
    const float dx = point.x < box.x ? box.x - point.x : point.x > box.x + box.width ? point.x - (box.x + box.width) : 0.F;
    const float dy = point.y < box.y ? box.y - point.y : point.y > box.y + box.height ? point.y - (box.y + box.height) : 0.F;

    return dx * dx + dy * dy;
}

static Vector2D getOverviewMousePosLocal(PHLMONITOR monitor) {
    if (!monitor)
        return {};

    return (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;
}

static bool isOverviewPointerOnMonitor(PHLMONITOR monitor) {
    const auto overview = monitor ? scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()) : SP<IOverview>{};
    return overview && overview->pMonitor.lock() == monitor;
}

static Vector2D axisOffsetVector(float offset, ScrollOverview::Config::ELayout layout) {
    return layout == ScrollOverview::Config::ELayout::HORIZONTAL ? Vector2D{offset, 0.F} : Vector2D{0.F, offset};
}

static float axisValue(const Vector2D& vector, ScrollOverview::Config::ELayout layout) {
    return layout == ScrollOverview::Config::ELayout::HORIZONTAL ? vector.x : vector.y;
}

static float axisSize(const Vector2D& size, ScrollOverview::Config::ELayout layout) {
    return layout == ScrollOverview::Config::ELayout::HORIZONTAL ? size.x : size.y;
}

static CBox getOverviewWorkspaceBox(PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout) {
    return getOverviewBox({{}, monitor->m_size}, monitor, scale, viewOffset, offset, layout);
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

static CBox clampBoxToWorkspace(CBox box, PHLWORKSPACE workspace, PHLMONITOR fallbackMonitor, float margin = 0.F) {
    const auto WORKSPACEBOX = getWorkspaceGlobalBox(workspace, fallbackMonitor);
    if (WORKSPACEBOX.width <= 0 || WORKSPACEBOX.height <= 0)
        return box;

    const float CLAMPMARGIN = std::max(0.F, margin);
    const float MINX        = WORKSPACEBOX.x + CLAMPMARGIN;
    const float MINY        = WORKSPACEBOX.y + CLAMPMARGIN;
    const float MAXX        = WORKSPACEBOX.x + std::max(0.F, sc<float>(WORKSPACEBOX.width - box.width - 2.F * CLAMPMARGIN)) + CLAMPMARGIN;
    const float MAXY        = WORKSPACEBOX.y + std::max(0.F, sc<float>(WORKSPACEBOX.height - box.height - 2.F * CLAMPMARGIN)) + CLAMPMARGIN;

    box.x = std::clamp(sc<float>(box.x), MINX, std::max(MINX, MAXX));
    box.y = std::clamp(sc<float>(box.y), MINY, std::max(MINY, MAXY));

    return box;
}

static CBox resizedOverviewBoxFromCorner(const CBox& originalBox, const Vector2D& delta, Layout::eRectCorner corner, const Vector2D& minSizePx,
                                         const std::optional<Vector2D>& maxSizePx) {
    float left   = originalBox.x;
    float top    = originalBox.y;
    float right  = originalBox.x + originalBox.width;
    float bottom = originalBox.y + originalBox.height;

    switch (corner) {
        case Layout::CORNER_TOPLEFT:
            left += delta.x;
            top += delta.y;
            break;
        case Layout::CORNER_TOPRIGHT:
            right += delta.x;
            top += delta.y;
            break;
        case Layout::CORNER_BOTTOMLEFT:
            left += delta.x;
            bottom += delta.y;
            break;
        case Layout::CORNER_BOTTOMRIGHT:
        default:
            right += delta.x;
            bottom += delta.y;
            break;
    }

    float width  = right - left;
    float height = bottom - top;

    const float minWidth  = sc<float>(std::max(1.0, minSizePx.x));
    const float minHeight = sc<float>(std::max(1.0, minSizePx.y));
    const float maxWidth  = maxSizePx ? sc<float>(std::max(sc<double>(minWidth), maxSizePx->x)) : std::numeric_limits<float>::max();
    const float maxHeight = maxSizePx ? sc<float>(std::max(sc<double>(minHeight), maxSizePx->y)) : std::numeric_limits<float>::max();

    width  = std::clamp(width, minWidth, maxWidth);
    height = std::clamp(height, minHeight, maxHeight);

    switch (corner) {
        case Layout::CORNER_TOPLEFT:
            left = right - width;
            top  = bottom - height;
            break;
        case Layout::CORNER_TOPRIGHT:
            right = left + width;
            top   = bottom - height;
            break;
        case Layout::CORNER_BOTTOMLEFT:
            left   = right - width;
            bottom = top + height;
            break;
        case Layout::CORNER_BOTTOMRIGHT:
        default:
            right  = left + width;
            bottom = top + height;
            break;
    }

    return CBox{{left, top}, {right - left, bottom - top}};
}

static CBox clampResizedOverviewBoxToWorkspace(const CBox& box, const CBox& workspaceBox, Layout::eRectCorner corner, float marginPx) {
    if (workspaceBox.width <= 0 || workspaceBox.height <= 0)
        return box;

    const float minX = workspaceBox.x + std::max(0.F, marginPx);
    const float minY = workspaceBox.y + std::max(0.F, marginPx);
    const float maxX = workspaceBox.x + workspaceBox.width - std::max(0.F, marginPx);
    const float maxY = workspaceBox.y + workspaceBox.height - std::max(0.F, marginPx);

    float left   = box.x;
    float top    = box.y;
    float right  = box.x + box.width;
    float bottom = box.y + box.height;

    switch (corner) {
        case Layout::CORNER_TOPLEFT:
            left = std::max(left, minX);
            top  = std::max(top, minY);
            break;
        case Layout::CORNER_TOPRIGHT:
            right = std::min(right, maxX);
            top   = std::max(top, minY);
            break;
        case Layout::CORNER_BOTTOMLEFT:
            left   = std::max(left, minX);
            bottom = std::min(bottom, maxY);
            break;
        case Layout::CORNER_BOTTOMRIGHT:
        default:
            right  = std::min(right, maxX);
            bottom = std::min(bottom, maxY);
            break;
    }

    return CBox{{left, top}, {right - left, bottom - top}};
}

static bool overviewBoxIntersectsMonitor(const CBox& box, PHLMONITOR monitor) {
    if (!monitor || box.width <= 0 || box.height <= 0)
        return false;

    const auto RENDERSIZE = monitor->m_size * monitor->m_scale;

    return box.x < RENDERSIZE.x && box.x + box.width > 0 && box.y < RENDERSIZE.y && box.y + box.height > 0;
}

static bool overviewBoxFullyVisibleOnMonitor(const CBox& box, PHLMONITOR monitor) {
    if (!monitor || box.empty())
        return false;

    const CBox VIEWPORT = {{}, monitor->m_size * monitor->m_scale};
    return box.x >= VIEWPORT.x && box.y >= VIEWPORT.y && box.x + box.width <= VIEWPORT.x + VIEWPORT.width && box.y + box.height <= VIEWPORT.y + VIEWPORT.height;
}

static double overviewBoxIntersectionArea(const CBox& a, const CBox& b) {
    const auto INTERSECTION = a.intersection(b);
    return std::max(0.0, INTERSECTION.width) * std::max(0.0, INTERSECTION.height);
}

static double overviewBoxArea(const CBox& box) {
    return std::max(0.0, box.width) * std::max(0.0, box.height);
}

static double overviewBoxCenterDistanceSquared(const CBox& a, const CBox& b) {
    const auto ACENTER = a.middle();
    const auto BCENTER = b.middle();
    const auto DX      = ACENTER.x - BCENTER.x;
    const auto DY      = ACENTER.y - BCENTER.y;

    return DX * DX + DY * DY;
}

static CBox getPinnedFloatingOverviewWindowBox(PHLMONITOR monitor, const PHLWINDOW& window, float targetOverviewScale, float animationProgress, float* renderScale) {
    if (!monitor || !window) {
        if (renderScale)
            *renderScale = 1.F;
        return {};
    }

    const auto MONITORSCALE = monitor->m_scale;
    const auto WINDOWSIZE   = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT) * MONITORSCALE;
    if (WINDOWSIZE.x <= 0 || WINDOWSIZE.y <= 0) {
        if (renderScale)
            *renderScale = 1.F;
        return {};
    }

    const CBox WINDOWBOX = {(window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - monitor->m_position) * MONITORSCALE, WINDOWSIZE};
    const auto MONITORW  = sc<float>(monitor->m_size.x * MONITORSCALE);
    const auto MONITORH  = sc<float>(monitor->m_size.y * MONITORSCALE);

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

    const float RESERVEDLEFT   = std::max(0.F, sc<float>(WORKBOX.x - FULLBOX.x)) * MONITORSCALE;
    const float RESERVEDTOP    = std::max(0.F, sc<float>(WORKBOX.y - FULLBOX.y)) * MONITORSCALE;
    const float RESERVEDRIGHT  = std::max(0.F, sc<float>((FULLBOX.x + FULLBOX.width) - (WORKBOX.x + WORKBOX.width))) * MONITORSCALE;
    const float RESERVEDBOTTOM = std::max(0.F, sc<float>((FULLBOX.y + FULLBOX.height) - (WORKBOX.y + WORKBOX.height))) * MONITORSCALE;

    const auto WORKSPACEGAP       = sc<float>(ScrollOverview::Config::getWorkspaceGap()) * MONITORSCALE;
    const auto RESERVEDWIDTH      = RIGHT ? RESERVEDRIGHT : RESERVEDLEFT;
    const auto CALCULATEDWIDTH    = std::max(1.F, sc<float>((MONITORW - MONITORW * targetOverviewScale) / 2.F - 2.F * WORKSPACEGAP - RESERVEDWIDTH));
    const auto CALCULATEDSCALE    = CALCULATEDWIDTH / sc<float>(WINDOWSIZE.x);
    const auto WINDOWRENDERSCALE  = std::min(1.F, std::max(CALCULATEDSCALE, targetOverviewScale));
    const auto PROGRESS           = std::clamp(animationProgress, 0.F, 1.F);
    const auto CURRENTRENDERSCALE = 1.F + (WINDOWRENDERSCALE - 1.F) * PROGRESS;
    const auto TARGETWIDTH       = sc<float>(WINDOWSIZE.x) * CURRENTRENDERSCALE;
    const auto TARGETHEIGHT      = sc<float>(WINDOWSIZE.y) * CURRENTRENDERSCALE;

    if (renderScale)
        *renderScale = CURRENTRENDERSCALE;

    const float X = RIGHT ? MONITORW - TARGETWIDTH - WORKSPACEGAP - RESERVEDRIGHT : WORKSPACEGAP + RESERVEDLEFT;
    const float Y = BOTTOM ? MONITORH - TARGETHEIGHT - WORKSPACEGAP - RESERVEDBOTTOM : WORKSPACEGAP + RESERVEDTOP;

    CBox box = {{X, Y}, {TARGETWIDTH, TARGETHEIGHT}};

    box.x = WINDOWBOX.x + (box.x - WINDOWBOX.x) * PROGRESS;
    box.y = WINDOWBOX.y + (box.y - WINDOWBOX.y) * PROGRESS;
    box.round();

    return box;
}

static std::chrono::milliseconds getOverviewIdleFrameInterval() {
    const int fps = std::clamp<int>(ScrollOverview::Config::getValue<int>("misc:render_unfocused_fps"), 1, 240);
    return std::chrono::milliseconds(std::max(1, 1000 / fps));
}

static constexpr std::chrono::milliseconds OVERVIEW_WINDOW_FRAME_INTERVAL = std::chrono::milliseconds(33);

struct SOverviewShadowConfig {
    bool       enabled     = false;
    int        range       = 0;
    int        renderPower = 1;
    Config::CGradientValueData color;
};

static SOverviewShadowConfig getOverviewShadowConfig() {
    const auto enabled     = ScrollOverview::Config::getShadowEnabled();
    const auto range       = ScrollOverview::Config::getShadowRange();
    const auto renderPower = ScrollOverview::Config::getShadowRenderPower();
    const auto color       = ScrollOverview::Config::getShadowColor();

    const auto globalRange       = ScrollOverview::Config::getValue<int>("decoration:shadow:range");
    const auto globalRenderPower = ScrollOverview::Config::getValue<int>("decoration:shadow:render_power");
    const auto globalColor       = ScrollOverview::Config::getValue<::Config::CGradientValueData>("decoration:shadow:color");

    Config::CGradientValueData shadowColor;
    if (color && !color->m_colors.empty())
        shadowColor = *color;
    else if (!globalColor.m_colors.empty())
        shadowColor = globalColor;

    return {
        .enabled      = !!enabled,
        .range        = std::max(0, range >= 0 ? range : globalRange),
        .renderPower  = std::clamp(renderPower >= 0 ? renderPower : globalRenderPower, 1, 4),
        .color        = shadowColor,
    };
}

static void renderOverviewWorkspaceShadow(PHLMONITOR monitor, const CBox& workspaceBox, float overviewScale, bool cutoutCenter, float alpha = 1.F) {
    if (!monitor)
        return;

    const auto SHADOW = getOverviewShadowConfig();
    const bool HASVISIBLECOLOR = std::ranges::any_of(SHADOW.color.m_colors, [](const CHyprColor& color) { return color.a > 0.F; });
    if (!SHADOW.enabled || SHADOW.range <= 0 || !HASVISIBLECOLOR || alpha <= 0.F)
        return;

    const int RANGE = sc<int>(std::round(SHADOW.range * monitor->m_scale * overviewScale));
    if (RANGE <= 0)
        return;

    auto baseBox = workspaceBox.copy().round();
    if (baseBox.width < 1 || baseBox.height < 1)
        return;

    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewShadowPassElement>(COverviewShadowPassElement::SData{
        .monitor       = monitor,
        .fullBox       = baseBox.copy().expand(RANGE).round(),
        .cutoutBox     = baseBox,
        .rounding      = 0,
        .roundingPower = 2.F,
        .range         = RANGE,
        .renderPower   = SHADOW.renderPower,
        .color         = SHADOW.color,
        .alpha         = alpha,
        .ignoreWindow  = cutoutCenter,
    }));
}

static float getWorkspaceRenderedPitch(PHLMONITOR monitor, float scale, ScrollOverview::Config::ELayout layout) {
    return (axisSize(monitor->m_size, layout) * scale + sc<float>(ScrollOverview::Config::getWorkspaceGap())) * monitor->m_scale;
}

static float getWorkspaceLogicalPitch(PHLMONITOR monitor, float scale, ScrollOverview::Config::ELayout layout) {
    const auto safeScale = std::max(scale, 0.01F);
    return axisSize(monitor->m_size, layout) + sc<float>(ScrollOverview::Config::getWorkspaceGap()) / safeScale;
}

static float getWindowVerticalOverlap(const PHLWINDOW& a, const PHLWINDOW& b) {
    if (!a || !b)
        return 0.F;

    const auto APOS  = a->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto ASIZE = a->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto BPOS  = b->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto BSIZE = b->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    const double overlap = std::min(APOS.y + ASIZE.y, BPOS.y + BSIZE.y) - std::max(APOS.y, BPOS.y);

    return std::max(0.F, sc<float>(overlap));
}

static float getWindowHorizontalOverlap(const PHLWINDOW& a, const PHLWINDOW& b) {
    if (!a || !b)
        return 0.F;

    const auto APOS  = a->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto ASIZE = a->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto BPOS  = b->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto BSIZE = b->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    const double overlap = std::min(APOS.x + ASIZE.x, BPOS.x + BSIZE.x) - std::max(APOS.x, BPOS.x);

    return std::max(0.F, sc<float>(overlap));
}

static bool overviewBoxesEqual(const CBox& a, const CBox& b) {
    return std::abs(a.x - b.x) < 0.5 && std::abs(a.y - b.y) < 0.5 && std::abs(a.width - b.width) < 0.5 && std::abs(a.height - b.height) < 0.5;
}

static bool moveOverviewScrollingTargetToWorkspaceEdge(const SP<Layout::ITarget>& target, int side);
static bool moveOverviewScrollingTargetNextToWindow(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction);

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

    return dc<Layout::Tiled::CScrollingAlgorithm*>(target->space()->algorithm()->m_tiled.get());
}

static Layout::Tiled::CScrollingAlgorithm* overviewScrollingAlgorithmForWorkspace(const PHLWORKSPACE& workspace) {
    if (!workspace || !workspace->m_space || !workspace->m_space->algorithm())
        return nullptr;

    return dc<Layout::Tiled::CScrollingAlgorithm*>(workspace->m_space->algorithm()->m_tiled.get());
}

static bool isWorkspaceScrolling(const PHLWORKSPACE& workspace) {
    return overviewScrollingAlgorithmForWorkspace(workspace) != nullptr;
}

static Vector2D overviewScrollingCameraTranslation(Layout::Tiled::CScrollingAlgorithm* algorithm) {
    if (!algorithm || !algorithm->m_scrollingData || !algorithm->m_scrollingData->controller)
        return {};

    const auto& CONTROLLER = algorithm->m_scrollingData->controller;
    const auto TRANSLATION = CONTROLLER->isReversed() ? CONTROLLER->getOffset() : -CONTROLLER->getOffset();

    return CONTROLLER->isPrimaryHorizontal() ? Vector2D{TRANSLATION, 0.F} : Vector2D{0.F, TRANSLATION};
}

static CBox getOverviewWorkspaceUsableBox(const PHLWORKSPACE& workspace, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset,
                                          ScrollOverview::Config::ELayout layout) {
    if (!workspace || !monitor)
        return {};

    if (const auto ALGO = overviewScrollingAlgorithmForWorkspace(workspace); ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller) {
        const auto USABLE = ALGO->usableArea();
        return getOverviewBox(USABLE, monitor, scale, viewOffset, offset, layout);
    }

    if (!workspace->m_space)
        return getOverviewWorkspaceBox(monitor, scale, viewOffset, offset, layout);

    auto USABLE = workspace->m_space->workArea();
    USABLE.translate(-monitor->m_position);
    USABLE.w = std::max(USABLE.w, 1.0);
    USABLE.h = std::max(USABLE.h, 1.0);

    return getOverviewBox(USABLE, monitor, scale, viewOffset, offset, layout);
}

static double clampOverviewScrollingOffset(Layout::Tiled::CScrollingAlgorithm* algo, double offset) {
    if (!algo || !algo->m_scrollingData)
        return offset;

    const double MAXOFFSET = std::max(0.0, algo->m_scrollingData->maxWidth() - algo->primaryViewportSize());
    return std::clamp(offset, 0.0, MAXOFFSET);
}

static bool moveOverviewScrollingTargetToWorkspaceEdge(const SP<Layout::ITarget>& target, int side) {
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

    const auto SRC_COL_WIDTH = SRC_COL->getColumnWidth();
    SRC_COL->remove(target);

    const int64_t INSERT_AFTER = side < 0 ? -1 : sc<int64_t>(ALGO->m_scrollingData->columns.size()) - 1;
    const auto    NEW_COL      = ALGO->m_scrollingData->add(INSERT_AFTER, SRC_COL_WIDTH);
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

    const bool PRIMARYHORIZONTAL = ALGO->m_scrollingData->controller && ALGO->m_scrollingData->controller->isPrimaryHorizontal();
    const bool MOVINGCOLUMN      = PRIMARYHORIZONTAL ? direction == "l" || direction == "r" : direction == "u" || direction == "d";
    const bool STACKINGCOLUMN    = PRIMARYHORIZONTAL ? direction == "u" || direction == "d" : direction == "l" || direction == "r";

    if (MOVINGCOLUMN) {
        const auto SRC_COL_WIDTH = SRC_COL->getColumnWidth();
        SRC_COL->remove(target);

        const auto ANCHOR_COL_IDX = ALGO->m_scrollingData->idx(ANCHOR_COL);
        if (ANCHOR_COL_IDX < 0)
            return false;

        const bool    INSERT_BEFORE = direction == "l" || direction == "u";
        const int64_t INSERT_AFTER  = INSERT_BEFORE ? ANCHOR_COL_IDX - 1 : ANCHOR_COL_IDX;
        const auto    NEW_COL      = ALGO->m_scrollingData->add(INSERT_AFTER, SRC_COL_WIDTH);
        NEW_COL->add(TDATA);
        ALGO->m_scrollingData->centerOrFitCol(NEW_COL);
        ALGO->m_scrollingData->recalculate();
        ALGO->focusTargetUpdate(target);

        return true;
    }

    if (!STACKINGCOLUMN)
        return false;

    SRC_COL->remove(target);

    const auto ANCHOR_IDX    = ANCHOR_COL->idx(anchor->layoutTarget());
    const bool INSERT_BEFORE = direction == "l" || direction == "u";
    const int  INSERT_AFTER  = INSERT_BEFORE ? sc<int>(ANCHOR_IDX) - 1 : sc<int>(ANCHOR_IDX);
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

    const auto PREVFALLBACK = ScrollOverview::Config::getValue<int>("binds:window_direction_monitor_fallback");
    ScrollOverview::Config::setValue("binds:window_direction_monitor_fallback", 0);
    auto restoreFallback   = Hyprutils::Utils::CScopeGuard([PREVFALLBACK] {
        ScrollOverview::Config::setValue("binds:window_direction_monitor_fallback", PREVFALLBACK);
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

CScrollOverview::~CScrollOverview() {
    cancelWindowDrag();
    if (g_pointerGrabOverview == this)
        g_pointerGrabOverview = nullptr;
    transferSharedStateOwnership();
    restoreSubmapIfActive();
    if (const auto OPENGL = g_pHyprRenderer ? g_pHyprRenderer->glBackend() : WP<Render::GL::CHyprOpenGLImpl>{})
        OPENGL->makeEGLCurrent();
    if (realtimePreviewTimer) {
        wl_event_source_remove(realtimePreviewTimer);
        realtimePreviewTimer = nullptr;
    }
    if (backdropBlurFB)
        backdropBlurFB->release();
    backdropBlurFB.reset();
    const auto MONITOR = pMonitor.lock();
    const auto WORKSPACE = MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{};
    emitFullscreenVisibilityState(getOverviewFullscreenVisibilityWindow(WORKSPACE, Desktop::focusState()->window()), false);
    restoreWorkspaceAnimationOverrides();
    restoreInputConfigOverrides();
    restoreForcedSurfaceVisibility();
    restoreForcedWindowVisibility();
    restoreForcedLayerVisibility();
    images.clear(); // otherwise we get a vram leak
    if (scrollOverviews().empty()) {
        restoreActiveWorkspaceVisibility();
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
    }
    if (const auto MONITOR = pMonitor.lock())
        MONITOR->m_blurFBDirty = true;
}

CScrollOverview::CScrollOverview(PHLWORKSPACE startedOn_, bool swipe_, PHLMONITOR monitor_) : startedOn(startedOn_), swipe(swipe_) {
    const auto          PMONITOR = monitor_ ? monitor_ : (startedOn_ && startedOn_->m_monitor ? startedOn_->m_monitor.lock() : Desktop::focusState()->monitor());
    pMonitor                     = PMONITOR;
    layout                       = ScrollOverview::Config::getLayout();
    if (layout == ScrollOverview::Config::ELayout::AUTO)
        layout = PMONITOR && PMONITOR->logicalBox().height > PMONITOR->logicalBox().width ? ScrollOverview::Config::ELayout::HORIZONTAL : ScrollOverview::Config::ELayout::VERTICAL;
    sharedStateOwner             = scrollOverviews().empty();
    usesSubmapKeybinds           = hasOverviewSubmap();

    applyWorkspaceAnimationOverrides();
    if (sharedStateOwner)
        forceWorkspaceAlphaVisible();
    applyInputConfigOverrides();
    g_pInputManager->unconstrainMouse();
    realtimePreviewTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, realtimePreviewTimerCallback, this);
    scheduleMinimumPreviewFrame();

    const auto WINDOWSMOVECONFIG = Config::animationTree()->getAnimationPropertyConfig("windowsMove");
    const auto WINDOWSMOVEVALUES = WINDOWSMOVECONFIG && WINDOWSMOVECONFIG->pValues ? WINDOWSMOVECONFIG->pValues.lock() : WINDOWSMOVECONFIG;
    if (!Animation::mgr()->bezierExists(OVERVIEW_INSERT_FADE_BEZIER))
        Animation::mgr()->addBezierWithName(OVERVIEW_INSERT_FADE_BEZIER, Vector2D{0.5, 0.0}, Vector2D{0.5, 0.0});
    if (!Animation::mgr()->bezierExists(OVERVIEW_REMOVE_FADE_BEZIER))
        Animation::mgr()->addBezierWithName(OVERVIEW_REMOVE_FADE_BEZIER, Vector2D{0.5, 1.0}, Vector2D{0.5, 1.0});

    workspaceInsertFadeConfig                  = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    workspaceInsertFadeConfig->overridden      = true;
    workspaceInsertFadeConfig->internalBezier  = OVERVIEW_INSERT_FADE_BEZIER;
    workspaceInsertFadeConfig->internalSpeed   = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalSpeed * 1.2F : 12.F;
    workspaceInsertFadeConfig->internalEnabled = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalEnabled : 1;
    workspaceInsertFadeConfig->internalStyle   = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalStyle : "";
    workspaceInsertFadeConfig->pValues         = workspaceInsertFadeConfig;

    workspaceRemoveFadeConfig                  = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    workspaceRemoveFadeConfig->overridden      = true;
    workspaceRemoveFadeConfig->internalBezier  = OVERVIEW_REMOVE_FADE_BEZIER;
    workspaceRemoveFadeConfig->internalSpeed   = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalSpeed : 10.F;
    workspaceRemoveFadeConfig->internalEnabled = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalEnabled : 1;
    workspaceRemoveFadeConfig->internalStyle   = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalStyle : "";
    workspaceRemoveFadeConfig->pValues         = workspaceRemoveFadeConfig;

    Animation::mgr()->createAnimation(1.F, scale, WINDOWSMOVECONFIG, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation({}, viewOffset, WINDOWSMOVECONFIG, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(1.F, workspaceInsertProgress, WINDOWSMOVECONFIG, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(1.F, workspaceInsertFadeProgress, workspaceInsertFadeConfig, AVARDAMAGE_NONE);

    scale->setUpdateCallback([this](auto) { damage(); });
    viewOffset->setUpdateCallback([this](auto) { damage(); });
    workspaceInsertProgress->setUpdateCallback([this](auto) { damage(); });
    workspaceInsertFadeProgress->setUpdateCallback([this](auto) { damage(); });

    if (!swipe)
        *scale = ScrollOverview::Config::getScale();

    const auto initialFullscreenWindow =
        PMONITOR && PMONITOR->m_activeWorkspace ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(PMONITOR->m_activeWorkspace)) : PHLWINDOW{};
    emitFullscreenVisibilityState(initialFullscreenWindow ? initialFullscreenWindow : Desktop::focusState()->window(), true);

    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

    auto onMouseMove = [this](Vector2D, Event::SCallbackInfo& info) {
        const auto INPUTOVERVIEW = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());

        if (closing || (g_pointerGrabOverview && g_pointerGrabOverview != this) || (!g_pointerGrabOverview && INPUTOVERVIEW.get() != this))
            return;

        if (dragCancelledAwaitingRelease) {
            info.cancelled = true;
            return;
        }

        const bool     LEFT_HANDED           = ScrollOverview::Config::getLeftHanded();
        const uint32_t MAIN_BUTTON           = LEFT_HANDED ? BTN_RIGHT : BTN_LEFT;
        const bool     INVERT_DRAG_MODE      = ScrollOverview::Config::getDragMode() == 1;
        const uint32_t SECONDARY_DRAG_BUTTON = BTN_MIDDLE;
        const float    DRAGTHRESHOLD         = ScrollOverview::Config::getDragThreshold() * (pMonitor ? pMonitor->m_scale : 1.F);
        const float    DRAGTHRESHOLDSQ       = std::pow(DRAGTHRESHOLD, 2);

        lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

        const auto beginScrollingPanAtPoint = [&](const Vector2D& point) {
            auto WORKSPACE = workspaceAtOverviewPoint(point);
            if (!WORKSPACE) {
                const auto WINDOW = windowAtOverviewPoint(point);
                if (WINDOW)
                    WORKSPACE = WINDOW->m_workspace;
            }

            if (!isWorkspaceScrolling(WORKSPACE))
                return false;

            beginScrollingPan(WORKSPACE);
            scrollingPanLastMouseLocal = point;
            updateScrollingPan();
            return true;
        };

        if (!dragPendingPrimary && !resizePointerDown && !scrollingPanPointerDown && !dragActiveWindow && !resizeActiveWindow && isPointerOnTopLayer(pMonitor.lock())) {
            submapMouseClickPending = false;
            submapMouseClickButton  = 0;
            return;
        }

        info.cancelled = true;
        requestInputFrame();

        if (submapMouseClickPending) {
            const bool DRAGTHRESHOLDREACHED = dragStartMouseLocal.distanceSq(lastMousePosLocal) > DRAGTHRESHOLDSQ;

            if (submapMouseClickButton == MAIN_BUTTON && !dragActiveWindow && !scrollingPanPointerDown && DRAGTHRESHOLDREACHED) {
                submapMouseClickPending = false;
                submapMouseClickButton  = 0;

                if (!INVERT_DRAG_MODE)
                    beginWindowDrag(windowAtOverviewPoint(dragStartMouseLocal));
                else
                    beginScrollingPanAtPoint(dragStartMouseLocal);
            }

            if (submapMouseClickButton == SECONDARY_DRAG_BUTTON && !INVERT_DRAG_MODE && !scrollingPanPointerDown && DRAGTHRESHOLDREACHED) {
                if (beginScrollingPanAtPoint(dragStartMouseLocal)) {
                    submapMouseClickPending = false;
                    submapMouseClickButton  = 0;
                }
            }

            if (submapMouseClickButton == SECONDARY_DRAG_BUTTON && INVERT_DRAG_MODE && !dragActiveWindow && DRAGTHRESHOLDREACHED) {
                submapMouseClickPending = false;
                submapMouseClickButton  = 0;
                beginWindowDrag(windowAtOverviewPoint(dragStartMouseLocal));
            }
        }

        if (dragPendingPrimary) {
            if (!dragActiveWindow && !scrollingPanPointerDown && dragStartMouseLocal.distanceSq(lastMousePosLocal) > DRAGTHRESHOLDSQ) {
                if (!INVERT_DRAG_MODE) {
                    beginWindowDrag(windowAtOverviewPoint(dragStartMouseLocal));
                } else
                    beginScrollingPanAtPoint(dragStartMouseLocal);
            }
        }

        if (dragActiveWindow)
            updateWindowDrag();

        if (resizePointerDown && resizePendingWindow) {
            if (!resizeActiveWindow && resizeStartMouseLocal.distanceSq(lastMousePosLocal) > DRAGTHRESHOLDSQ)
                beginWindowResize();

            if (resizeActiveWindow)
                updateWindowResize();
        }

        if (scrollingPanPointerDown)
            updateScrollingPan();

        //  highlightHoverDebug();
    };

    auto onTouchMove = [this](ITouch::SMotionEvent, Event::SCallbackInfo& info) {
        if (closing || scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()).get() != this)
            return;

        info.cancelled    = true;
        lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());
        requestInputFrame();
    };

    auto onMouseButton = [this](IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
        if (info.cancelled)
            return;

        const bool FORWARDEDTOPLAYERRELEASE =
            event.state == WL_POINTER_BUTTON_STATE_RELEASED && g_topLayerPointerButtons.contains(event.button);
        const bool ADOPTEDNATIVEDRAGRELEASE = dragAdoptedFromNative && g_pointerGrabOverview == this && event.state == WL_POINTER_BUTTON_STATE_RELEASED;
        const bool CANCELLEDGRABRELEASE = closing && dragCancelledAwaitingRelease && g_pointerGrabOverview == this && event.state == WL_POINTER_BUTTON_STATE_RELEASED;
        const auto INPUTOVERVIEW = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
        if ((closing && !CANCELLEDGRABRELEASE) || (g_pointerGrabOverview && g_pointerGrabOverview != this) ||
            (!g_pointerGrabOverview && INPUTOVERVIEW.get() != this && !FORWARDEDTOPLAYERRELEASE))
            return;

        if (dragCancelledAwaitingRelease && event.state != WL_POINTER_BUTTON_STATE_RELEASED) {
            info.cancelled = true;
            return;
        }

        const bool RELEASESPOINTERGRAB = event.state == WL_POINTER_BUTTON_STATE_RELEASED;
        auto       releasePointerGrab  = Hyprutils::Utils::CScopeGuard([this, RELEASESPOINTERGRAB] {
            if (RELEASESPOINTERGRAB && g_pointerGrabOverview == this) {
                if (dragCancelledAwaitingRelease && closing && closeRemovalPending && !scale->isBeingAnimated())
                    wl_event_loop_add_idle(
                        g_pCompositor->m_wlEventLoop,
                        [](void* data) {
                            auto* const overview = sc<CScrollOverview*>(data);
                            if (overview->closing && overview->closeRemovalPending && !overview->dragCancelledAwaitingRelease)
                                removeOverview(overview);
                        },
                        this);

                dragCancelledAwaitingRelease = false;
                dragAdoptedFromNative        = false;
                g_pointerGrabOverview        = nullptr;
            }
        });

        if (ADOPTEDNATIVEDRAGRELEASE) {
            if (dragActiveWindow)
                endWindowDrag();
            return;
        }

        const bool POINTERONTOPLAYER =
            !dragPendingPrimary && !resizePointerDown && !scrollingPanPointerDown && !dragActiveWindow && !resizeActiveWindow && isPointerOnTopLayer(pMonitor.lock());

        if (dragCancelledAwaitingRelease) {
            info.cancelled = true;
            return;
        }

        if (FORWARDEDTOPLAYERRELEASE ||
            (event.state == WL_POINTER_BUTTON_STATE_PRESSED && POINTERONTOPLAYER && usesSubmapKeybinds && isOverviewSubmapActive())) {
            submapMouseClickPending = false;
            submapMouseClickButton  = 0;

            info.cancelled = true;
            if (event.state == WL_POINTER_BUTTON_STATE_PRESSED)
                g_topLayerPointerButtons.emplace(event.button);
            else
                g_topLayerPointerButtons.erase(event.button);

            g_pSeatManager->sendPointerButton(event.timeMs, event.button, event.state);
            g_pSeatManager->sendPointerFrame();
            return;
        }

        if (POINTERONTOPLAYER) {
            submapMouseClickPending = false;
            submapMouseClickButton  = 0;
            return;
        }

        if (event.state == WL_POINTER_BUTTON_STATE_PRESSED)
            g_pointerGrabOverview = this;

        info.cancelled = true;
        Config::Actions::state()->m_lastMouseCode = event.button;
        Config::Actions::state()->m_lastCode      = 0;
        Config::Actions::state()->m_timeLastMs    = event.timeMs;
        // Without releasing buttons, mouse-triggered overview consumes release
        // events
        // before they reach Hyprland's input manager, leaving it stuck thinking
        // buttons are still pressed, which locks focus.
        releaseTopLayerPointerButtons(event.timeMs);
        g_pInputManager->releaseAllMouseButtons();

        const bool     LEFT_HANDED        = ScrollOverview::Config::getLeftHanded();
        const uint32_t MAIN_BUTTON        = LEFT_HANDED ? BTN_RIGHT : BTN_LEFT;
        const uint32_t RESIZE_BUTTON      = LEFT_HANDED ? BTN_LEFT : BTN_RIGHT;
        const bool     INVERT_DRAG_MODE   = ScrollOverview::Config::getDragMode() == 1;
        const uint32_t SECONDARY_DRAG_BUTTON = BTN_MIDDLE;
        const auto     clearSubmapMouseClickPending = [&]() {
            submapMouseClickPending = false;
            submapMouseClickButton  = 0;
        };
        const auto     beginSubmapMouseClickPending = [&](uint32_t button) {
            if (!usesSubmapKeybinds || !isOverviewSubmapActive())
                return false;

            submapMouseClickPending = true;
            submapMouseClickButton  = button;
            dragStartMouseLocal     = lastMousePosLocal;
            return true;
        };
        const auto     shouldRunDefaultClickAction = [&](uint32_t button) {
            if (submapMouseClickPending && submapMouseClickButton == button) {
                clearSubmapMouseClickPending();
                dispatchSubmapMouseClick(button);
                return false;
            }

            return !usesSubmapKeybinds || !isOverviewSubmapActive();
        };
        const auto     performClickAction = [&](uint32_t button) {
            if (!shouldRunDefaultClickAction(button))
                return;

            selectHoveredWorkspace();
            selectWindowAtOverviewCursor(true);
            closeAll();
        };
        const auto     finishWindowDragOrClick = [&](uint32_t button, bool allowClick) {
            const float CLICK_MAX_DRAG_DISTANCE = 10.F * (pMonitor ? pMonitor->m_scale : 1.F);

            if (dragActiveWindow) {
                if (dragStartMouseLocal.distanceSq(lastMousePosLocal) < CLICK_MAX_DRAG_DISTANCE * CLICK_MAX_DRAG_DISTANCE) {
                    finishWindowDragSession();

                    if (allowClick)
                        performClickAction(button);
                    else
                        clearSubmapMouseClickPending();

                    return;
                }

                endWindowDrag();
                clearSubmapMouseClickPending();
                return;
            }

            clearDragPending();

            if (allowClick)
                performClickAction(button);
            else
                clearSubmapMouseClickPending();
        };
        const auto     workspaceAtPanPoint = [&](const Vector2D& point) {
            auto WORKSPACE = workspaceAtOverviewPoint(point);
            if (WORKSPACE)
                return WORKSPACE;

            const auto WINDOW = windowAtOverviewPoint(point);
            return WINDOW ? WINDOW->m_workspace : PHLWORKSPACE{};
        };

        if (event.button == MAIN_BUTTON) {
            lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

            if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                if (beginSubmapMouseClickPending(event.button))
                    return;

                dragPendingPrimary  = true;
                dragStartMouseLocal = lastMousePosLocal;
                return;
            }

            const bool WASPANNING = scrollingPanPointerDown;
            if (scrollingPanPointerDown)
                endScrollingPan();

            finishWindowDragOrClick(event.button, !WASPANNING);
            return;
        }

        if (event.button == SECONDARY_DRAG_BUTTON && !INVERT_DRAG_MODE) {
            lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

            if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                if (beginSubmapMouseClickPending(event.button))
                    return;

                const auto WORKSPACE = workspaceAtPanPoint(lastMousePosLocal);

                if (!isWorkspaceScrolling(WORKSPACE)) {
                    scrollingPanPointerDown = false;
                    return;
                }

                beginScrollingPan(WORKSPACE);
                return;
            }

            if (submapMouseClickPending && submapMouseClickButton == event.button) {
                const bool WASPANNING = scrollingPanPointerDown;
                if (scrollingPanPointerDown)
                    endScrollingPan();

                if (!WASPANNING)
                    shouldRunDefaultClickAction(event.button);
                else
                    clearSubmapMouseClickPending();

                return;
            }

            if (scrollingPanPointerDown)
                endScrollingPan();
            return;
        }

        if (event.button == RESIZE_BUTTON) {
            lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

            if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                beginSubmapMouseClickPending(event.button);

                size_t resizeWorkspace = 0;
                const auto window      = windowAtOverviewCursor(&resizeWorkspace);
                if (!shouldShowOverviewWindow(window) || shouldShowPinnedFloatingOverviewWindow(window)) {
                    resizePointerDown = false;
                    resizePendingWindow.reset();
                    return;
                }

                const auto MONITOR = pMonitor.lock();
                if (!MONITOR)
                    return;

                const auto WORKSPACEOFFSET =
                    workspaceOverviewOffset(resizeWorkspace, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
                const auto WINDOWBOX = getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);

                resizePointerDown    = true;
                resizeStartMouseLocal = lastMousePosLocal;
                resizePendingWindow   = window;
                resizeWorkspaceIdx    = resizeWorkspace;
                resizeCorner          = Layout::cornerFromBox(WINDOWBOX, lastMousePosLocal);
                return;
            }

            if (resizeActiveWindow) {
                endWindowResize();
                clearSubmapMouseClickPending();
                return;
            }

            resizePointerDown = false;
            resizePendingWindow.reset();

            shouldRunDefaultClickAction(event.button);
            return;
        }

        if (event.button != SECONDARY_DRAG_BUTTON || !INVERT_DRAG_MODE)
            return;

        lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

        if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
            if (beginSubmapMouseClickPending(event.button))
                return;

            dragStartMouseLocal = lastMousePosLocal;
            beginWindowDrag(windowAtOverviewCursor());
            return;
        }

        finishWindowDragOrClick(event.button, true);
    };

    auto onCursorSelect = [this](auto, Event::SCallbackInfo& info) {
        if (closing || scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()).get() != this)
            return;

        if (isPointerOnTopLayer(pMonitor.lock()))
            return;

        info.cancelled = true;

        selectWindowAtOverviewCursor(true);

        closeAll();
    };

    auto onMouseAxis = [this](IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
        if (info.cancelled || closing || scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()).get() != this)
            return;

        if (usesSubmapKeybinds && isOverviewSubmapActive() && hasApplicableScrollKeybind(e)) {
            if (scrollKeybindIsThrottled())
                info.cancelled = true;
            return;
        }

        info.cancelled = true;

        const auto ACTION = e.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? ScrollOverview::Config::getHorizontalScrollAction(layout) :
                                                                          ScrollOverview::Config::getVerticalScrollAction(layout);

        // mouse wheel: discrete stepping, throttled by scroll_event_delay so one notch is one step
        if (e.source == WL_POINTER_AXIS_SOURCE_WHEEL) {
            if (e.delta == 0.0)
                return;
            trackpadScrollAccum        = 0.0;
            trackpadWorkspaceFollowing = false;
            trackpadTapeFollowing      = false;
            if (!scrollStepAllowed(e.timeMs))
                return;

            if (ACTION == ScrollOverview::Config::EScrollAction::WORKSPACE)
                moveViewportWorkspace(e.delta > 0);
            else
                moveScrollingColumnSelection(e.delta > 0);

            return;
        }

        if (images.empty() || viewportCurrentWorkspace >= images.size())
            return;

        // scroll workspace or layout with 1:1 animation (snaping on release)
        if (ACTION == ScrollOverview::Config::EScrollAction::WORKSPACE)
            trackpadSwipeWorkspace(e.delta);
        else
            trackpadSwipeLayout(images[viewportCurrentWorkspace]->pWorkspace, e.delta);
    };

    auto onWindowOpen = [this](PHLWINDOW) {
        if (closing)
            return;

        rebuildPending = true;
        damage();
    };

    auto onWindowClose = [this](PHLWINDOW window) {
        if (closing)
            return;

        if (dragActiveWindow && getOverviewWindowToShow(window) == getOverviewWindowToShow(dragActiveWindow.lock()))
            cancelWindowDrag();

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

        g_pInputManager->unconstrainMouse();

        const auto overviewWindow = getOverviewWindowToShow(window);
        const auto fullscreenWindow = overviewWindow && overviewWindow->m_workspace ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(overviewWindow->m_workspace)) : PHLWINDOW{};

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
        if (!window || window->m_monitor != pMonitor || !Fullscreen::controller()->isFullscreen(window))
            return;

        emitFullscreenVisibilityState(window, true);
    };

    auto onWorkspaceLifecycle = [this](auto) {
        if (closing)
            return;

        rebuildPending = true;
        damage();
    };

    auto onKeyboardKey = [this](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
        if (closing || event.state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;

        const auto KEYSYM = getOverviewKeysym(event);
        if (activeScrollOverview().get() != this || isTopLayerFocused(pMonitor.lock()))
            return;
        const auto MODS   = g_pInputManager->getModsFromAllKBs() & ~(HL_MODIFIER_CAPS | HL_MODIFIER_MOD2);

        if ((KEYSYM == XKB_KEY_Return || KEYSYM == XKB_KEY_KP_Enter || KEYSYM == XKB_KEY_Left || KEYSYM == XKB_KEY_KP_Left || KEYSYM == XKB_KEY_Right ||
             KEYSYM == XKB_KEY_KP_Right || KEYSYM == XKB_KEY_Up || KEYSYM == XKB_KEY_KP_Up || KEYSYM == XKB_KEY_Down || KEYSYM == XKB_KEY_KP_Down) &&
            MODS != 0)
            return;

        switch (KEYSYM) {
            case XKB_KEY_Left:
            case XKB_KEY_KP_Left:
                moveSelection("left");
                break;
            case XKB_KEY_Right:
            case XKB_KEY_KP_Right:
                moveSelection("right");
                break;
            case XKB_KEY_Up:
            case XKB_KEY_KP_Up:
                moveSelection("up");
                break;
            case XKB_KEY_Down:
            case XKB_KEY_KP_Down:
                moveSelection("down");
                break;
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter: closeAll(); break;
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
    activateSubmapIfConfigured();
    if (!usesSubmapKeybinds)
        keyboardKeyHook = Event::bus()->m_events.input.keyboard.key.listen(onKeyboardKey);

    dragKeyboardKeyHook = Event::bus()->m_events.input.keyboard.key.listen([this](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
        if (closing || !dragActiveWindow || event.state != WL_KEYBOARD_KEY_STATE_PRESSED || getOverviewKeysym(event) != XKB_KEY_Escape)
            return;

        info.cancelled = true;
        cancelWindowDrag();
    });

    Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);

    redrawAll();

    rememberSelection(Desktop::focusState()->window());
    viewportCurrentWorkspace = activeWorkspaceIndex();
    syncSelectionToViewport();
}

static void renderOverviewLayerLevel(PHLMONITOR monitor, uint32_t layer, const CBox& workspaceBox, float renderScale, const Time::steady_tp& now, float alpha = 1.F) {
    if (!monitor)
        return;

    bool pushedRenderHints = false;
    const bool MODULATEALPHA = alpha < 0.999F;

    for (auto const& ls : monitor->m_layerSurfaceLayers[layer]) {
        const auto LAYER = ls.lock();
        if (!Desktop::View::validMapped(LAYER))
            continue;

        if (!pushedRenderHints) {
            Render::SRenderModifData modif;
            modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, renderScale);
            modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, workspaceBox.pos());

            g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
            pushedRenderHints = true;
        }

		auto& lsAlpha = LAYER->alpha()[Desktop::View::LS_ALPHA_FADE];
        float previousAlpha = 1.F;
        if (MODULATEALPHA && lsAlpha->value()) {
			previousAlpha = lsAlpha->value();
			lsAlpha->setValueAndWarp(previousAlpha * std::clamp(alpha, 0.F, 1.F));
		}

        g_pHyprRenderer->renderLayer(LAYER, monitor, now);

        if (MODULATEALPHA && lsAlpha->value())
			lsAlpha->setValueAndWarp(previousAlpha);
    }

    if (pushedRenderHints)
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));
}

void CScrollOverview::renderWallpaperLayers(PHLMONITOR monitor, const CBox& workspaceBox, float renderScale, const Time::steady_tp& now, float alpha) {
    if (!monitor)
        return;

    renderOverviewLayerLevel(monitor, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, workspaceBox, renderScale, now, alpha);
}

void CScrollOverview::renderGlobalWallpaper(PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!monitor)
        return;

    g_pHyprRenderer->renderBackground(monitor);

    for (auto const& ls : monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]) {
        if (!Desktop::View::validMapped(ls.lock()))
            continue;

        g_pHyprRenderer->renderLayer(ls.lock(), monitor, now);
    }
}

void CScrollOverview::updateBackdropBlurCache(PHLMONITOR monitor, int wallpaperMode, const Time::steady_tp& now) {
    if (!monitor || wallpaperMode == 1 || !ScrollOverview::Config::getBlur())
        return;

    if (lastBackdropWallpaperMode != wallpaperMode) {
        backdropBlurDirty         = true;
        lastBackdropWallpaperMode = wallpaperMode;
    }

    const auto FBSIZE     = monitor->m_pixelSize;
    const auto RENDERSIZE = monitor->m_transformedSize;
    const auto FBFORMAT   = g_pHyprRenderer->m_renderData.currentFB->m_drmFormat;
    if (!backdropBlurFB)
        backdropBlurFB = g_pHyprRenderer->createFB("scrolloverview_backdrop_blur");

    if (!backdropBlurFB || !backdropBlurFB->isAllocated() || backdropBlurFB->m_size != FBSIZE || backdropBlurFB->m_drmFormat != FBFORMAT) {
        if (backdropBlurFB)
            backdropBlurFB->release();
        if (!backdropBlurFB || !backdropBlurFB->alloc(sc<int>(FBSIZE.x), sc<int>(FBSIZE.y), FBFORMAT))
            return;
        backdropBlurDirty = true;
    }

    if (!backdropBlurDirty)
        return;

    if (g_pHyprRenderer->m_renderData.currentFB)
        backdropBlurFB->setImageDescription(g_pHyprRenderer->m_renderData.currentFB->imageDescription());

    const CRegion fullDamage{CBox{0, 0, RENDERSIZE.x, RENDERSIZE.y}};

    {
        auto bindBackdrop = g_pHyprRenderer->bindTempFB(backdropBlurFB);
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 1.F}}, fullDamage);
        renderGlobalWallpaper(monitor, now);
        OverviewRender::flushPass(monitor);
    }

    auto blurDamage = fullDamage;
    const auto BLURREDTEX = g_pHyprRenderer->blurFramebuffer(backdropBlurFB, 1.F, &blurDamage);
    if (!BLURREDTEX || !BLURREDTEX->m_size.x || !BLURREDTEX->m_size.y)
        return;

    {
        auto bindBackdrop = g_pHyprRenderer->bindTempFB(backdropBlurFB);
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 0.F}}, fullDamage);

        const auto SAVEDTRANSFORM = BLURREDTEX->m_transform;
        BLURREDTEX->m_transform   = Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform));
        auto restoreTransform     = Hyprutils::Utils::CScopeGuard([BLURREDTEX, SAVEDTRANSFORM] { BLURREDTEX->m_transform = SAVEDTRANSFORM; });

        g_pHyprRenderer->pushMonitorTransformEnabled(true);
        auto restoreMonitorTransform = Hyprutils::Utils::CScopeGuard([] { g_pHyprRenderer->popMonitorTransformEnabled(); });

        g_pHyprRenderer->draw(
            CTexPassElement::SRenderData{
                .tex    = BLURREDTEX,
                .box    = CBox{0, 0, RENDERSIZE.x, RENDERSIZE.y},
                .damage = fullDamage,
            },
            fullDamage);
    }

    backdropBlurDirty = false;
}

void CScrollOverview::renderBackdropBlurCache(PHLMONITOR monitor) {
    if (!monitor || !backdropBlurFB || !backdropBlurFB->isAllocated() || !backdropBlurFB->getTexture())
        return;

    const auto TEX = backdropBlurFB->getTexture();
    const CRegion fullDamage{CBox{0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y}};

    g_pHyprRenderer->draw(
        CTexPassElement::SRenderData{
            .tex      = TEX,
            .box      = CBox{0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y},
            .a        = 1.F,
            .damage   = fullDamage,
            .flipEndFrame = true,
        },
        fullDamage);
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

bool CScrollOverview::isSelectedWorkspace(const PHLWORKSPACE& workspace) const {
    return workspace && viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] &&
        images[viewportCurrentWorkspace]->pWorkspace == workspace;
}

float CScrollOverview::workspaceOverviewOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const {
    const auto MONITOR             = pMonitor.lock();
    const auto MONITORSCALE        = MONITOR ? std::max(MONITOR->m_scale, 0.01F) : 1.F;
    const auto MONITORSIZE         = MONITOR ? axisSize(MONITOR->m_size, layout) : 0.F;
    const auto RENDERSCALE         = MONITOR && MONITORSIZE > 0 ?
        std::max(0.01F, (workspacePitch / MONITORSCALE - sc<float>(ScrollOverview::Config::getWorkspaceGap())) / sc<float>(MONITORSIZE)) :
        std::max(scale->value(), 0.01F);
    const auto LOGICALPITCH        = MONITOR ? getWorkspaceLogicalPitch(MONITOR, RENDERSCALE, layout) : workspacePitch / std::max(RENDERSCALE * MONITORSCALE, 0.01F);
    const auto RENDEREDLOGICALUNIT = RENDERSCALE * MONITORSCALE;
    const auto DEFAULTOFFSET       = workspaceOverviewLogicalOffset(workspaceIdx, activeIdx, LOGICALPITCH) * RENDEREDLOGICALUNIT;

    if (!workspaceInsertTransition.active || workspaceIdx >= images.size() || !images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
        return DEFAULTOFFSET;

    const auto WORKSPACEID = images[workspaceIdx]->pWorkspace->m_id;
    const auto NEWIT       = workspaceInsertTransition.newRelativeOffsets.find(WORKSPACEID);
    if (NEWIT == workspaceInsertTransition.newRelativeOffsets.end())
        return DEFAULTOFFSET;

    const float T         = std::clamp(workspaceInsertProgress->value(), 0.F, 1.F);
    const float NEWOFFSET = NEWIT->second * RENDEREDLOGICALUNIT;

    if (const auto OLDIT = workspaceInsertTransition.oldRelativeOffsets.find(WORKSPACEID); OLDIT != workspaceInsertTransition.oldRelativeOffsets.end()) {
        const float OLDOFFSET = OLDIT->second * RENDEREDLOGICALUNIT;
        return OLDOFFSET + (NEWOFFSET - OLDOFFSET) * T;
    }

    return NEWOFFSET;
}

float CScrollOverview::workspaceOverviewLogicalOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const {
    const auto EXTRAINTERVAL = [this](size_t workspaceIdx_) -> float {
        if (workspaceIdx_ + 1 >= images.size() || !images[workspaceIdx_] || !images[workspaceIdx_ + 1])
            return 0.F;

        if (layout == ScrollOverview::Config::ELayout::HORIZONTAL)
            return images[workspaceIdx_]->overflowRight + images[workspaceIdx_ + 1]->overflowLeft;

        return images[workspaceIdx_]->overflowBottom + images[workspaceIdx_ + 1]->overflowTop;
    };

    float offset = 0.F;

    if (workspaceIdx > activeIdx) {
        for (size_t i = activeIdx; i < workspaceIdx; ++i)
            offset += workspacePitch + EXTRAINTERVAL(i);
    } else {
        for (size_t i = workspaceIdx; i < activeIdx; ++i)
            offset -= workspacePitch + EXTRAINTERVAL(i);
    }

    return offset;
}

float CScrollOverview::workspaceOverviewAlpha(size_t workspaceIdx) const {
    if (!workspaceInsertTransition.active || workspaceIdx >= images.size() || !images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
        return 1.F;

    if (images[workspaceIdx]->pWorkspace->m_id != workspaceInsertTransition.transitionWorkspaceID)
        return 1.F;

    if (!workspaceInsertTransition.transitionFadeIn)
        return 1.F;

    if (workspaceInsertTransition.oldRelativeOffsets.contains(workspaceInsertTransition.transitionWorkspaceID))
        return 1.F;

    return std::clamp(workspaceInsertFadeProgress->value(), 0.F, 1.F);
}

void CScrollOverview::rebuildWorkspaceImages() {
    const auto selectedWorkspace = closeOnWindow ? closeOnWindow->m_workspace : startedOn;
    const auto selectedWindow    = closeOnWindow;
    const auto viewportWorkspace = viewportCurrentWorkspace < images.size() ? images[viewportCurrentWorkspace]->pWorkspace : startedOn;
    const auto REMOVEDWORKSPACE  = pendingRemovedWorkspace.lock();

    images.clear();

    for (const auto& w : State::workspaceState()->workspaces()) {
        const auto WORKSPACE = w.lock();
        if (!valid(WORKSPACE) || WORKSPACE->m_monitor != pMonitor || WORKSPACE->m_isSpecialWorkspace)
            continue;

        if (WORKSPACE == REMOVEDWORKSPACE)
            continue;

        images.emplace_back(makeShared<SWorkspaceImage>(WORKSPACE));
    }

    std::sort(images.begin(), images.end(), [](const auto& a, const auto& b) { return a->pWorkspace->m_id < b->pWorkspace->m_id; });

    if (images.empty()) {
        viewportCurrentWorkspace = 0;
        closeOnWindow.reset();
        return;
    }

    viewportCurrentWorkspace = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == viewportWorkspace) {
            viewportCurrentWorkspace = i;
            break;
        }
    }
    if (images[viewportCurrentWorkspace]->pWorkspace != viewportWorkspace) {
        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i]->pWorkspace == selectedWorkspace) {
                viewportCurrentWorkspace = i;
                break;
            }
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

void CScrollOverview::updateWorkspaceOverflow() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    for (const auto& img : images) {
        if (!img)
            continue;

        img->overflowLeft   = 0.F;
        img->overflowRight  = 0.F;
        img->overflowTop    = 0.F;
        img->overflowBottom = 0.F;
    }

    for (const auto& img : images) {
        if (!img || !img->pWorkspace)
            continue;

        for (const auto& windowRef : img->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!shouldShowOverviewWindow(window) || window->m_isFloating)
                continue;

            const auto POS  = window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - MONITOR->m_position;
            const auto SIZE = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            // Windows hidden under a fullscreen window can get sentinel geometry like -2x-2.
            if (SIZE.x <= 0 || SIZE.y <= 0)
                continue;

            img->overflowLeft   = std::max(img->overflowLeft, std::max(0.F, sc<float>(-POS.x)));
            img->overflowRight  = std::max(img->overflowRight, std::max(0.F, sc<float>(POS.x + SIZE.x - MONITOR->m_size.x)));
            img->overflowTop    = std::max(img->overflowTop, std::max(0.F, sc<float>(-POS.y)));
            img->overflowBottom = std::max(img->overflowBottom, std::max(0.F, sc<float>(POS.y + SIZE.y - MONITOR->m_size.y)));
        }
    }
}

CBox CScrollOverview::workspaceOverviewVisibleBox(size_t workspaceIdx, const CBox& workspaceBox, float renderScale, PHLMONITOR monitor) const {
    if (workspaceIdx >= images.size() || !images[workspaceIdx] || !monitor)
        return workspaceBox;

    auto box = workspaceBox;
    const auto LEFT   = images[workspaceIdx]->overflowLeft * renderScale * monitor->m_scale;
    const auto RIGHT  = images[workspaceIdx]->overflowRight * renderScale * monitor->m_scale;
    const auto TOP    = images[workspaceIdx]->overflowTop * renderScale * monitor->m_scale;
    const auto BOTTOM = images[workspaceIdx]->overflowBottom * renderScale * monitor->m_scale;

    box.x -= LEFT;
    box.y -= TOP;
    box.width += LEFT + RIGHT;
    box.height += TOP + BOTTOM;

    return box;
}

PHLWINDOW CScrollOverview::windowAtOverviewPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx) const {
    size_t activeIdx = activeWorkspaceIndex();
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return nullptr;

    const auto WORKSPACEPITCH = getWorkspaceRenderedPitch(MONITOR, scale->value(), layout);
    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        const auto  offset = workspaceOverviewOffset(workspaceIdx, activeIdx, WORKSPACEPITCH);

        const auto selectWindow = [&](const PHLWINDOW& window) -> PHLWINDOW {
            if (hoveredWorkspaceIdx)
                *hoveredWorkspaceIdx = workspaceIdx;

            return window;
        };

        const auto fullscreenWindow = wimg->pWorkspace ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(wimg->pWorkspace)) : PHLWINDOW{};

        if (!isWorkspaceScrolling(wimg->pWorkspace) && shouldShowOverviewWindow(fullscreenWindow)) {
            for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
                const auto window = getOverviewWindowToShow(it->lock());
                if (!shouldShowOverviewWindow(window) || !window->m_isFloating)
                    continue;

                const auto texbox = getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), offset, layout);

                if (texbox.containsPoint(point))
                    return selectWindow(window);
            }

            const auto texbox = getOverviewWindowBox(fullscreenWindow, MONITOR, scale->value(), viewOffset->value(), offset, layout);

            if (texbox.containsPoint(point))
                return selectWindow(fullscreenWindow);

            continue;
        }

        for (const bool floating : {true, false}) {
            for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
                const auto window = getOverviewWindowToShow(it->lock());
                if (!shouldShowOverviewWindow(window) || window->m_isFloating != floating)
                    continue;

                const auto texbox = getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), offset, layout);

                if (texbox.containsPoint(point))
                    return selectWindow(window);
            }
        }
    }

    return nullptr;
}

PHLWINDOW CScrollOverview::windowAtOverviewCursor(size_t* hoveredWorkspaceIdx) {
    return windowAtOverviewPoint(lastMousePosLocal, hoveredWorkspaceIdx);
}

PHLWINDOW CScrollOverview::windowClosestToWorkspaceCenter(size_t workspaceIdx) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || workspaceIdx >= images.size() || !images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
        return {};

    const auto& WORKSPACEIMAGE  = images[workspaceIdx];
    const auto  SCALE           = scale->value();
    const auto  WORKSPACEOFFSET =
        workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, SCALE, layout));
    const auto WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto FULLSCREENWINDOW = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(WORKSPACEIMAGE->pWorkspace));
    const bool HASFULLSCREENPATH = !isWorkspaceScrolling(WORKSPACEIMAGE->pWorkspace) && shouldShowOverviewWindow(FULLSCREENWINDOW) &&
        FULLSCREENWINDOW->m_workspace == WORKSPACEIMAGE->pWorkspace;

    PHLWINDOW bestWindow;
    double    bestDistance = std::numeric_limits<double>::max();

    for (const auto& windowRef : WORKSPACEIMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(WINDOW))
            continue;
        if (HASFULLSCREENPATH && WINDOW != FULLSCREENWINDOW && !WINDOW->m_isFloating)
            continue;

        const auto WINDOWBOX = getOverviewWindowBox(WINDOW, MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        const auto DISTANCE  = overviewBoxCenterDistanceSquared(WINDOWBOX, WORKSPACEBOX);
        if (DISTANCE >= bestDistance)
            continue;

        bestWindow   = WINDOW;
        bestDistance = DISTANCE;
    }

    return bestWindow;
}

PHLWINDOW CScrollOverview::windowAtOverviewCursorOnWorkspace(size_t workspaceIdx, const PHLWINDOW& ignoredWindow, CBox* windowBox) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || workspaceIdx >= images.size() || !images[workspaceIdx])
        return nullptr;

    const auto ACTIVEIDX         = activeWorkspaceIndex();
    const auto WORKSPACE_OFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));

    PHLWINDOW bestWindow;
    CBox      bestBox;
    float     bestDistanceSq = std::numeric_limits<float>::max();

    for (const bool floating : {true, false}) {
        for (auto it = images[workspaceIdx]->windows.rbegin(); it != images[workspaceIdx]->windows.rend(); ++it) {
            const auto WINDOW = getOverviewWindowToShow(it->lock());
            if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating != floating)
                continue;

            const auto box    = getOverviewWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout);
            const auto hitbox = expandOverviewWindowHitbox(box, scale->value(), MONITOR->m_scale);
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

CDropIndicator::SDropAnchor CScrollOverview::dropAnchorAtOverviewCursorOnWorkspace(size_t workspaceIdx, const PHLWINDOW& ignoredWindow,
                                                                                   CScrollOverview* dragContext) {
    CDropIndicator::SDropAnchor result;
    const auto                  DRAGCONTEXT = dragContext ? dragContext : this;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || workspaceIdx >= images.size() || !images[workspaceIdx])
        return result;

    const auto& IMAGE     = images[workspaceIdx];
    const auto  WORKSPACE = IMAGE->pWorkspace;
    if (!WORKSPACE)
        return result;

    const auto WORKSPACE_OFFSET =
        workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    const auto WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout);

    if (!overviewBoxFullyVisibleOnMonitor(WORKSPACEBOX, MONITOR))
        return result;

    if (!DRAGCONTEXT->dragStartedTiled)
        return result;

    const auto boxesForWindow = [&](const PHLWINDOW& window) {
        const auto TARGET = window->layoutTarget();
        return std::pair{
            getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout, false),
            TARGET ? getOverviewGlobalBox(TARGET->position(), MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout, false) : CBox{},
        };
    };
    const auto setAnchor = [](CDropIndicator::SDropAnchor& anchor, const PHLWINDOW& window, const CBox& box, const CBox& logicalBox = {}, const std::string& direction = {}) {
        anchor.window     = window;
        anchor.box        = box;
        anchor.logicalBox = logicalBox;
        anchor.direction  = direction;
    };

    const auto directionForBox = [&](const CBox& box) {
        const auto LOCAL_X = lastMousePosLocal.x - box.x;
        const auto LOCAL_Y = lastMousePosLocal.y - box.y;

        if (LOCAL_X < box.width / 3.F)
            return std::string{"l"};
        if (LOCAL_X > box.width * 2.F / 3.F)
            return std::string{"r"};
        return LOCAL_Y < box.height / 2.F ? std::string{"u"} : std::string{"d"};
    };

    DRAGCONTEXT->refreshDragOriginalOverviewBoxes();

    const auto ORIGINALHITBOX =
        DRAGCONTEXT->dragOriginalOverviewHitbox.empty() ? DRAGCONTEXT->dragOriginalOverviewBox : DRAGCONTEXT->dragOriginalOverviewHitbox;
    if (ignoredWindow && !DRAGCONTEXT->dragOriginalOverviewBox.empty() && ORIGINALHITBOX.containsPoint(lastMousePosLocal) &&
        WORKSPACE == DRAGCONTEXT->dragOriginalWorkspace.lock()) {
        setAnchor(result, ignoredWindow, DRAGCONTEXT->dragOriginalOverviewBox);
        return result;
    }

    const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACE);
    const bool PRIMARYHORIZONTAL = ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller && ALGO->m_scrollingData->controller->isPrimaryHorizontal();
    const auto pointsToOriginalStackSlot = [&](const PHLWINDOW& anchor, const std::string& direction) {
        if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller || !ignoredWindow || DRAGCONTEXT->dragOriginalOverviewBox.empty() ||
            WORKSPACE != DRAGCONTEXT->dragOriginalWorkspace.lock() || !anchor || !anchor->layoutTarget() || !ignoredWindow->layoutTarget())
            return false;

        const auto ANCHORDATA  = ALGO->dataFor(anchor->layoutTarget());
        const auto ORIGINALDATA = ALGO->dataFor(ignoredWindow->layoutTarget());
        const auto ANCHORCOL   = ANCHORDATA ? ANCHORDATA->column.lock() : nullptr;
        const auto ORIGINALCOL = ORIGINALDATA ? ORIGINALDATA->column.lock() : nullptr;
        if (!ANCHORCOL || ANCHORCOL != ORIGINALCOL)
            return false;

        const auto ANCHORIDX   = ANCHORCOL->idx(anchor->layoutTarget());
        const auto ORIGINALIDX = ORIGINALCOL->idx(ignoredWindow->layoutTarget());
        if (ANCHORIDX == ORIGINALIDX)
            return false;

        const bool ANCHORPREVIOUS = ANCHORIDX < ORIGINALIDX;
        if ((ANCHORPREVIOUS && ANCHORIDX + 1 != ORIGINALIDX) || (!ANCHORPREVIOUS && ORIGINALIDX + 1 != ANCHORIDX))
            return false;

        if (PRIMARYHORIZONTAL)
            return (ANCHORPREVIOUS && direction == "d") || (!ANCHORPREVIOUS && direction == "u");

        return (ANCHORPREVIOUS && direction == "r") || (!ANCHORPREVIOUS && direction == "l");
    };
    const auto pointsToOriginalColumnSlot = [&](const PHLWINDOW& anchor, const std::string& direction) {
        if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller || !ignoredWindow || DRAGCONTEXT->dragOriginalOverviewBox.empty() ||
            WORKSPACE != DRAGCONTEXT->dragOriginalWorkspace.lock() || !anchor || !anchor->layoutTarget() || !ignoredWindow->layoutTarget())
            return false;

        const auto ANCHORDATA   = ALGO->dataFor(anchor->layoutTarget());
        const auto ORIGINALDATA = ALGO->dataFor(ignoredWindow->layoutTarget());
        const auto ANCHORCOL    = ANCHORDATA ? ANCHORDATA->column.lock() : nullptr;
        const auto ORIGINALCOL  = ORIGINALDATA ? ORIGINALDATA->column.lock() : nullptr;
        if (!ANCHORCOL || !ORIGINALCOL || ORIGINALCOL->targetDatas.size() != 1)
            return false;

        const auto ANCHORCOLIDX   = ALGO->m_scrollingData->idx(ANCHORCOL);
        const auto ORIGINALCOLIDX = ALGO->m_scrollingData->idx(ORIGINALCOL);
        if (ANCHORCOLIDX < 0 || ORIGINALCOLIDX < 0 || ANCHORCOLIDX == ORIGINALCOLIDX)
            return false;

        const bool ANCHORPREVIOUS = ANCHORCOLIDX < ORIGINALCOLIDX;
        if ((ANCHORPREVIOUS && ANCHORCOLIDX + 1 != ORIGINALCOLIDX) || (!ANCHORPREVIOUS && ORIGINALCOLIDX + 1 != ANCHORCOLIDX))
            return false;

        if (PRIMARYHORIZONTAL)
            return (ANCHORPREVIOUS && direction == "r") || (!ANCHORPREVIOUS && direction == "l");

        return (ANCHORPREVIOUS && direction == "d") || (!ANCHORPREVIOUS && direction == "u");
    };
    const auto normalizeOriginalSlotAnchor = [&]() {
        if (pointsToOriginalStackSlot(result.window, result.direction) || pointsToOriginalColumnSlot(result.window, result.direction))
            setAnchor(result, ignoredWindow, DRAGCONTEXT->dragOriginalOverviewBox);
    };

    float bestDistanceSq = std::numeric_limits<float>::max();
    for (auto it = IMAGE->windows.rbegin(); it != IMAGE->windows.rend(); ++it) {
        const auto WINDOW = getOverviewWindowToShow(it->lock());
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating)
            continue;

        const auto [BOX, LOGICALBOX] = boxesForWindow(WINDOW);
        const auto HITBOX            = LOGICALBOX.empty() ? BOX : LOGICALBOX;

        if (!HITBOX.containsPoint(lastMousePosLocal))
            continue;

        const auto distanceSq = overviewPointDistanceSqToBox(lastMousePosLocal, BOX);
        if (distanceSq >= bestDistanceSq)
            continue;

        setAnchor(result, WINDOW, BOX, LOGICALBOX, directionForBox(BOX));
        bestDistanceSq = distanceSq;
    }

    if (result.window) {
        normalizeOriginalSlotAnchor();
        return result;
    }

    if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller)
        return result;

    const auto POINTER   = PRIMARYHORIZONTAL ? sc<float>(lastMousePosLocal.x) : sc<float>(lastMousePosLocal.y);
    float      firstEdge = std::numeric_limits<float>::max();
    float      lastEdge  = std::numeric_limits<float>::lowest();

    for (const auto& windowRef : IMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating)
            continue;

        const auto [BOX, LOGICALBOX] = boxesForWindow(WINDOW);
        const auto HITBOX            = LOGICALBOX.empty() ? BOX : LOGICALBOX;
        const auto MIN               = PRIMARYHORIZONTAL ? sc<float>(HITBOX.x) : sc<float>(HITBOX.y);
        const auto MAX               = PRIMARYHORIZONTAL ? sc<float>(HITBOX.x + HITBOX.width) : sc<float>(HITBOX.y + HITBOX.height);

        firstEdge = std::min(firstEdge, MIN);
        lastEdge  = std::max(lastEdge, MAX);
    }

    if (POINTER >= firstEdge && POINTER <= lastEdge)
        return result;

    const bool FIND_FIRST = POINTER < firstEdge;
    float      bestEdge   = FIND_FIRST ? std::numeric_limits<float>::max() : std::numeric_limits<float>::lowest();

    for (const auto& windowRef : IMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating)
            continue;

        const auto [BOX, LOGICALBOX] = boxesForWindow(WINDOW);
        const auto HITBOX            = LOGICALBOX.empty() ? BOX : LOGICALBOX;
        const auto EDGE              = PRIMARYHORIZONTAL ? sc<float>(FIND_FIRST ? HITBOX.x : HITBOX.x + HITBOX.width) :
                                                           sc<float>(FIND_FIRST ? HITBOX.y : HITBOX.y + HITBOX.height);

        if (FIND_FIRST ? EDGE < bestEdge : EDGE > bestEdge) {
            bestEdge = EDGE;
            setAnchor(result, WINDOW, BOX, LOGICALBOX);
        }
    }

    if (result.window) {
        result.direction = PRIMARYHORIZONTAL ? (FIND_FIRST ? "l" : "r") : (FIND_FIRST ? "u" : "d");
        normalizeOriginalSlotAnchor();
    }

    return result;
}

PHLWORKSPACE CScrollOverview::workspaceAtOverviewPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return nullptr;

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto WORKSPACEBOX = getOverviewWorkspaceUsableBox(wimg->pWorkspace, MONITOR, scale->value(), viewOffset->value(),
                                                                workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout)),
                                                                layout);

        if (WORKSPACEBOX.containsPoint(point)) {
            if (hoveredWorkspaceIdx)
                *hoveredWorkspaceIdx = workspaceIdx;

            return wimg->pWorkspace;
        }
    }

    return nullptr;
}

PHLWORKSPACE CScrollOverview::workspaceAtOverviewDropPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx, const PHLWINDOW& draggedWindow) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return nullptr;

    const auto ACTIVEIDX       = activeWorkspaceIndex();
    const auto WORKSPACEPITCH = getWorkspaceRenderedPitch(MONITOR, scale->value(), layout);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, WORKSPACEPITCH);
        for (const bool floating : {true, false}) {
            for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
                const auto WINDOW = getOverviewWindowToShow(it->lock());
                if (!shouldShowOverviewWindow(WINDOW) || WINDOW == draggedWindow || WINDOW->m_isFloating != floating)
                    continue;

                const auto WINDOWBOX = getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
                if (!WINDOWBOX.containsPoint(point))
                    continue;

                if (hoveredWorkspaceIdx)
                    *hoveredWorkspaceIdx = workspaceIdx;

                return wimg->pWorkspace;
            }
        }
    }

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto WORKSPACEBOX = getOverviewWorkspaceUsableBox(wimg->pWorkspace, MONITOR, scale->value(), viewOffset->value(),
                                                                workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, WORKSPACEPITCH), layout);
        const bool ONLAYOUTAXIS = layout == ScrollOverview::Config::ELayout::HORIZONTAL ?
            point.x >= WORKSPACEBOX.x && point.x <= WORKSPACEBOX.x + WORKSPACEBOX.width :
            point.y >= WORKSPACEBOX.y && point.y <= WORKSPACEBOX.y + WORKSPACEBOX.height;

        if (WORKSPACEBOX.containsPoint(point) || (ONLAYOUTAXIS && isWorkspaceScrolling(wimg->pWorkspace))) {
            if (hoveredWorkspaceIdx)
                *hoveredWorkspaceIdx = workspaceIdx;

            return wimg->pWorkspace;
        }
    }

    return nullptr;
}

PHLWORKSPACE CScrollOverview::workspaceAtOverviewCursor(size_t* hoveredWorkspaceIdx) const {
    return workspaceAtOverviewPoint(lastMousePosLocal, hoveredWorkspaceIdx);
}

static void syncWorkspaceGeometry(const PHLWORKSPACE& workspace) {
    if (!workspace || !workspace->m_space)
        return;

    for (const auto& targetRef : workspace->m_space->targets()) {
        const auto TARGET = targetRef.lock();
        if (TARGET)
            TARGET->warpPositionSize();
    }
}

bool CScrollOverview::selectOverviewWindow(PHLWINDOW window, size_t workspaceIdx, bool syncFocus) {
    if (!window)
        return false;

    closeOnWindow            = window;
    viewportCurrentWorkspace = workspaceIdx;
    rememberSelection(window);
    if (syncFocus) {
        if (const auto MONITOR = pMonitor.lock(); MONITOR && Desktop::focusState()->monitor() != MONITOR)
            Desktop::focusState()->rawMonitorFocus(MONITOR);
        syncFocusedSelection();
    }
    damage();
    return true;
}

bool CScrollOverview::selectWindowAtOverviewCursor(bool syncFocus) {
    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

    size_t    workspaceIdx = viewportCurrentWorkspace;
    PHLWINDOW window       = windowAtOverviewCursor(&workspaceIdx);

    return selectOverviewWindow(window, workspaceIdx, syncFocus);
}

void CScrollOverview::selectHoveredWorkspace() {
    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

    size_t     workspaceIdx = viewportCurrentWorkspace;
    const auto WORKSPACE    = workspaceAtOverviewCursor(&workspaceIdx);
    if (!WORKSPACE)
        return;

    closeOnWindow.reset();
    viewportCurrentWorkspace = workspaceIdx;

    if (pMonitor && pMonitor->m_activeWorkspace != WORKSPACE) {
        if (focusSyncedFromWorkspaceID == WORKSPACE_INVALID)
            focusSyncedFromWorkspaceID = pMonitor->m_activeWorkspace ? pMonitor->m_activeWorkspace->m_id : WORKSPACE_INVALID;
        pMonitor->changeWorkspace(WORKSPACE, false, true, true);
    }

    damage();
}

bool CScrollOverview::windowDispatcherAction(const std::string& action) {
    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

    size_t    workspaceIdx = viewportCurrentWorkspace;
    PHLWINDOW WINDOW       = windowAtOverviewCursor(&workspaceIdx);

    if (!WINDOW)
        return false;

    if (action == "select")
        return selectOverviewWindow(WINDOW, workspaceIdx, true);

    if (action == "close") {
        WINDOW->sendClose();
        damage();
        return true;
    }

    return false;
}

Vector2D CScrollOverview::overviewPointToGlobal(size_t workspaceIdx, const Vector2D& pointLocal) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return pointLocal;

    const auto  SAFE_SCALE       = std::max(scale->value(), 0.01F);
    const auto  SAFE_MON_SCALE   = std::max(MONITOR->m_scale, 0.01F);
    const auto  VIEWPORT_CENTER  = CBox{{}, MONITOR->m_size * MONITOR->m_scale}.middle();
    const auto  VIEWPORT_CENTER_LOGICAL = CBox{{}, MONITOR->m_size}.middle();
    const auto  WORKSPACE_OFFSET   = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));

    return ((pointLocal - axisOffsetVector(WORKSPACE_OFFSET, layout) + viewOffset->value() * scale->value() * SAFE_MON_SCALE - VIEWPORT_CENTER) * (1.F / (SAFE_SCALE * SAFE_MON_SCALE))) +
        VIEWPORT_CENTER_LOGICAL + MONITOR->m_position;
}

CBox CScrollOverview::draggedWindowBox(size_t workspaceIdx) const {
    const auto WINDOW  = getOverviewWindowToShow(dragActiveWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!WINDOW || !MONITOR || workspaceIdx >= images.size())
        return {};

    const auto WORKSPACE_OFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    auto       box               = getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout);
    box.x = lastMousePosLocal.x - dragGrabOffsetLocal.x;
    box.y = lastMousePosLocal.y - dragGrabOffsetLocal.y;

    return box;
}

CBox CScrollOverview::draggedWindowBoxFor(PHLWINDOW window, size_t workspaceIdx, const Vector2D& pointLocal, const Vector2D& grabRatio) const {
    window             = getOverviewWindowToShow(window);
    const auto MONITOR = pMonitor.lock();
    if (!window || !MONITOR || workspaceIdx >= images.size())
        return {};

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    auto       box             = getOverviewDragWindowBox(window, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
    box.x = pointLocal.x - box.width * std::clamp(grabRatio.x, 0.0, 1.0);
    box.y = pointLocal.y - box.height * std::clamp(grabRatio.y, 0.0, 1.0);
    return box;
}

CBox CScrollOverview::draggedWindowGlobalBox() const {
    const auto WINDOW  = getOverviewWindowToShow(dragActiveWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!WINDOW || !MONITOR)
        return {};

    const auto WORKSPACEIDX = dragWorkspaceIndex(WINDOW);
    if (WORKSPACEIDX >= images.size())
        return {};

    const auto WORKSPACEOFFSET =
        workspaceOverviewOffset(WORKSPACEIDX, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    const auto SOURCEBOX =
        getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout, false);
    const auto GLOBALSIZE = SOURCEBOX.size() * (1.F / std::max(MONITOR->m_scale, 0.01F));
    const auto CURSOR     = g_pInputManager->getMouseCoordsInternal();

    return {
        CURSOR.x - GLOBALSIZE.x * std::clamp(dragGrabRatio.x, 0.0, 1.0),
        CURSOR.y - GLOBALSIZE.y * std::clamp(dragGrabRatio.y, 0.0, 1.0),
        GLOBALSIZE.x,
        GLOBALSIZE.y,
    };
}

void CScrollOverview::refreshDragOriginalOverviewBoxes() {
    const auto WINDOW    = getOverviewWindowToShow(dragActiveWindow.lock());
    const auto WORKSPACE = dragOriginalWorkspace.lock();
    const auto MONITOR   = pMonitor.lock();
    if (!WINDOW || !WORKSPACE || !MONITOR || dragOriginalVisualBox.empty() || dragOriginalBox.empty())
        return;

    size_t workspaceIdx = images.size();
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace == WORKSPACE) {
            workspaceIdx = i;
            break;
        }
    }

    if (workspaceIdx >= images.size())
        return;

    Vector2D tapeDelta;
    if (const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACE); ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller)
        tapeDelta = overviewScrollingCameraTranslation(ALGO) - dragOriginalTapeTranslation;

    auto visualBox = dragOriginalVisualBox;
    auto hitbox    = dragOriginalBox;
    visualBox.translate(tapeDelta);
    hitbox.translate(tapeDelta);

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    dragOriginalOverviewBox    = getOverviewGlobalBox(visualBox, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
    dragOriginalOverviewHitbox = getOverviewGlobalBox(hitbox, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout, false);
}

CBox CScrollOverview::resizedWindowBox() const {
    const auto WINDOW  = getOverviewWindowToShow(resizeActiveWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!WINDOW || !MONITOR || resizeWorkspaceIdx >= images.size() || !WINDOW->m_isFloating)
        return {};

    const float SCALEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
    const auto  TARGET      = WINDOW->layoutTarget();
    const auto  DELTA       = lastMousePosLocal - resizeStartMouseLocal;

    Vector2D    minSizePx   = Vector2D{1.F, 1.F};
    if (TARGET) {
        if (const auto MINSIZE = TARGET->minSize(); MINSIZE.has_value())
            minSizePx = Vector2D{std::max(1.F, sc<float>(MINSIZE->x * SCALEFACTOR)), std::max(1.F, sc<float>(MINSIZE->y * SCALEFACTOR))};
    }

    std::optional<Vector2D> maxSizePx;
    if (TARGET) {
        if (const auto MAXSIZE = TARGET->maxSize(); MAXSIZE.has_value() && MAXSIZE->x > 0.F && MAXSIZE->y > 0.F)
            maxSizePx = Vector2D{std::max(minSizePx.x, sc<double>(MAXSIZE->x * SCALEFACTOR)), std::max(minSizePx.y, sc<double>(MAXSIZE->y * SCALEFACTOR))};
    }

    auto box = resizedOverviewBoxFromCorner(resizeOriginalBox, DELTA, resizeCorner, minSizePx, maxSizePx);

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(resizeWorkspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    const auto WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto BORDERMARGIN = WINDOW->getRealBorderSize() * MONITOR->m_scale * scale->value();

    return clampResizedOverviewBoxToWorkspace(box, WORKSPACEBOX, resizeCorner, BORDERMARGIN);
}

void CScrollOverview::initializeWindowDrag(const SWindowDragSnapshot& snapshot, bool adoptedFromNative, bool crossMonitorDrag) {
    g_pseudoFocusedWindow.reset();
    g_pseudoFocusUntil = {};
    closeOnWindow      = snapshot.window;
    rememberSelection(snapshot.window);

    dragActiveWindow             = snapshot.window;
    dragOriginalWorkspace        = snapshot.workspace;
    dragOriginalBox              = snapshot.box;
    dragOriginalVisualBox        = snapshot.visualBox;
    dragOriginalOverviewBox      = CBox{};
    dragOriginalOverviewHitbox   = CBox{};
    dragOriginalFloatSize        = snapshot.floatSize;
    dragOriginalTapeTranslation  = snapshot.tapeTranslation;
    dragGrabOffsetLocal          = snapshot.grabOffsetLocal;
    dragGrabRatio                = snapshot.grabRatio;
    dragStartedTiled             = snapshot.startedTiled;
    dragAdoptedFromNative        = adoptedFromNative;
    dragCrossMonitor             = crossMonitorDrag;
    dragCancelledAwaitingRelease = false;
}

void CScrollOverview::beginWindowDrag(PHLWINDOW window) {
    const auto WINDOW = getOverviewWindowToShow(window);
    const auto TARGET = WINDOW ? WINDOW->layoutTarget() : nullptr;
    if (!shouldShowOverviewWindow(WINDOW) || !TARGET)
        return;

    size_t workspaceIdx = viewportCurrentWorkspace;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == WINDOW->m_workspace) {
            viewportCurrentWorkspace = i;
            workspaceIdx              = i;
            break;
        }
    }

    SWindowDragSnapshot snapshot{
        .window       = WINDOW,
        .workspace    = WINDOW->m_workspace,
        .box          = TARGET->position(),
        .visualBox    = WINDOW->m_group ? TARGET->position() : WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT),
        .floatSize    = TARGET->lastFloatingSize(),
        .startedTiled = !TARGET->floating(),
    };

    const auto MONITOR = pMonitor.lock();
    if (MONITOR && workspaceIdx < images.size()) {
        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
        const auto WINDOWBOX       = getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
        snapshot.grabOffsetLocal   = dragStartMouseLocal - WINDOWBOX.pos();
        snapshot.grabRatio         = Vector2D{
            WINDOWBOX.width > 0.0 ? snapshot.grabOffsetLocal.x / WINDOWBOX.width : 0.5,
            WINDOWBOX.height > 0.0 ? snapshot.grabOffsetLocal.y / WINDOWBOX.height : 0.5,
        };
    }

    if (MONITOR && workspaceIdx < images.size())
        snapshot.tapeTranslation = overviewScrollingCameraTranslation(overviewScrollingAlgorithmForWorkspace(snapshot.workspace));

    initializeWindowDrag(snapshot, false, ScrollOverview::Config::getCrossMonitorDrag());
    if (MONITOR && workspaceIdx < images.size())
        refreshDragOriginalOverviewBoxes();
    updateWindowDrag();
}

std::optional<CScrollOverview::SWindowDragSnapshot> CScrollOverview::snapshotNativeWindowDrag(Layout::Supplementary::CDragStateController* dragController,
                                                                                              PHLWINDOW expectedWindow) const {
    if (!dragController || dragController->mode() != MBIND_MOVE || !dragController->dragThresholdReached() || !g_pInputManager)
        return std::nullopt;

    const auto TARGET = dragController->target();
    if (!TARGET)
        return std::nullopt;

    const auto WINDOW = getOverviewWindowToShow(TARGET->window());
    if (!shouldShowOverviewWindow(WINDOW) || !WINDOW->layoutTarget())
        return std::nullopt;
    if (expectedWindow && WINDOW != getOverviewWindowToShow(expectedWindow))
        return std::nullopt;

    const auto BOX = TARGET->position();
    SWindowDragSnapshot snapshot{
        .window       = WINDOW,
        .workspace    = WINDOW->m_workspace,
        .box          = BOX,
        .visualBox    = WINDOW->m_group ? BOX : WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT),
        .floatSize    = TARGET->lastFloatingSize(),
        .startedTiled = dragController->draggingTiled(),
    };

    const auto CURSOR = g_pInputManager->getMouseCoordsInternal();
    snapshot.grabRatio = Vector2D{
        snapshot.visualBox.width > 0.0 ? (CURSOR.x - snapshot.visualBox.x) / snapshot.visualBox.width : 0.5,
        snapshot.visualBox.height > 0.0 ? (CURSOR.y - snapshot.visualBox.y) / snapshot.visualBox.height : 0.5,
    };

    return snapshot;
}

bool CScrollOverview::adoptNativeWindowDrag(PHLWINDOW expectedWindow) {
    if (dragActiveWindow || g_finishingAdoptedNativeDrag || !g_layoutManager)
        return false;

    const auto& DRAGCONTROLLER = g_layoutManager->dragController();
    auto        snapshot       = snapshotNativeWindowDrag(DRAGCONTROLLER.get(), expectedWindow);
    if (!snapshot)
        return false;

    g_finishingAdoptedNativeDrag = true;
    [[maybe_unused]] auto restoreFinishingAdoptedDrag = Hyprutils::Utils::CScopeGuard([] { g_finishingAdoptedNativeDrag = false; });
    finishNativeDragAdoption(DRAGCONTROLLER.get());
    g_layoutManager->endDragTarget();

    if (!shouldShowOverviewWindow(snapshot->window) || !snapshot->window->layoutTarget())
        return false;

    snapshot->tapeTranslation = overviewScrollingCameraTranslation(overviewScrollingAlgorithmForWorkspace(snapshot->workspace));
    initializeWindowDrag(*snapshot, true, true);
    dragStartMouseLocal   = getOverviewMousePosLocal(pMonitor.lock());
    lastMousePosLocal     = dragStartMouseLocal;
    g_pointerGrabOverview = this;

    requestInputFrame();
    damage();
    return true;
}

void CScrollOverview::beginWindowResize() {
    const auto WINDOW  = getOverviewWindowToShow(resizePendingWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!shouldShowOverviewWindow(WINDOW) || shouldShowPinnedFloatingOverviewWindow(WINDOW) || !WINDOW->layoutTarget() || !MONITOR || resizeWorkspaceIdx >= images.size())
        return;

    closeOnWindow = WINDOW;
    rememberSelection(WINDOW);

    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == WINDOW->m_workspace) {
            viewportCurrentWorkspace = i;
            resizeWorkspaceIdx       = i;
            break;
        }
    }

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(resizeWorkspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    resizeOriginalBox   = getOverviewWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
    resizeActiveWindow  = WINDOW;
    resizeLastMouseLocal = lastMousePosLocal;

    updateWindowResize();
}

void CScrollOverview::ensureDragDestinationOverview() {
    if (!dragActiveWindow || !g_pInputManager || !dragCrossMonitor)
        return;

    const auto GLOBALCURSOR = g_pInputManager->getMouseCoordsInternal();
    auto       monitor      = State::monitorState()->query().vec(GLOBALCURSOR).run();
    if (!monitor || !monitor->m_enabled || !monitor->logicalBox().containsPoint(GLOBALCURSOR) || monitor == pMonitor.lock())
        return;

    if (const auto EXISTING = scrollOverviewForMonitor(monitor)) {
        if (EXISTING->isClosing()) {
            EXISTING->reopen();
            std::erase_if(dragTransientOverviews, [&EXISTING](const auto& transient) { return transient == EXISTING; });
        }
        return;
    }

    const auto [overview, created] = openOverview(monitor);
    if (!overview)
        return;

    if (created && overview.get() != this)
        dragTransientOverviews.emplace_back(overview);
    else if (overview->isClosing()) {
        overview->reopen();
        std::erase_if(dragTransientOverviews, [&overview](const auto& transient) { return transient == overview; });
    }

    overview->requestInputFrame();
    overview->damage();
}

void CScrollOverview::updateWindowDrag() {
    if (!dragActiveWindow)
        return;

    const auto WINDOW = getOverviewWindowToShow(dragActiveWindow.lock());
    if (!shouldShowOverviewWindow(WINDOW) || !WINDOW->layoutTarget()) {
        cancelWindowDrag();
        return;
    }

    ensureDragDestinationOverview();

    for (const auto& overview : scrollOverviews()) {
        if (!overview)
            continue;

        overview->requestInputFrame();
        overview->damage();
    }
}

void CScrollOverview::updateWindowResize() {
    const auto WINDOW  = getOverviewWindowToShow(resizeActiveWindow.lock());
    const auto TARGET  = WINDOW ? WINDOW->layoutTarget() : nullptr;
    const auto MONITOR = pMonitor.lock();

    if (!WINDOW || !TARGET || !MONITOR || resizeWorkspaceIdx >= images.size())
        return;

    TARGET->damageEntire();

    if (WINDOW->m_isFloating) {
        const auto RESIZEDBOX  = resizedWindowBox();
        const auto SCALEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
        const auto GLOBALBOX   = CBox{overviewPointToGlobal(resizeWorkspaceIdx, RESIZEDBOX.pos()), RESIZEDBOX.size() * (1.F / SCALEFACTOR)};

        TARGET->rememberFloatingSize(GLOBALBOX.size());
        TARGET->setPositionGlobal(GLOBALBOX);
        TARGET->warpPositionSize();
    } else {
        const auto DELTALOCAL = lastMousePosLocal - resizeLastMouseLocal;
        const auto SAFEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
        const auto DELTAGLOBAL = DELTALOCAL * (1.F / SAFEFACTOR);

        if (std::abs(DELTAGLOBAL.x) > 0.01F || std::abs(DELTAGLOBAL.y) > 0.01F) {
            g_layoutManager->resizeTarget(DELTAGLOBAL, TARGET, resizeCorner);
            syncWorkspaceGeometry(TARGET->workspace());
            resizeLastMouseLocal = lastMousePosLocal;
        }
    }

    TARGET->damageEntire();
    damage();
}

void CScrollOverview::finishWindowDragSession(const SP<IOverview>& persistentDestination) {
    clearDragPending();

    auto transientOverviews = std::move(dragTransientOverviews);
    dragTransientOverviews.clear();

    const bool DESTINATIONCREATEDBYDRAG = persistentDestination && std::ranges::any_of(transientOverviews, [&persistentDestination](const auto& ref) {
        return ref.lock() == persistentDestination;
    });
    if (DESTINATIONCREATEDBYDRAG)
        markCrossMonitorDragSession(this, persistentDestination);

    for (const auto& transientRef : transientOverviews) {
        const auto TRANSIENT = transientRef.lock();
        if (!TRANSIENT || TRANSIENT == persistentDestination || TRANSIENT.get() == this || std::ranges::find(scrollOverviews(), TRANSIENT) == scrollOverviews().end())
            continue;

        TRANSIENT->dismissTransient();
    }

    for (const auto& overview : scrollOverviews()) {
        if (!overview)
            continue;

        overview->requestInputFrame();
        overview->damage();
    }
}

void CScrollOverview::cancelWindowDrag() {
    if (!dragActiveWindow && dragTransientOverviews.empty())
        return;

    if (dragActiveWindow && g_pointerGrabOverview == this)
        dragCancelledAwaitingRelease = true;

    finishWindowDragSession();
}

void CScrollOverview::clearDragPending() {
    dragPendingPrimary          = false;
    dragActiveWindow.reset();
    dragOriginalWorkspace.reset();
    dragStartedTiled            = false;
    dragCrossMonitor            = false;
    dragOriginalFloatSize       = Vector2D{};
    dragOriginalTapeTranslation = Vector2D{};
    dragGrabOffsetLocal         = Vector2D{};
    dragGrabRatio               = Vector2D{0.5, 0.5};
    dragOriginalBox             = CBox{};
    dragOriginalVisualBox       = CBox{};
    dragOriginalOverviewBox     = CBox{};
    dragOriginalOverviewHitbox  = CBox{};
}

void CScrollOverview::updateScrollingPan() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    const auto WORKSPACE = scrollingPanWorkspace.lock();
    const auto ALGO      = overviewScrollingAlgorithmForWorkspace(WORKSPACE);
    if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller) {
        scrollingPanLastMouseLocal = lastMousePosLocal;
        return;
    }

    const auto DELTALOCAL = lastMousePosLocal - scrollingPanLastMouseLocal;
    const auto SAFEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
    const auto DELTAGLOBAL = DELTALOCAL * (1.F / SAFEFACTOR);
    const auto DELTA       = ALGO->m_scrollingData->controller->isPrimaryHorizontal() ? DELTAGLOBAL.x : DELTAGLOBAL.y;

    if (std::abs(DELTA) > 0.01F) {
        const double OFFSET = ALGO->m_scrollingData->controller->getOffset() - DELTA;
        ALGO->m_scrollingData->controller->setOffset(OFFSET);
        ALGO->m_scrollingData->lockedCameraOffset = OFFSET;
        ALGO->m_scrollingData->recalculate(true);
        scrollingPanLastMouseLocal = lastMousePosLocal;
        markBlurDirty();
        damage();
    }
}

void CScrollOverview::beginScrollingPan(PHLWORKSPACE workspace) {
    const auto ALGO = overviewScrollingAlgorithmForWorkspace(workspace);
    if (!ALGO)
        return;

    scrollingPanPointerDown    = true;
    scrollingPanWorkspace      = workspace;
    scrollingPanLastMouseLocal = lastMousePosLocal;
    scrollingPanInitialWindow  = getOverviewWindowToShow(closeOnWindow.lock());
    if (!scrollingPanInitialWindow || scrollingPanInitialWindow->m_workspace != workspace)
        scrollingPanInitialWindow = getOverviewWindowToShow(Desktop::focusState()->window());
    if (scrollingPanInitialWindow && scrollingPanInitialWindow->m_workspace != workspace)
        scrollingPanInitialWindow.reset();

    ALGO->m_scrollingData->lockedCameraOffset = ALGO->m_scrollingData->controller->getOffset();

    if (Desktop::focusState()->window() && Desktop::focusState()->window()->m_workspace == workspace)
        Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
}

void CScrollOverview::endScrollingPan() {
    const auto WORKSPACE = scrollingPanWorkspace.lock();
    const auto ALGO      = overviewScrollingAlgorithmForWorkspace(WORKSPACE);
    if (ALGO) {
        const double OFFSET = clampOverviewScrollingOffset(ALGO, ALGO->m_scrollingData->controller->getOffset());
        ALGO->m_scrollingData->controller->setOffset(OFFSET);
        ALGO->m_scrollingData->lockedCameraOffset.reset();
        ALGO->m_scrollingData->recalculate();
    }

    scrollingPanPointerDown    = false;
    scrollingPanLastMouseLocal = Vector2D{};
    scrollingPanWorkspace.reset();

    focusMostVisibleScrollingWindow(WORKSPACE);
    scrollingPanInitialWindow.reset();
}

void CScrollOverview::focusMostVisibleScrollingWindow(const PHLWORKSPACE& workspace) {
    const auto MONITOR = workspace && workspace->m_monitor ? workspace->m_monitor.lock() : pMonitor.lock();
    if (!workspace || !MONITOR)
        return;

    PHLWINDOW  bestFullWindow;
    PHLWINDOW  bestPartialWindow;
    double     bestFullDistance     = std::numeric_limits<double>::max();
    double     bestPartialVisibleArea = 0.0;
    double     bestPartialDistance  = std::numeric_limits<double>::max();
    const auto MONITORBOX           = MONITOR->logicalBox();
    const auto ALGO                 = overviewScrollingAlgorithmForWorkspace(workspace);
    auto       WORKSPACEBOX         = ALGO ? ALGO->usableArea() : MONITORBOX;
    if (ALGO)
        WORKSPACEBOX.translate(MONITOR->m_position);

    const auto preferredWindow = getOverviewWindowToShow(scrollingPanInitialWindow.lock());
    if (shouldShowOverviewWindow(preferredWindow) && preferredWindow->m_workspace == workspace && !preferredWindow->m_isFloating && preferredWindow->layoutTarget()) {
        const auto WINDOWBOX   = preferredWindow->layoutTarget()->position();
        const auto AREA        = overviewBoxArea(WINDOWBOX);
        const auto VISIBLEAREA = overviewBoxIntersectionArea(WINDOWBOX, WORKSPACEBOX);
        if (AREA > 0.0 && std::abs(VISIBLEAREA - AREA) < 0.5) {
            closeOnWindow = preferredWindow;
            rememberSelection(preferredWindow);
            Desktop::focusState()->fullWindowFocus(preferredWindow, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
            return;
        }
    }

    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_workspace != workspace || WINDOW->m_isFloating || !WINDOW->layoutTarget())
            continue;

        const auto WINDOWBOX = WINDOW->layoutTarget()->position();
        const auto AREA      = overviewBoxArea(WINDOWBOX);
        if (AREA <= 0.0)
            continue;

        const auto VISIBLEAREA = overviewBoxIntersectionArea(WINDOWBOX, WORKSPACEBOX);
        if (VISIBLEAREA <= 0.0)
            continue;

        const auto DISTANCE = overviewBoxCenterDistanceSquared(WINDOWBOX, WORKSPACEBOX);
        if (std::abs(VISIBLEAREA - AREA) < 0.5) {
            if (DISTANCE < bestFullDistance) {
                bestFullWindow   = WINDOW;
                bestFullDistance = DISTANCE;
            }
            continue;
        }

        if (VISIBLEAREA > bestPartialVisibleArea || (std::abs(VISIBLEAREA - bestPartialVisibleArea) < 0.5 && DISTANCE < bestPartialDistance)) {
            bestPartialWindow     = WINDOW;
            bestPartialVisibleArea = VISIBLEAREA;
            bestPartialDistance   = DISTANCE;
        }
    }

    const auto bestWindow = bestFullWindow ? bestFullWindow : bestPartialWindow;
    if (!bestWindow)
        return;

    closeOnWindow = bestWindow;
    rememberSelection(bestWindow);
    Desktop::focusState()->fullWindowFocus(bestWindow, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
}

bool CScrollOverview::moveScrollingColumnSelection(bool next) {
    if (images.empty() || viewportCurrentWorkspace >= images.size())
        return false;

    const auto& WORKSPACEIMAGE = images[viewportCurrentWorkspace];
    if (!WORKSPACEIMAGE || !WORKSPACEIMAGE->pWorkspace)
        return false;

    const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACEIMAGE->pWorkspace);
    if (!ALGO || !ALGO->m_scrollingData || ALGO->m_scrollingData->columns.empty())
        return false;

    if (!closeOnWindow || closeOnWindow->m_workspace != WORKSPACEIMAGE->pWorkspace || !shouldShowOverviewWindow(closeOnWindow.lock()) || closeOnWindow->m_isFloating)
        syncSelectionToViewport();

    const auto CURRENT = getOverviewWindowToShow(closeOnWindow.lock());
    const auto CURRENTDATA = CURRENT && CURRENT->layoutTarget() ? ALGO->dataFor(CURRENT->layoutTarget()) : nullptr;
    const auto CURRENTCOL  = CURRENTDATA ? CURRENTDATA->column.lock() : ALGO->currentColumn();
    if (!CURRENTCOL)
        return false;

    const auto firstWindowInColumn = [&](const SP<Layout::Tiled::SColumnData>& column) -> PHLWINDOW {
        if (!column)
            return {};

        for (const auto& targetData : column->targetDatas) {
            if (!targetData || !targetData->target)
                continue;

            for (const auto& windowRef : WORKSPACEIMAGE->windows) {
                const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
                if (shouldShowOverviewWindow(WINDOW) && !WINDOW->m_isFloating && WINDOW->layoutTarget() == targetData->target && WINDOW->m_workspace == WORKSPACEIMAGE->pWorkspace)
                    return WINDOW;
            }
        }

        return {};
    };

    std::vector<SP<Layout::Tiled::SColumnData>> columns;
    for (const auto& column : ALGO->m_scrollingData->columns) {
        if (firstWindowInColumn(column))
            columns.emplace_back(column);
    }

    const bool PRIMARYHORIZONTAL = ALGO->m_scrollingData->controller && ALGO->m_scrollingData->controller->isPrimaryHorizontal();
    std::ranges::sort(columns, [PRIMARYHORIZONTAL](const auto& a, const auto& b) {
        const auto getColumnPosition = [PRIMARYHORIZONTAL](const auto& column) {
            if (!column || column->targetDatas.empty() || !column->targetDatas[0])
                return std::numeric_limits<double>::max();

            return PRIMARYHORIZONTAL ? column->targetDatas[0]->layoutBox.x : column->targetDatas[0]->layoutBox.y;
        };

        return getColumnPosition(a) < getColumnPosition(b);
    });

    const auto CURRENTIT = std::ranges::find(columns, CURRENTCOL);
    if (CURRENTIT == columns.end())
        return false;

    const auto CURRENTIDX = sc<int64_t>(std::distance(columns.begin(), CURRENTIT));
    const auto TARGETIDX  = CURRENTIDX + (next ? 1 : -1);
    if (TARGETIDX < 0 || TARGETIDX >= sc<int64_t>(columns.size()))
        return false;

    const auto TARGETCOL = columns[TARGETIDX];
    const auto WINDOW    = firstWindowInColumn(TARGETCOL);
    if (!WINDOW)
        return false;

    ALGO->m_scrollingData->centerOrFitCol(TARGETCOL);
    ALGO->m_scrollingData->recalculate();

    closeOnWindow = WINDOW;
    rememberSelection(WINDOW);
    syncFocusedSelection();
    damage();

    return true;
}

bool CScrollOverview::moveScrollingStackSelection(bool next) {
    if (images.empty() || viewportCurrentWorkspace >= images.size())
        return false;

    const auto& WORKSPACEIMAGE = images[viewportCurrentWorkspace];
    if (!WORKSPACEIMAGE || !WORKSPACEIMAGE->pWorkspace)
        return false;

    const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACEIMAGE->pWorkspace);
    if (!ALGO || !ALGO->m_scrollingData || ALGO->m_scrollingData->columns.empty())
        return false;

    if (!closeOnWindow || closeOnWindow->m_workspace != WORKSPACEIMAGE->pWorkspace || !shouldShowOverviewWindow(closeOnWindow.lock()) || closeOnWindow->m_isFloating)
        syncSelectionToViewport();

    const auto CURRENT = getOverviewWindowToShow(closeOnWindow.lock());
    if (!CURRENT || !CURRENT->layoutTarget())
        return false;

    const auto CURRENTDATA = ALGO->dataFor(CURRENT->layoutTarget());
    const auto CURRENTCOL  = CURRENTDATA ? CURRENTDATA->column.lock() : nullptr;
    if (!CURRENTCOL)
        return false;

    const auto CURRENTIDX = CURRENTCOL->idx(CURRENT->layoutTarget());
    const auto TARGETIDX  = CURRENTIDX + (next ? 1 : -1);
    if (TARGETIDX < 0 || TARGETIDX >= sc<int>(CURRENTCOL->targetDatas.size()))
        return false;

    const auto TARGETDATA = CURRENTCOL->targetDatas[TARGETIDX];
    if (!TARGETDATA || !TARGETDATA->target)
        return false;

    for (const auto& windowRef : WORKSPACEIMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (shouldShowOverviewWindow(WINDOW) && !WINDOW->m_isFloating && WINDOW->layoutTarget() == TARGETDATA->target && WINDOW->m_workspace == WORKSPACEIMAGE->pWorkspace) {
            closeOnWindow = WINDOW;
            rememberSelection(WINDOW);
            syncFocusedSelection();
            damage();
            return true;
        }
    }

    return false;
}

void CScrollOverview::endWindowDrag() {
    SP<IOverview> persistentDestination;
    auto          finishDragSession = Hyprutils::Utils::CScopeGuard([this, &persistentDestination] { finishWindowDragSession(persistentDestination); });

    ensureDragDestinationOverview();

    const auto WINDOW = getOverviewWindowToShow(dragActiveWindow.lock());
    const auto TARGET = WINDOW ? WINDOW->layoutTarget() : nullptr;
    const auto SPACE  = TARGET ? TARGET->space() : nullptr;
    const auto ALGO   = SPACE ? SPACE->algorithm() : nullptr;
    if (!WINDOW || !TARGET)
        return;

    const auto          targetOverview = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
    auto*               dropOverview   = targetOverview ? dynamic_cast<CScrollOverview*>(targetOverview.get()) : nullptr;

    const auto          DROPMONITOR       = dropOverview ? dropOverview->pMonitor.lock() : PHLMONITOR{};
    const auto          DROPPOINTLOCAL    = getOverviewMousePosLocal(DROPMONITOR);
    const bool          RETILEONEND       = dragStartedTiled && TARGET && SPACE && ALGO;
    size_t              dropWorkspaceIdx  = 0;
    const auto          DROPWORKSPACE     = dropOverview ? dropOverview->workspaceAtOverviewDropPoint(DROPPOINTLOCAL, &dropWorkspaceIdx, WINDOW) : PHLWORKSPACE{};
    const auto          DROPSCROLLINGALGO  = overviewScrollingAlgorithmForWorkspace(DROPWORKSPACE);
    const bool          DROPSCROLLINGLAYOUT = DROPSCROLLINGALGO != nullptr;
    const bool          DROPSCROLLINGPRIMARYHORIZONTAL =
        DROPSCROLLINGALGO && DROPSCROLLINGALGO->m_scrollingData && DROPSCROLLINGALGO->m_scrollingData->controller &&
        DROPSCROLLINGALGO->m_scrollingData->controller->isPrimaryHorizontal();
    const auto          ORIGINALWORKSPACE = dragOriginalWorkspace.lock();
    const bool          MOVEWORKSPACE    = DROPWORKSPACE && DROPWORKSPACE != ORIGINALWORKSPACE;
    const auto          DRAGBOX          = DROPWORKSPACE ? dropOverview->draggedWindowBoxFor(WINDOW, dropWorkspaceIdx, DROPPOINTLOCAL, dragGrabRatio) : CBox{};
    int                 dropSide         = 0;
    CDropIndicator::SDropAnchor dropAnchor;
    std::string         dropDirection;

    if (!DROPWORKSPACE)
        return;

    if (dropOverview != this)
        persistentDestination = targetOverview;

    const auto SOURCEFULLSCREENWINDOW = ORIGINALWORKSPACE ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(ORIGINALWORKSPACE)) : PHLWINDOW{};
    const bool RESTORESOURCEFULLSCREENFOCUS = WINDOW && WINDOW->m_isFloating && MOVEWORKSPACE && shouldShowOverviewWindow(SOURCEFULLSCREENWINDOW) &&
        SOURCEFULLSCREENWINDOW != WINDOW && SOURCEFULLSCREENWINDOW->m_workspace == ORIGINALWORKSPACE && Fullscreen::controller()->isFullscreen(SOURCEFULLSCREENWINDOW);

    const bool DROPSIDEHORIZONTAL = dropOverview->layout != ScrollOverview::Config::ELayout::HORIZONTAL || DROPSCROLLINGPRIMARYHORIZONTAL;

    const auto MONITOR = pMonitor.lock();
    const auto ACTIVEWORKSPACEBEFOREDROP = MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{};
    const auto DROPACTIVEWORKSPACEBEFOREDROP = DROPMONITOR ? DROPMONITOR->m_activeWorkspace : PHLWORKSPACE{};
    const auto RESTOREACTIVEWORKSPACE = [&]() {
        if (MONITOR && ACTIVEWORKSPACEBEFOREDROP && MONITOR->m_activeWorkspace != ACTIVEWORKSPACEBEFOREDROP)
            MONITOR->changeWorkspace(ACTIVEWORKSPACEBEFOREDROP, false, true, true);
        if (DROPMONITOR && DROPMONITOR != MONITOR && DROPACTIVEWORKSPACEBEFOREDROP && DROPMONITOR->m_activeWorkspace != DROPACTIVEWORKSPACEBEFOREDROP)
            DROPMONITOR->changeWorkspace(DROPACTIVEWORKSPACEBEFOREDROP, false, true, true);
    };
    const auto RESTOREVIEWPORTWORKSPACE = [&]() {
        if (!ACTIVEWORKSPACEBEFOREDROP)
            return;

        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i] && images[i]->pWorkspace == ACTIVEWORKSPACEBEFOREDROP) {
                viewportCurrentWorkspace = i;
                return;
            }
        }
    };

    bool       dropWorkspaceFullyVisible = false;
    if (DROPMONITOR) {
        const auto WORKSPACEBOX =
            getOverviewWorkspaceBox(DROPMONITOR, dropOverview->scale->value(), dropOverview->viewOffset->value(),
                                    dropOverview->workspaceOverviewOffset(dropWorkspaceIdx, dropOverview->activeWorkspaceIndex(),
                                                                         getWorkspaceRenderedPitch(DROPMONITOR, dropOverview->scale->value(), dropOverview->layout)),
                                    dropOverview->layout);
        dropWorkspaceFullyVisible = overviewBoxFullyVisibleOnMonitor(WORKSPACEBOX, DROPMONITOR);

        if (DROPSIDEHORIZONTAL) {
            if (DROPPOINTLOCAL.x < WORKSPACEBOX.x)
                dropSide = -1;
            else if (DROPPOINTLOCAL.x > WORKSPACEBOX.x + WORKSPACEBOX.width)
                dropSide = 1;
        } else {
            if (DROPPOINTLOCAL.y < WORKSPACEBOX.y)
                dropSide = -1;
            else if (DROPPOINTLOCAL.y > WORKSPACEBOX.y + WORKSPACEBOX.height)
                dropSide = 1;
        }
    }

    if (DROPWORKSPACE) {
        dropOverview->lastMousePosLocal = DROPPOINTLOCAL;
        dropAnchor                      = dropOverview->dropAnchorAtOverviewCursorOnWorkspace(dropWorkspaceIdx, WINDOW, this);
    }

    const auto DROPANCHOR = dropAnchor.window;
    dropDirection         = dropAnchor.direction;
    const bool DROPPINGONSCROLLINGCROSSAXIS =
        DROPSCROLLINGPRIMARYHORIZONTAL ? dropDirection == "u" || dropDirection == "d" : dropDirection == "l" || dropDirection == "r";

    if (DROPSCROLLINGLAYOUT && DROPANCHOR && DROPPINGONSCROLLINGCROSSAXIS && Fullscreen::controller()->isFullscreen(DROPANCHOR)) {
        Fullscreen::controller()->setFullscreenMode(DROPANCHOR, Fullscreen::FSMODE_NONE);
        if (const auto ANCHORDATA = DROPSCROLLINGALGO->dataFor(DROPANCHOR->layoutTarget()); ANCHORDATA) {
            if (const auto ANCHORCOL = ANCHORDATA->column.lock())
                ANCHORCOL->setColumnWidth(1.F);
        }
    }

    if (RETILEONEND && MOVEWORKSPACE) {
        Desktop::globalWindowController()->moveWindowToWorkspace(WINDOW, DROPWORKSPACE);
        RESTOREACTIVEWORKSPACE();

        if (DROPSCROLLINGLAYOUT) {
            if (!dropWorkspaceFullyVisible)
                moveOverviewScrollingTargetToWorkspaceEdge(TARGET, 1);
            else if (DROPANCHOR && !dropDirection.empty())
                moveOverviewTargetNextToWindow(TARGET, DROPANCHOR, dropDirection);
            else if (!DROPANCHOR)
                moveOverviewScrollingTargetToWorkspaceEdge(TARGET, dropSide);
        }

        TARGET->rememberFloatingSize(dragOriginalFloatSize);
        RESTOREACTIVEWORKSPACE();
        RESTOREVIEWPORTWORKSPACE();
    } else if (RETILEONEND) {
        TARGET->damageEntire();

        if (DROPANCHOR && !dropDirection.empty() && !DROPSCROLLINGLAYOUT && DROPANCHOR->layoutTarget()) {
            DROPANCHOR->layoutTarget()->damageEntire();
            g_layoutManager->switchTargets(TARGET, DROPANCHOR->layoutTarget(), true);
            DROPANCHOR->layoutTarget()->damageEntire();
        } else if (DROPANCHOR && !dropDirection.empty())
            moveOverviewTargetNextToWindow(TARGET, DROPANCHOR, dropDirection);
        else if (DROPSCROLLINGLAYOUT && !DROPANCHOR)
            moveOverviewScrollingTargetToWorkspaceEdge(TARGET, dropSide);

        TARGET->rememberFloatingSize(dragOriginalFloatSize);
        TARGET->warpPositionSize();
        TARGET->damageEntire();

        Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

        if (const auto WORKSPACE = SPACE->workspace())
        WORKSPACE->updateWindows();
    } else if (WINDOW && MOVEWORKSPACE) {
        Desktop::globalWindowController()->moveWindowToWorkspace(WINDOW, DROPWORKSPACE);
        RESTOREACTIVEWORKSPACE();
        if (TARGET) {
            const auto GLOBALSIZE = DRAGBOX.size() * (1.F / (std::max(dropOverview->scale->value(), 0.01F) * std::max(DROPMONITOR ? DROPMONITOR->m_scale : 1.F, 0.01F)));
            auto       GLOBALBOX  = dropWorkspaceFullyVisible ? CBox{dropOverview->overviewPointToGlobal(dropWorkspaceIdx, DRAGBOX.pos()), GLOBALSIZE} :
                                                                centerBoxInWorkspace(CBox{Vector2D{}, GLOBALSIZE}, DROPWORKSPACE, DROPMONITOR);
            if (dropWorkspaceFullyVisible)
                GLOBALBOX = clampBoxToWorkspace(GLOBALBOX, DROPWORKSPACE, DROPMONITOR, WINDOW->getRealBorderSize());

            TARGET->setPositionGlobal(GLOBALBOX);
            TARGET->warpPositionSize();
        }

        RESTOREACTIVEWORKSPACE();
        RESTOREVIEWPORTWORKSPACE();
    } else if (TARGET && !dragStartedTiled) {
        const auto workspaceIdx = dragWorkspaceIndex(WINDOW);

        const auto FLOATBOX   = draggedWindowBox(workspaceIdx);
        const auto GLOBALPOS  = overviewPointToGlobal(workspaceIdx, FLOATBOX.pos());
        const auto GLOBALSIZE = FLOATBOX.size() * (1.F / (std::max(scale->value(), 0.01F) * std::max(MONITOR ? MONITOR->m_scale : 1.F, 0.01F)));
        auto       GLOBALBOX  = CBox{GLOBALPOS, GLOBALSIZE};
        const auto WORKSPACE  = workspaceIdx < images.size() && images[workspaceIdx] ? images[workspaceIdx]->pWorkspace : WINDOW->m_workspace;

        GLOBALBOX = clampBoxToWorkspace(GLOBALBOX, WORKSPACE, MONITOR, WINDOW->getRealBorderSize());

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

    if (DROPWORKSPACE && DROPMONITOR && DROPWORKSPACE == DROPMONITOR->m_activeWorkspace) {
        const auto FULLSCREENWINDOW = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(DROPWORKSPACE));
        if (shouldShowOverviewWindow(FULLSCREENWINDOW) && FULLSCREENWINDOW->m_workspace == DROPWORKSPACE)
            dropOverview->emitFullscreenVisibilityState(FULLSCREENWINDOW, true);
    }

    if (WINDOW && DROPMONITOR && DROPWORKSPACE == DROPMONITOR->m_activeWorkspace) {
        dropOverview->selectOverviewWindow(WINDOW, dropWorkspaceIdx, true);
        g_pseudoFocusedWindow = WINDOW;
        g_pseudoFocusUntil    = Time::steadyNow() + POST_DROP_PSEUDO_FOCUS_DURATION;
    }

    dragCancelledAwaitingRelease = false;
    rebuildPending               = true;
    damage();
    if (dropOverview != this) {
        dropOverview->rebuildPending = true;
        dropOverview->damage();
    }
}

void CScrollOverview::endWindowResize() {
    const auto WINDOW  = getOverviewWindowToShow(resizeActiveWindow.lock());
    const auto TARGET  = WINDOW ? WINDOW->layoutTarget() : nullptr;
    const auto MONITOR = pMonitor.lock();

    if (WINDOW && TARGET && WINDOW->m_isFloating && resizeWorkspaceIdx < images.size()) {
        const auto RESIZEDBOX  = resizedWindowBox();
        const auto SCALEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR ? MONITOR->m_scale : 1.F, 0.01F);
        auto       GLOBALBOX   = CBox{overviewPointToGlobal(resizeWorkspaceIdx, RESIZEDBOX.pos()), RESIZEDBOX.size() * (1.F / SCALEFACTOR)};

        TARGET->damageEntire();
        TARGET->rememberFloatingSize(GLOBALBOX.size());
        TARGET->setPositionGlobal(GLOBALBOX);
        TARGET->warpPositionSize();
        TARGET->damageEntire();
    }

    resizePointerDown = false;
    resizePendingWindow.reset();
    resizeActiveWindow.reset();
    resizeOriginalBox  = CBox{};
    resizeLastMouseLocal = Vector2D{};
    resizeCorner       = Layout::CORNER_NONE;
    rebuildPending     = true;
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

    if (!closeOnWindow)
        closeOnWindow = windowClosestToWorkspaceCenter(viewportCurrentWorkspace);

    if (pMonitor && pMonitor->m_activeWorkspace != TARGETWORKSPACEIMAGE->pWorkspace)
        pMonitor->changeWorkspace(TARGETWORKSPACEIMAGE->pWorkspace, false, true, true);

    damage();
}

bool CScrollOverview::scrollStepAllowed(uint32_t timeMs) {
    const uint32_t DELAY = sc<uint32_t>(ScrollOverview::Config::getScrollEventDelay());

    // throttle discrete scroll steps so a single notch / a burst of high-res events only steps once
    if (lastScrollStepTimeMs != 0 && timeMs >= lastScrollStepTimeMs && timeMs - lastScrollStepTimeMs < DELAY)
        return false;

    lastScrollStepTimeMs = timeMs;
    return true;
}

void CScrollOverview::trackpadSwipeLayout(const PHLWORKSPACE target, const double delta) {
    const float SCALE = std::max<float>(scale->value(), 0.01F);
    const auto ALGO = overviewScrollingAlgorithmForWorkspace(target);

    if (!ALGO) {
        return;
    }

    // fingers lifted — snap to the nearest column
    if (delta == 0.0) {
        if (trackpadTapeFollowing) {
            trackpadTapeFollowing = false;
            ALGO->snapToGrid();
            focusMostVisibleScrollingWindow(target);
            damage();
        }
        return;
    }

    trackpadTapeFollowing = true;
    ALGO->moveTape(sc<float>(-1 * delta * ScrollOverview::Config::getTouchpadScrollFactor() / SCALE));
    damage();
}

void CScrollOverview::trackpadSwipeWorkspace(const double delta) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    // fingers lifted — snap to the nearest workspace
    if (delta == 0.0) {
        finishWorkspaceScrollFollow();
        return;
    }

    const float SCALE = std::max<float>(scale->value(), 0.01F);

    trackpadWorkspaceFollowing  = true;
    trackpadScrollAccum         += delta * ScrollOverview::Config::getTouchpadScrollFactor();

    viewOffset->setValueAndWarp(axisOffsetVector(sc<float>(trackpadWorkspaceScrollOffset(MONITOR, SCALE)), layout));
    damage();
}

double CScrollOverview::trackpadWorkspaceScrollOffset(PHLMONITOR monitor, float renderScale) {
    const float RENDEREDLOGICALUNIT = renderScale * std::max<float>(monitor->m_scale, 0.01F);
    const float LOGICALPITCH        = getWorkspaceLogicalPitch(monitor, renderScale, layout);

    double offset    = trackpadScrollAccum / RENDEREDLOGICALUNIT;
    double minOffset = 0.0;
    double maxOffset = 0.0;

    for (size_t i = 0; i < images.size(); ++i) {
        if (!images[i] || !images[i]->pWorkspace)
            continue;

        const double WORKSPACEOFFSET = workspaceOverviewLogicalOffset(i, viewportCurrentWorkspace, LOGICALPITCH);
        minOffset                    = std::min(minOffset, WORKSPACEOFFSET);
        maxOffset                    = std::max(maxOffset, WORKSPACEOFFSET);
    }

    offset              = std::clamp(offset, minOffset, maxOffset);
    trackpadScrollAccum = offset * RENDEREDLOGICALUNIT;

    return offset;
}

void CScrollOverview::finishWorkspaceScrollFollow() {
    if (!trackpadWorkspaceFollowing)
        return;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR) {
        trackpadWorkspaceFollowing = false;
        trackpadScrollAccum        = 0.0;
        return;
    }

    trackpadWorkspaceFollowing  = false;
    trackpadScrollAccum         = 0.0;

    const float  SCALE          = std::max<float>(scale->value(), 0.01F);
    const float  LOGICALPITCH   = getWorkspaceLogicalPitch(MONITOR, SCALE, layout);
    const float  RENDEREDPITCH  = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);
    const size_t ACTIVEIDX      = activeWorkspaceIndex();
    const double VIEWPORTCENTER = axisSize(MONITOR->m_size * MONITOR->m_scale, layout) / 2.0;

    size_t targetIdx    = viewportCurrentWorkspace;
    double bestDistance = std::numeric_limits<double>::max();

    for (size_t i = 0; i < images.size(); ++i) {
        if (!images[i] || !images[i]->pWorkspace)
            continue;

        const auto   WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), workspaceOverviewOffset(i, ACTIVEIDX, RENDEREDPITCH), layout);
        const double DISTANCE     = std::abs(axisValue(WORKSPACEBOX.middle(), layout) - VIEWPORTCENTER);
        if (DISTANCE < bestDistance) {
            bestDistance = DISTANCE;
            targetIdx    = i;
        }
    }

    if (targetIdx == viewportCurrentWorkspace) {
        *viewOffset = Vector2D{};
        return;
    }

    const double OFFSET       = axisValue(viewOffset->value(), layout);
    const double TARGETOFFSET = workspaceOverviewLogicalOffset(targetIdx, viewportCurrentWorkspace, LOGICALPITCH);

    trackpadGestureSettleOffset  = OFFSET - TARGETOFFSET;
    trackpadGestureSettlePending = true;

    const size_t BEFORE = viewportCurrentWorkspace;
    const bool   NEXT   = targetIdx > viewportCurrentWorkspace;
    const size_t STEPS  = sc<size_t>(std::abs(sc<long>(targetIdx) - sc<long>(viewportCurrentWorkspace)));
    for (size_t i = 0; i < STEPS; ++i)
        moveViewportWorkspace(NEXT);

    if (viewportCurrentWorkspace == BEFORE) {
        trackpadGestureSettlePending = false;
        *viewOffset                  = Vector2D{};
    }
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

    if (const auto window = windowClosestToWorkspaceCenter(viewportCurrentWorkspace)) {
        closeOnWindow = window;
        rememberSelection(window);
        syncFocusedSelection();
        return;
    }

    closeOnWindow.reset();
    if (activeScrollOverview().get() == this)
        Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
}

void CScrollOverview::syncFocusedSelection() {
    const auto window = getOverviewWindowToShow(closeOnWindow.lock());
    if (!shouldShowOverviewWindow(window))
        return;

    closeOnWindow = window;

    if (activeScrollOverview().get() != this)
        return;

    if (Desktop::focusState()->window() == window && window->m_workspace == pMonitor->m_activeWorkspace)
        return;

    const auto PREVIOUSWORKSPACE = pMonitor ? pMonitor->m_activeWorkspace : PHLWORKSPACE{};

    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_KEYBIND);

    if (window->m_workspace != PREVIOUSWORKSPACE && focusSyncedFromWorkspaceID == WORKSPACE_INVALID)
        focusSyncedFromWorkspaceID = PREVIOUSWORKSPACE ? PREVIOUSWORKSPACE->m_id : WORKSPACE_INVALID;
}

size_t CScrollOverview::dragWorkspaceIndex(PHLWINDOW window) const {
    if (viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] && images[viewportCurrentWorkspace]->pWorkspace)
        return viewportCurrentWorkspace;

    if (!window)
        return images.size();

    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace == window->m_workspace)
            return i;
    }

    return images.size();
}

bool CScrollOverview::moveSelection(const std::string& direction) {
    const bool MOVINGLEFT  = direction == "left";
    const bool MOVINGRIGHT = direction == "right";
    const bool MOVINGUP    = direction == "up";
    const bool MOVINGDOWN  = direction == "down";

    if (!MOVINGLEFT && !MOVINGRIGHT && !MOVINGUP && !MOVINGDOWN)
        return false;

    bool shouldMoveWorkspace = images.empty() || viewportCurrentWorkspace >= images.size();

    const auto WORKSPACEIMAGE = shouldMoveWorkspace ? SP<SWorkspaceImage>{} : images[viewportCurrentWorkspace];
    if (!WORKSPACEIMAGE || !WORKSPACEIMAGE->pWorkspace)
        shouldMoveWorkspace = true;

    if (!shouldMoveWorkspace && (!closeOnWindow || closeOnWindow->m_workspace != WORKSPACEIMAGE->pWorkspace || !shouldShowOverviewWindow(closeOnWindow.lock()) || closeOnWindow->m_isFloating)) {
        syncSelectionToViewport();
        if (!closeOnWindow || closeOnWindow->m_workspace != WORKSPACEIMAGE->pWorkspace || !shouldShowOverviewWindow(closeOnWindow.lock()) || closeOnWindow->m_isFloating)
            shouldMoveWorkspace = true;
    }

    const auto CURRENT = getOverviewWindowToShow(closeOnWindow.lock());
    if (!CURRENT)
        shouldMoveWorkspace = true;

    if (!shouldMoveWorkspace)
        closeOnWindow = CURRENT;

    if (!shouldMoveWorkspace) {
        const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACEIMAGE->pWorkspace);
        if (ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller) {
            const bool PRIMARYHORIZONTAL = ALGO->m_scrollingData->controller->isPrimaryHorizontal();
            const bool MOVINGPRIMARY     = PRIMARYHORIZONTAL ? MOVINGLEFT || MOVINGRIGHT : MOVINGUP || MOVINGDOWN;
            const bool MOVINGSTACK       = PRIMARYHORIZONTAL ? MOVINGUP || MOVINGDOWN : MOVINGLEFT || MOVINGRIGHT;
            const bool NEXT              = MOVINGRIGHT || MOVINGDOWN;

            if (MOVINGPRIMARY || MOVINGSTACK) {
                if (MOVINGPRIMARY ? moveScrollingColumnSelection(NEXT) : moveScrollingStackSelection(NEXT))
                    return true;

                shouldMoveWorkspace = true;
            }
        }
    }

    const auto CURRENTCENTER = shouldMoveWorkspace ? Vector2D{} : CURRENT->middle();

    PHLWINDOW bestCandidate;
    float     bestPrimaryDistance   = std::numeric_limits<float>::max();
    float     bestSecondaryDistance = std::numeric_limits<float>::max();
    float     bestOverlap           = -1.F;
    bool      bestHasOverlap         = false;

    for (const auto& windowRef : shouldMoveWorkspace ? std::vector<PHLWINDOWREF>{} : WORKSPACEIMAGE->windows) {
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

        const float OVERLAP = MOVINGLEFT || MOVINGRIGHT ? getWindowVerticalOverlap(CURRENT, WINDOW) : getWindowHorizontalOverlap(CURRENT, WINDOW);
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

    const auto WINDOWSELECTIONMOVED = bestCandidate;
    if (!WINDOWSELECTIONMOVED)
        shouldMoveWorkspace = true;

    if (shouldMoveWorkspace) {
        if (((MOVINGLEFT || MOVINGRIGHT) && layout != ScrollOverview::Config::ELayout::HORIZONTAL) || ((MOVINGUP || MOVINGDOWN) && layout == ScrollOverview::Config::ELayout::HORIZONTAL))
            return false;

        moveViewportWorkspace(MOVINGRIGHT || MOVINGDOWN);
        return true;
    }

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

    constexpr auto FULLSCREENALPHA = Desktop::View::WINDOW_ALPHA_FULLSCREEN;

    for (auto& entry : forcedWindowVisibility) {
        if (entry.window == window) {
            window->m_hidden = false;
            window->alpha(FULLSCREENALPHA)->setValueAndWarp(1.F);
            return;
        }
    }

    auto& entry                  = forcedWindowVisibility.emplace_back();
    entry.window                 = window;
    entry.hidden                 = window->m_hidden;

    window->m_hidden = false;
    window->alpha(FULLSCREENALPHA)->setValueAndWarp(1.F);
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

			auto& lsAlpha = ls->alpha()[Desktop::View::LS_ALPHA_FADE];
            if (!known)
                forcedLayerVisibility.push_back({ls, ls->m_aboveFullscreen, lsAlpha->value()});

            if (!ls->m_aboveFullscreen)
                ls->m_aboveFullscreen = true;

            if (lsAlpha->value() != 1.F || lsAlpha->goal() != 1.F || lsAlpha->isBeingAnimated())
                lsAlpha->setValueAndWarp(1.F);
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

        constexpr auto FULLSCREENALPHA = Desktop::View::WINDOW_ALPHA_FULLSCREEN;
        WINDOW->updateFullscreenInputState();
        *WINDOW->alpha(FULLSCREENALPHA) = WINDOW->isBlockedByFullscreen() ? 0.F : 1.F;

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

		auto& entryLsAlpha = entry.layer->alpha()[Desktop::View::LS_ALPHA_FADE];
        const auto MONITOR = entry.layer->m_monitor.lock();
        if (!MONITOR) {
            entryLsAlpha->setValueAndWarp(entry.alpha);
            continue;
        }

        const bool fullscreen = Fullscreen::controller()->hasFullscreen(MONITOR);
        const bool visible    = !fullscreen || entry.layer->m_layer >= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY || entry.layer->m_aboveFullscreen;
        entryLsAlpha->setValueAndWarp(visible ? 1.F : 0.F);
    }

    forcedLayerVisibility.clear();
}

void CScrollOverview::applyWorkspaceAnimationOverrides() {
    if (!sharedStateOwner || workspaceAnimationsOverridden)
        return;

    savedWorkspaceAnimationConfigs.clear();

    for (const std::string name : {"workspaces", "workspacesIn", "workspacesOut"}) {
        const auto CONFIG = Config::animationTree()->getAnimationPropertyConfig(name);
        if (!CONFIG)
            continue;

        auto& saved   = savedWorkspaceAnimationConfigs.emplace_back();
        saved.name    = name;
        saved.config  = *CONFIG;
    }

    for (const auto& saved : savedWorkspaceAnimationConfigs)
        Config::animationTree()->setConfigForNode(saved.name, false, 1.F, "default", "");

    workspaceAnimationsOverridden = true;
}

void CScrollOverview::restoreWorkspaceAnimationOverrides() {
    if (!workspaceAnimationsOverridden)
        return;

    const auto propagateAnimationValues = [](const SP<Hyprutils::Animation::SAnimationPropertyConfig>& parent, auto&& self) -> void {
        if (!parent)
            return;

        for (const auto& [name, animation] : Config::animationTree()->getAnimationConfig()) {
            if (!animation || animation->overridden || animation->pParentAnimation != parent)
                continue;

            animation->pValues = parent->pValues;
            self(animation, self);
        }
    };

    for (const auto& saved : savedWorkspaceAnimationConfigs) {
        const auto CONFIG = Config::animationTree()->getAnimationPropertyConfig(saved.name);
        if (!CONFIG)
            continue;

        *CONFIG = saved.config;
        propagateAnimationValues(CONFIG, propagateAnimationValues);
    }

    savedWorkspaceAnimationConfigs.clear();
    workspaceAnimationsOverridden = false;
}

void CScrollOverview::forceWorkspaceAlphaVisible() {
    for (const auto& workspace : State::workspaceState()->workspaces()) {
        if (!workspace || !workspace->m_alpha)
            continue;

        workspace->m_alpha->setValueAndWarp(1.F);
        *workspace->m_alpha = 1.F;
    }
}

void CScrollOverview::forceWorkspaceWindowsDecoRecalc(const PHLWORKSPACE& workspace) {
    if (!workspace)
        return;

    const auto workspaceImage = std::ranges::find_if(images, [&workspace](const auto& image) { return image && image->pWorkspace == workspace; });
    if (workspaceImage == images.end())
        return;

    for (const auto& windowRef : (*workspaceImage)->windows)
        OverviewWindow::forceDecoRecalc(windowRef.lock());
}

void CScrollOverview::applyInputConfigOverrides() {
    if (!sharedStateOwner || inputConfigOverridden)
        return;

    previousNoWarps                    = ScrollOverview::Config::getValue<int>("cursor:no_warps");
    previousWarpOnChangeWorkspace      = ScrollOverview::Config::getValue<int>("cursor:warp_on_change_workspace");
    previousWarpOnToggleSpecial        = ScrollOverview::Config::getValue<int>("cursor:warp_on_toggle_special");
    previousWarpBackAfterNonMouseInput = ScrollOverview::Config::getValue<int>("cursor:warp_back_after_non_mouse_input");
    previousFollowMouse                = ScrollOverview::Config::getValue<int>("input:follow_mouse");
    inputConfigOverridden = true;

    ScrollOverview::Config::setValue("cursor:no_warps", 1);
    ScrollOverview::Config::setValue("cursor:warp_on_change_workspace", 0);
    ScrollOverview::Config::setValue("cursor:warp_on_toggle_special", 0);
    ScrollOverview::Config::setValue("cursor:warp_back_after_non_mouse_input", 0);
    ScrollOverview::Config::setValue("input:follow_mouse", 0);
}

void CScrollOverview::restoreInputConfigOverrides() {
    if (!inputConfigOverridden)
        return;

    ScrollOverview::Config::setValue("cursor:no_warps", previousNoWarps);
    ScrollOverview::Config::setValue("cursor:warp_on_change_workspace", previousWarpOnChangeWorkspace);
    ScrollOverview::Config::setValue("cursor:warp_on_toggle_special", previousWarpOnToggleSpecial);
    ScrollOverview::Config::setValue("cursor:warp_back_after_non_mouse_input", previousWarpBackAfterNonMouseInput);
    ScrollOverview::Config::setValue("input:follow_mouse", previousFollowMouse);

    inputConfigOverridden = false;
}

void CScrollOverview::transferSharedStateOwnership() {
    if (!sharedStateOwner)
        return;

    CScrollOverview* successor = nullptr;
    for (const auto& overview : scrollOverviews()) {
        auto* candidate = overview ? dynamic_cast<CScrollOverview*>(overview.get()) : nullptr;
        if (candidate && candidate != this && !candidate->closing) {
            successor = candidate;
            break;
        }
    }

    if (!successor)
        return;

    successor->sharedStateOwner = true;

    successor->savedWorkspaceAnimationConfigs = std::move(savedWorkspaceAnimationConfigs);
    successor->workspaceAnimationsOverridden  = workspaceAnimationsOverridden;
    savedWorkspaceAnimationConfigs.clear();
    workspaceAnimationsOverridden = false;

    successor->previousNoWarps                    = previousNoWarps;
    successor->previousWarpOnChangeWorkspace      = previousWarpOnChangeWorkspace;
    successor->previousWarpOnToggleSpecial        = previousWarpOnToggleSpecial;
    successor->previousWarpBackAfterNonMouseInput = previousWarpBackAfterNonMouseInput;
    successor->previousFollowMouse                = previousFollowMouse;
    successor->inputConfigOverridden              = inputConfigOverridden;
    inputConfigOverridden = false;

    successor->usesSubmapKeybinds   = usesSubmapKeybinds;
    successor->submapActive         = submapActive;
    successor->previousSubmapName   = std::move(previousSubmapName);
    if (successor->usesSubmapKeybinds)
        successor->keyboardKeyHook.reset();
    usesSubmapKeybinds = false;
    submapActive       = false;
    sharedStateOwner   = false;
}

void CScrollOverview::emitFullscreenVisibilityState(PHLWINDOW window, bool hideFullscreen) {
    if (emittingFullscreenVisibilityState)
        return;

    emittingFullscreenVisibilityState = true;
    auto resetEmittingFullscreenVisibilityState = Hyprutils::Utils::CScopeGuard([this] { emittingFullscreenVisibilityState = false; });

    window = getOverviewWindowToShow(window);

    if (!validMapped(window) || !window->m_workspace || window->m_monitor != pMonitor) {
        if (g_pEventManager)
            g_pEventManager->postEvent(SHyprIPCEvent{.event = "fullscreen", .data = "0"});
        return;
    }

    if (!hideFullscreen || !Fullscreen::controller()->isFullscreen(window)) {
        Event::bus()->m_events.window.fullscreen.emit(window);

        if (g_pEventManager)
            g_pEventManager->postEvent(SHyprIPCEvent{.event = "fullscreen", .data = Fullscreen::controller()->isFullscreen(window) ? "1" : "0"});

        return;
    }

    const auto SAVEDMODES = Fullscreen::controller()->getFullscreenModes(window);

    Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);

    Event::bus()->m_events.window.fullscreen.emit(window);

    if (g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{.event = "fullscreen", .data = "0"});

    Fullscreen::controller()->setFullscreenMode(window, SAVEDMODES.internal, SAVEDMODES.client);
}

static PHLWINDOW getOverviewFullscreenVisibilityWindow(const PHLWORKSPACE& workspace, const PHLWINDOW& fallback) {
    const auto FULLSCREENWINDOW = workspace ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace)) : PHLWINDOW{};

    if (shouldShowOverviewWindow(FULLSCREENWINDOW) && FULLSCREENWINDOW->m_workspace == workspace)
        return FULLSCREENWINDOW;

    return getOverviewWindowToShow(fallback);
}

void CScrollOverview::renderWorkspaceBackground(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode,
                                                const Time::steady_tp& now) {
    const auto& workspaceImage = images[workspaceIdx];
    if (!workspaceImage || !workspaceImage->pWorkspace)
        return;

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeIdx, workspacePitch);
    const auto WORKSPACEBOX     = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto WORKSPACEALPHA   = workspaceOverviewAlpha(workspaceIdx);

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

    renderOverviewWorkspaceShadow(monitor, WORKSPACEBOX, renderScale, wallpaperMode == 0, WORKSPACEALPHA);

    if (ScrollOverview::Config::getBlur() && wallpaperMode != 1 && WORKSPACEALPHA > 0.001F)
        OverviewRender::queueBlur(WORKSPACEBOX, 0, 2.F, WORKSPACEALPHA, false);

    if (wallpaperMode != 0 && WORKSPACEALPHA > 0.001F)
        renderWallpaperLayers(monitor, WORKSPACEBOX, renderScale, now, WORKSPACEALPHA);

    if (WORKSPACEALPHA >= 0.999F)
        renderOverviewLayerLevel(monitor, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, WORKSPACEBOX, renderScale, now);
}

void CScrollOverview::renderWorkspaceLive(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now) {
    const auto& workspaceImage = images[workspaceIdx];
    if (!workspaceImage || !workspaceImage->pWorkspace)
        return;

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeIdx, workspacePitch);
    const auto WORKSPACEBOX     = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto VISIBLEBOX       = workspaceOverviewVisibleBox(workspaceIdx, WORKSPACEBOX, renderScale, monitor);

    if (!overviewBoxIntersectsMonitor(VISIBLEBOX, monitor))
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

        const auto windowBox = getOverviewWindowBox(window, monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
        if (!overviewBoxIntersectsMonitor(windowBox, monitor))
            return;

        renderWindowLive(monitor, window, windowBox, renderScale, now, &WORKSPACEBOX);
    };

    const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace));
    const bool scrollingLayout   = isWorkspaceScrolling(workspace);
    const bool hasFullscreenPath = shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace;
    const auto renderDropIndicator = [&] {
        if (hasRunningWorkspaceAnimation())
            return;

        auto*      dragContext = g_pointerGrabOverview && g_pointerGrabOverview->dragActiveWindow ? g_pointerGrabOverview : this;
        const auto DRAGGED     = getOverviewWindowToShow(dragContext->dragActiveWindow.lock());
        if (!DRAGGED)
            return;

        if (!isOverviewPointerOnMonitor(monitor))
            return;

        size_t     dropWorkspaceIdx = 0;
        const auto DROPWORKSPACE    = workspaceAtOverviewDropPoint(lastMousePosLocal, &dropWorkspaceIdx, DRAGGED);
        if (DROPWORKSPACE != workspace || dropWorkspaceIdx != workspaceIdx)
            return;

        const auto ANCHOR      = dropAnchorAtOverviewCursorOnWorkspace(workspaceIdx, DRAGGED, dragContext);
        const auto WORKSPACEBOX = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);

        CDropIndicator::renderDropIndicator({
            .monitor               = monitor,
            .workspace             = workspace,
            .workspaceUsableBox    = getOverviewWorkspaceUsableBox(workspace, monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout),
            .anchor                = ANCHOR,
            .renderScale           = renderScale,
            .workspaceFullyVisible = overviewBoxFullyVisibleOnMonitor(WORKSPACEBOX, monitor),
            .floating              = DRAGGED->m_isFloating,
            .layout                = layout,
        });
    };

    if (!scrollingLayout && hasFullscreenPath) {
        renderOverviewWindow(fullscreenWindow);
        OverviewRender::flushPass(monitor);
        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!shouldShowOverviewWindow(window) || !window->m_isFloating || window == fullscreenWindow)
                continue;

            renderOverviewWindow(window);
        }
        renderDropIndicator();
        return;
    }

    const auto renderWindowsByState = [&](bool floating) {
        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!window || window->m_isFloating != floating)
                continue;

            renderOverviewWindow(window);
        }
    };

    renderWindowsByState(false);
    renderWindowsByState(true);
    renderDropIndicator();
}

void CScrollOverview::renderDraggedWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale, const Time::steady_tp& now) {
    auto*      dragOwner = g_pointerGrabOverview && g_pointerGrabOverview->dragActiveWindow ? g_pointerGrabOverview : this;
    const auto WINDOW    = getOverviewWindowToShow(dragOwner->dragActiveWindow.lock());
    if (!shouldShowOverviewWindow(WINDOW) || !WINDOW->m_workspace)
        return;

    const auto GLOBALBOX  = dragOwner->draggedWindowGlobalBox();
    const auto MONITORBOX = monitor ? monitor->logicalBox() : CBox{};
    if (GLOBALBOX.empty() || MONITORBOX.empty() || GLOBALBOX.intersection(MONITORBOX).empty())
        return;

    const auto windowBox = CBox{
        (GLOBALBOX.pos() - monitor->m_position) * monitor->m_scale,
        GLOBALBOX.size() * monitor->m_scale,
    };

    renderWindowLive(monitor, WINDOW, windowBox, dragOwner->scale->value(), now, nullptr, true);
}

bool CScrollOverview::hasVisiblePrecomputedBlurWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale) const {
    if (!monitor)
        return false;

    const auto DRAGGEDWINDOW = getOverviewWindowToShow(dragActiveWindow.lock());

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || !workspaceImage->pWorkspace)
            continue;

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeIdx, workspacePitch);
        const auto WORKSPACEBOX     = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
        const auto VISIBLEBOX       = workspaceOverviewVisibleBox(workspaceIdx, WORKSPACEBOX, renderScale, monitor);
        if (!overviewBoxIntersectsMonitor(VISIBLEBOX, monitor))
            continue;

        const auto workspace = workspaceImage->pWorkspace;

        const auto isVisiblePrecomputedBlurWindow = [&](const PHLWINDOW& window) {
            if (window == DRAGGEDWINDOW || !OverviewWindow::shouldUseBlurFramebuffer(window))
                return false;

            const auto windowBox = getOverviewWindowBox(window, monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
            return overviewBoxIntersectsMonitor(windowBox, monitor);
        };

        if (!isWorkspaceScrolling(workspace)) {
            const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace));
            if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace) {
                if (isVisiblePrecomputedBlurWindow(fullscreenWindow))
                    return true;

                continue;
            }
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

    const auto TARGETOVERVIEWSCALE = ScrollOverview::Config::getScale();
    const auto ANIMATIONPROGRESS   = (1.F - TARGETOVERVIEWSCALE) > 0.001F ? (1.F - overviewScale) / (1.F - TARGETOVERVIEWSCALE) : 1.F;

    for (const auto& windowRef : pinnedFloatingWindows) {
        const auto window = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowPinnedFloatingOverviewWindow(window))
            continue;
        if (dragActiveWindow && window == getOverviewWindowToShow(dragActiveWindow.lock()))
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
                                       bool dragged) {
    if (!window)
        return;

    auto* const DRAGOWNER = g_pointerGrabOverview && g_pointerGrabOverview->dragActiveWindow ? g_pointerGrabOverview : this;
    const auto  DRAGGED   = getOverviewWindowToShow(DRAGOWNER->dragActiveWindow.lock());
    auto        PSEUDOFOCUSED = PHLWINDOW{};
    if (now < g_pseudoFocusUntil)
        PSEUDOFOCUSED = getOverviewWindowToShow(g_pseudoFocusedWindow.lock());
    else {
        g_pseudoFocusedWindow.reset();
        g_pseudoFocusUntil = {};
    }

    if (!shouldShowOverviewWindow(PSEUDOFOCUSED))
        PSEUDOFOCUSED.reset();

    const auto PSEUDOFOCUSWINDOW = DRAGGED ? DRAGGED : PSEUDOFOCUSED;

    forceWindowVisible(window);
    forceWindowSurfaceVisibility(window);

    OverviewWindow::renderOverviewWindow({
        .monitor              = monitor,
        .window               = window,
        .windowBox            = windowBox,
        .renderScale          = renderScale,
        .now                  = now,
        .workspaceBox         = workspaceBox,
        .selected             = closeOnWindow == window,
        .dragged              = dragged,
        .pseudoFocusWindow    = PSEUDOFOCUSWINDOW,
    });
}

void CScrollOverview::redrawAll(bool forcelowres) {
    rebuildWorkspaceImages();
    seedRememberedSelections();

    for (const auto& img : images) {
        img->windows.clear();
        img->overflowLeft   = 0.F;
        img->overflowRight  = 0.F;
        img->overflowTop    = 0.F;
        img->overflowBottom = 0.F;
    }
    pinnedFloatingWindows.clear();

    std::unordered_map<WORKSPACEID, SP<SWorkspaceImage>> imagesByWorkspace;
    imagesByWorkspace.reserve(images.size());

    for (const auto& img : images) {
        if (img && img->pWorkspace)
            imagesByWorkspace.emplace(img->pWorkspace->m_id, img);
    }

    std::vector<PHLWINDOW> addedWindows;
    addedWindows.reserve(Desktop::windowState()->windows().size());

    std::vector<PHLWINDOW> addedPinnedFloatingWindows;
    addedPinnedFloatingWindows.reserve(Desktop::windowState()->windows().size());

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

    for (const auto& window : Desktop::windowState()->windows()) {
        if (getOverviewWindowToShow(window) != window)
            continue;

        addOverviewWindow(window);
        addPinnedFloatingWindow(window);
    }

    for (const auto& window : Desktop::windowState()->windows()) {
        if (getOverviewWindowToShow(window) == window)
            continue;

        addOverviewWindow(window);
        addPinnedFloatingWindow(window);
    }

    updateWorkspaceOverflow();
}

void CScrollOverview::damage() {
    blockDamageReporting = true;
    g_pHyprRenderer->damageMonitor(pMonitor.lock());
    blockDamageReporting = false;
}

void CScrollOverview::requestInputFrame() {
    if (closing)
        return;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    inputFramePending = true;
    MONITOR->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_CURSOR_MOVE);
}

void CScrollOverview::markBlurDirty() {
    overviewBlurDirty = true;
}

void CScrollOverview::markBackdropBlurDirty() {
    backdropBlurDirty = true;
}

void CScrollOverview::onDamageReported() {
    return;
}

bool CScrollOverview::isVisibleRealtimePreviewWindow(const PHLWINDOW& window) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !window || !Fullscreen::controller()->isFullscreen(window) || window->m_monitor != MONITOR)
        return false;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || !workspaceImage->pWorkspace || workspaceImage->pWorkspace != window->m_workspace)
            continue;

        const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspaceImage->pWorkspace));
        if (fullscreenWindow != window)
            return false;

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);
        const auto WINDOWBOX        = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
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
    if (inputFramePending) {
        inputFramePending = false;
        return true;
    }

    if (selectedWorkspaceFramePending) {
        selectedWorkspaceFramePending = false;
        return true;
    }

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

bool CScrollOverview::hasRunningWorkspaceAnimation() const {
    return viewOffset->isBeingAnimated() || workspaceInsertProgress->isBeingAnimated() || workspaceInsertFadeProgress->isBeingAnimated();
}

bool CScrollOverview::shouldSuppressRenderDamage() const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing)
        return false;

    if (scale->isBeingAnimated() || viewOffset->isBeingAnimated())
        return false;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);
    const auto DRAGGED   = getOverviewWindowToShow(dragActiveWindow.lock());

    const auto isVisibleAnimatedWindow = [&](const PHLWINDOW& window, float workspaceOffset) {
        if (!shouldShowOverviewWindow(window) || window == DRAGGED)
            return false;

        const auto WINDOWBOX = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), workspaceOffset, layout);
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

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);
        const auto WORKSPACEBOX     = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        const auto VISIBLEBOX       = workspaceOverviewVisibleBox(workspaceIdx, WORKSPACEBOX, SCALE, MONITOR);
        if (!overviewBoxIntersectsMonitor(VISIBLEBOX, MONITOR))
            continue;

        const auto workspace = workspaceImage->pWorkspace;
        if (!isWorkspaceScrolling(workspace)) {
            const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace));
            if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace) {
                if (isVisibleAnimatedWindow(fullscreenWindow, WORKSPACEOFFSET))
                    return false;

                for (const auto& windowRef : workspaceImage->windows) {
                    const auto window = getOverviewWindowToShow(windowRef.lock());
                    if (window && window->m_isFloating && isVisibleAnimatedWindow(window, WORKSPACEOFFSET))
                        return false;
                }

                continue;
            }
        }

        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (isVisibleAnimatedWindow(window, WORKSPACEOFFSET))
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
    if (!MONITOR)
        return;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);
    const auto DRAGGED   = getOverviewWindowToShow(dragActiveWindow.lock());
    const bool CANFRAMETHROTTLEDWINDOWS = closing || shouldAllowRealtimePreviewFrame();
    bool       sentThrottledWindowFrame = false;

    const bool PREVSENDINGFRAMECALLBACKS = sendingOverviewFrameCallbacks;
    sendingOverviewFrameCallbacks        = CANFRAMETHROTTLEDWINDOWS;
    auto resetSendingFrameCallbacks      = Hyprutils::Utils::CScopeGuard([this, PREVSENDINGFRAMECALLBACKS] { sendingOverviewFrameCallbacks = PREVSENDINGFRAMECALLBACKS; });

    const auto frameWindow = [&](const PHLWINDOW& window, float workspaceOffset, bool realtime) {
        if (!shouldShowOverviewWindow(window))
            return;

        const bool ISDRAGGED = window == DRAGGED;
        if (!ISDRAGGED) {
            const auto WINDOWBOX = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), workspaceOffset, layout);
            if (!overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR))
                return;
        }

        if (!realtime && !ISDRAGGED && !CANFRAMETHROTTLEDWINDOWS) {
            scheduleRealtimePreviewFrame();
            return;
        }

        surfaceTreePresent(window->wlSurface() ? window->wlSurface()->resource() : nullptr, MONITOR, now);
        if (!realtime && !ISDRAGGED)
            sentThrottledWindowFrame = true;
    };

    for (const auto& windowRef : pinnedFloatingWindows) {
        const auto window = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowPinnedFloatingOverviewWindow(window) || window->m_monitor != MONITOR)
            continue;

        if (!CANFRAMETHROTTLEDWINDOWS) {
            scheduleRealtimePreviewFrame();
            continue;
        }

        surfaceTreePresent(window->wlSurface() ? window->wlSurface()->resource() : nullptr, MONITOR, now);
        sentThrottledWindowFrame = true;
    }

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || !workspaceImage->pWorkspace)
            continue;

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);
        const auto WORKSPACEBOX     = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        const auto VISIBLEBOX       = workspaceOverviewVisibleBox(workspaceIdx, WORKSPACEBOX, SCALE, MONITOR);
        if (!overviewBoxIntersectsMonitor(VISIBLEBOX, MONITOR))
            continue;

        const auto workspace = workspaceImage->pWorkspace;
        const bool REALTIME  = isSelectedWorkspace(workspace);
        if (!isWorkspaceScrolling(workspace)) {
            const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace));
            if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace) {
                frameWindow(fullscreenWindow, WORKSPACEOFFSET, REALTIME);
                for (const auto& windowRef : workspaceImage->windows) {
                    const auto window = getOverviewWindowToShow(windowRef.lock());
                    if (window && window->m_isFloating)
                        frameWindow(window, WORKSPACEOFFSET, REALTIME);
                }
                continue;
            }
        }

        for (const auto& windowRef : workspaceImage->windows) {
            frameWindow(getOverviewWindowToShow(windowRef.lock()), WORKSPACEOFFSET, REALTIME);
        }
    }

    if (sentThrottledWindowFrame)
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
    if (g_pointerGrabOverview && g_pointerGrabOverview->dragActiveWindow &&
        window == getOverviewWindowToShow(g_pointerGrabOverview->dragActiveWindow.lock()))
        return true;

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
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || workspaceImage->pWorkspace != window->m_workspace)
            continue;

        if (!isWorkspaceScrolling(workspaceImage->pWorkspace)) {
            const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspaceImage->pWorkspace));
            if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspaceImage->pWorkspace && fullscreenWindow != window && !window->m_isFloating)
                return false;
        }

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);
        const auto WINDOWBOX        = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        if (!overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR))
            return false;

        if (isSelectedWorkspace(workspaceImage->pWorkspace))
            return true;

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
            return true;

        if (layerOwner->m_layer > ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)
            return false;

        markBlurDirty();
        if (layerOwner->m_layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND)
            markBackdropBlurDirty();
        return true;
    }

    if (!windowOwner)
        return true;

    auto window = getOverviewWindowToShow(windowOwner);
    if (g_pointerGrabOverview && g_pointerGrabOverview->dragActiveWindow &&
        window == getOverviewWindowToShow(g_pointerGrabOverview->dragActiveWindow.lock())) {
        const auto DRAGBOX = g_pointerGrabOverview->draggedWindowGlobalBox();
        if (!DRAGBOX.empty() && !DRAGBOX.intersection(MONITOR->logicalBox()).empty())
            damage();
        return true;
    }

    if (shouldShowPinnedFloatingOverviewWindow(window)) {
        if (window->m_monitor != MONITOR)
            return true;

        if (!realtimePreviewFrameQueued && shouldAllowRealtimePreviewFrame())
            return true;

        scheduleRealtimePreviewFrame();
        return false;
    }

    if (window && window->m_monitor != MONITOR)
        return true;

    if (!shouldShowOverviewWindow(window) || !window->m_workspace)
        return false;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || workspaceImage->pWorkspace != window->m_workspace)
            continue;

        if (!isWorkspaceScrolling(workspaceImage->pWorkspace)) {
            const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspaceImage->pWorkspace));
            if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspaceImage->pWorkspace && fullscreenWindow != window && !window->m_isFloating)
                return false;
        }

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);
        const auto WINDOWBOX        = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        if (!overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR))
            return false;

        if (isSelectedWorkspace(workspaceImage->pWorkspace)) {
            selectedWorkspaceFramePending = true;
            return true;
        }

        if (!realtimePreviewFrameQueued && shouldAllowRealtimePreviewFrame())
            return true;

        scheduleRealtimePreviewFrame();
        return false;

    }

    return false;
}

void CScrollOverview::close() {
    close(ECloseMode::COMMIT_SELECTION);
}

void CScrollOverview::dismissTransient() {
    if (closing && !closeRemovalPending) {
        setClosing(false);
        closeApplied = false;
    }

    close(ECloseMode::PRESERVE_MONITOR_STATE);
}

void CScrollOverview::close(ECloseMode mode) {
    if (closeApplied)
        return;
    closeApplied = true;

    const bool PRESERVEMONITORSTATE = mode == ECloseMode::PRESERVE_MONITOR_STATE;
    const bool ACTIVATESELECTION    = !PRESERVEMONITORSTATE && activeScrollOverview().get() == this;
    const auto MONITOR              = pMonitor.lock();
    const auto SELECTEDWORKSPACE    = PRESERVEMONITORSTATE ?
        (MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{}) :
        (viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] ? images[viewportCurrentWorkspace]->pWorkspace : PHLWORKSPACE{});

    if (PRESERVEMONITORSTATE) {
        closeOnWindow.reset();
        focusSyncedFromWorkspaceID = WORKSPACE_INVALID;
        startedOn                  = SELECTEDWORKSPACE;
        viewportCurrentWorkspace   = activeWorkspaceIndex();
        viewOffset->setValueAndWarp(Vector2D{});
    }

    setClosing(true);

    const auto finishClose = [&](const PHLWORKSPACE& finalWorkspace, const PHLWINDOW& finalWindow) {
        emitFullscreenVisibilityState(getOverviewFullscreenVisibilityWindow(finalWorkspace, finalWindow), false);

        *scale = 1.F;

        if (!ScrollOverview::Config::getValue<int>("animations:enabled")) {
            forceWorkspaceWindowsDecoRecalc(finalWorkspace ? finalWorkspace : pMonitor->m_activeWorkspace);
            damage();
        }

        closeRemovalPending = true;
        scale->setCallbackOnEnd([this](auto) {
            if (!dragCancelledAwaitingRelease)
                removeOverview(this);
        });
    };

    if (!PRESERVEMONITORSTATE && !closeOnWindow && (!SELECTEDWORKSPACE || SELECTEDWORKSPACE == pMonitor->m_activeWorkspace)) {
        const auto FOCUSEDWINDOW = getOverviewWindowToShow(Desktop::focusState()->window());
        if (!SELECTEDWORKSPACE || (FOCUSEDWINDOW && FOCUSEDWINDOW->m_workspace == SELECTEDWORKSPACE))
            closeOnWindow = FOCUSEDWINDOW;
    }

    closeOnWindow = getOverviewWindowToShow(closeOnWindow.lock());

    if (closeOnWindow && focusSyncedFromWorkspaceID != WORKSPACE_INVALID) {
        const auto FINALWORKSPACE = closeOnWindow->m_workspace;
        size_t     sourceIdx      = images.size();
        size_t     targetIdx      = images.size();

        for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
            if (!images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
                continue;

            if (images[workspaceIdx]->pWorkspace->m_id == focusSyncedFromWorkspaceID)
                sourceIdx = workspaceIdx;
            if (images[workspaceIdx]->pWorkspace == FINALWORKSPACE)
                targetIdx = workspaceIdx;
        }

        if (sourceIdx < images.size() && targetIdx < images.size()) {
            if (FINALWORKSPACE != pMonitor->m_activeWorkspace)
                pMonitor->changeWorkspace(FINALWORKSPACE, false, true, true);

            if (ACTIVATESELECTION)
                Desktop::focusState()->fullWindowFocus(closeOnWindow.lock(), Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

            startedOn                = FINALWORKSPACE;
            viewportCurrentWorkspace = targetIdx;

            const auto FINALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), 1.F, layout);
            viewOffset->setValueAndWarp(axisOffsetVector(workspaceOverviewLogicalOffset(sourceIdx, targetIdx, FINALPITCH), layout));
            *viewOffset = Vector2D{};

            focusSyncedFromWorkspaceID = WORKSPACE_INVALID;

            const auto FINALWINDOW = getOverviewWindowToShow(closeOnWindow.lock());
            finishClose(FINALWORKSPACE, FINALWINDOW);
            return;
        }
    }

    if (!closeOnWindow) {
        const auto ACTIVEIDX = activeWorkspaceIndex();
        const auto FINALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), 1.F, layout);
        *viewOffset = Vector2D{};

        for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
            if (!images[workspaceIdx] || images[workspaceIdx]->pWorkspace != SELECTEDWORKSPACE)
                continue;

            *viewOffset = axisOffsetVector(workspaceOverviewLogicalOffset(workspaceIdx, ACTIVEIDX, FINALPITCH), layout);
            break;
        }

        if (SELECTEDWORKSPACE && SELECTEDWORKSPACE != pMonitor->m_activeWorkspace)
            pMonitor->changeWorkspace(SELECTEDWORKSPACE, false, true, true);
    } else if (closeOnWindow == Desktop::focusState()->window() && closeOnWindow->m_workspace == pMonitor->m_activeWorkspace) {
        if (focusSyncedFromWorkspaceID != WORKSPACE_INVALID) {
            const auto ACTIVEIDX   = activeWorkspaceIndex();
            const auto FINALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), 1.F, layout);

            for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
                if (!images[workspaceIdx] || !images[workspaceIdx]->pWorkspace || images[workspaceIdx]->pWorkspace->m_id != focusSyncedFromWorkspaceID)
                    continue;

                viewOffset->setValueAndWarp(axisOffsetVector(workspaceOverviewLogicalOffset(workspaceIdx, ACTIVEIDX, FINALPITCH), layout));
                break;
            }
        }

        *viewOffset = Vector2D{};
    } else {

        if (closeOnWindow->m_workspace != pMonitor->m_activeWorkspace)
            pMonitor->changeWorkspace(closeOnWindow->m_workspace, false, true, true);

        if (ACTIVATESELECTION)
            Desktop::focusState()->fullWindowFocus(closeOnWindow.lock(), Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

        const auto ACTIVEIDX = activeWorkspaceIndex();
        const auto FINALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), 1.F, layout);
        bool       found      = false;
        const auto selectedWindow = getOverviewWindowToShow(closeOnWindow.lock());
        for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
            const auto& wimg = images[workspaceIdx];
            for (const auto& windowRef : wimg->windows) {
                const auto window = getOverviewWindowToShow(windowRef.lock());
                if (window == selectedWindow && window) {
                    *viewOffset = axisOffsetVector(workspaceOverviewLogicalOffset(workspaceIdx, ACTIVEIDX, FINALPITCH), layout);
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
    }

    focusSyncedFromWorkspaceID = WORKSPACE_INVALID;

    const auto FINALWINDOW    = getOverviewWindowToShow(closeOnWindow.lock());
    const auto FINALWORKSPACE = FINALWINDOW ? FINALWINDOW->m_workspace : SELECTEDWORKSPACE;
    finishClose(FINALWORKSPACE, FINALWINDOW);
}

bool CScrollOverview::isClosing() const {
    return closing;
}

void CScrollOverview::reopen() {
    if (!closing)
        return;

    scale->setCallbackOnEnd({});
    closeApplied        = false;
    closeRemovalPending = false;
    setClosing(false);
    activateSubmapIfConfigured();
    emitFullscreenVisibilityState(Desktop::focusState()->window(), true);
    *scale = ScrollOverview::Config::getScale();
    damage();
}

void CScrollOverview::onPreRender() {
    if (pMonitor)
        pMonitor->m_solitaryClient.reset();

    forceLayersAboveFullscreen();
    updateWorkspaceOverflow();

    if (closing)
        return;

    if (pMonitor && pMonitor->m_activeWorkspace && pMonitor->m_activeWorkspace != startedOn) {
        rebuildPending = false;
        markBlurDirty();
        onWorkspaceChange();
        focusSyncedFromWorkspaceID = WORKSPACE_INVALID;
        emitFullscreenVisibilityState(Desktop::focusState()->window(), true);
        return;
    }

    focusSyncedFromWorkspaceID = WORKSPACE_INVALID;

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
    const auto previousStartedOn = startedOn;

    // consume any pending gesture-driven settle (set by finishWorkspaceScrollFollow)
    const bool   GESTURESETTLE       = trackpadGestureSettlePending;
    const double GESTURESETTLEOFFSET = trackpadGestureSettleOffset;
    trackpadGestureSettlePending     = false;

    std::vector<WORKSPACEID> previousWorkspaceIDs;
    previousWorkspaceIDs.reserve(images.size());
    std::unordered_map<WORKSPACEID, float> previousWorkspaceOffsets;
    const auto PREVIOUSLOGICALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), scale->value(), layout);
    for (size_t i = 0; i < images.size(); ++i) {
        const auto& image = images[i];
        if (!image || !image->pWorkspace)
            continue;

        previousWorkspaceIDs.push_back(image->pWorkspace->m_id);
        previousWorkspaceOffsets.emplace(image->pWorkspace->m_id, workspaceOverviewLogicalOffset(i, previousActiveIdx, PREVIOUSLOGICALPITCH));
    }

    const auto NEWWORKSPACE      = pMonitor->m_activeWorkspace;
    const bool INSERTEDWORKSPACE = std::find(previousWorkspaceIDs.begin(), previousWorkspaceIDs.end(), NEWWORKSPACE->m_id) == previousWorkspaceIDs.end();
    const auto REQUESTEDREMOVEDWORKSPACE = pendingRemovedWorkspace.lock();
    const bool SHOULDREMOVEPREVIOUSWORKSPACE =
        previousStartedOn && previousStartedOn != NEWWORKSPACE && !previousStartedOn->m_isSpecialWorkspace && !previousStartedOn->isPersistent() && previousStartedOn->getWindowCount() == 0;
    const auto REMOVEDWORKSPACE = REQUESTEDREMOVEDWORKSPACE ? REQUESTEDREMOVEDWORKSPACE : SHOULDREMOVEPREVIOUSWORKSPACE ? previousStartedOn : PHLWORKSPACE{};

    pendingRemovedWorkspace = REMOVEDWORKSPACE;

    startedOn = NEWWORKSPACE;
    redrawAll();
    viewportCurrentWorkspace = activeWorkspaceIndex();

    const bool REMOVEDPREVIOUSWORKSPACE =
        REMOVEDWORKSPACE &&
        std::find_if(images.begin(), images.end(), [REMOVEDWORKSPACE](const auto& image) { return image && image->pWorkspace == REMOVEDWORKSPACE; }) == images.end();

    if (INSERTEDWORKSPACE || REMOVEDPREVIOUSWORKSPACE) {
        workspaceInsertTransition.active                 = true;
        workspaceInsertTransition.transitionWorkspaceID  = INSERTEDWORKSPACE ? NEWWORKSPACE->m_id : REMOVEDWORKSPACE->m_id;
        workspaceInsertTransition.transitionFadeIn       = INSERTEDWORKSPACE;
        workspaceInsertFadeProgress->setConfig(INSERTEDWORKSPACE ? workspaceInsertFadeConfig : workspaceRemoveFadeConfig);
        workspaceInsertTransition.oldRelativeOffsets.clear();
        workspaceInsertTransition.newRelativeOffsets.clear();
        workspaceInsertTransition.transitionOldRelativeOffset = 0.F;

        for (size_t i = 0; i < previousWorkspaceIDs.size(); ++i) {
            const auto OFFSET = previousWorkspaceOffsets.contains(previousWorkspaceIDs[i]) ? previousWorkspaceOffsets.at(previousWorkspaceIDs[i]) : 0.F;
            workspaceInsertTransition.oldRelativeOffsets.emplace(previousWorkspaceIDs[i], OFFSET);
            if (REMOVEDPREVIOUSWORKSPACE && previousWorkspaceIDs[i] == REMOVEDWORKSPACE->m_id)
                workspaceInsertTransition.transitionOldRelativeOffset = OFFSET;
        }

        const auto NEWLOGICALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), scale->value(), layout);
        for (size_t i = 0; i < images.size(); ++i) {
            if (!images[i] || !images[i]->pWorkspace)
                continue;

            workspaceInsertTransition.newRelativeOffsets.emplace(images[i]->pWorkspace->m_id, workspaceOverviewLogicalOffset(i, viewportCurrentWorkspace, NEWLOGICALPITCH));
        }

        workspaceInsertProgress->setValueAndWarp(0.F);
        workspaceInsertFadeProgress->setValueAndWarp(0.F);
        *workspaceInsertProgress = 1.F;
        *workspaceInsertFadeProgress = 1.F;
        viewOffset->setValueAndWarp(Vector2D{});
        *viewOffset = Vector2D{};
    } else {
        workspaceInsertTransition.active                 = false;
        workspaceInsertTransition.transitionWorkspaceID  = WORKSPACE_INVALID;
        workspaceInsertTransition.transitionFadeIn       = true;
        workspaceInsertFadeProgress->setConfig(workspaceInsertFadeConfig);
        workspaceInsertTransition.oldRelativeOffsets.clear();
        workspaceInsertTransition.newRelativeOffsets.clear();
        workspaceInsertTransition.transitionOldRelativeOffset = 0.F;
        workspaceInsertProgress->setValueAndWarp(1.F);
        workspaceInsertFadeProgress->setValueAndWarp(1.F);
        if (GESTURESETTLE) // a trackpad follow committed this change: settle from the drag position, not a full pitch
            viewOffset->setValueAndWarp(axisOffsetVector(sc<float>(GESTURESETTLEOFFSET), layout));
        else
            viewOffset->setValueAndWarp(
            axisOffsetVector(workspaceOverviewLogicalOffset(previousActiveIdx, viewportCurrentWorkspace, getWorkspaceLogicalPitch(pMonitor.lock(), scale->value(), layout)), layout));
        *viewOffset = Vector2D{};
    }

    syncSelectionToViewport();
    markBlurDirty();
    damage();
}

void CScrollOverview::render() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    if (g_pointerGrabOverview && g_pointerGrabOverview != this && g_pointerGrabOverview->dragActiveWindow && isOverviewPointerOnMonitor(MONITOR))
        lastMousePosLocal = getOverviewMousePosLocal(MONITOR);

    const bool PREVBLOCKSURFACEFEEDBACK       = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    g_pHyprRenderer->m_bBlockSurfaceFeedback  = true;
    auto restoreSurfaceFeedback               = Hyprutils::Utils::CScopeGuard([PREVBLOCKSURFACEFEEDBACK] { g_pHyprRenderer->m_bBlockSurfaceFeedback = PREVBLOCKSURFACEFEEDBACK; });

    const auto NOW       = Time::steadyNow();
    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);

    const auto VIEWOFFSET = viewOffset->value();
    if (!overviewBlurStateValid || std::abs(lastOverviewBlurScale - SCALE) > 0.001F || lastOverviewBlurViewOffset.distanceSq(VIEWOFFSET) > 0.001F) {
        markBlurDirty();
        overviewBlurStateValid     = true;
        lastOverviewBlurScale      = SCALE;
        lastOverviewBlurViewOffset = VIEWOFFSET;
    }

    const auto WALLPAPERMODE = ScrollOverview::Config::getWallpaperMode();

    if (ScrollOverview::Config::getBlur() && WALLPAPERMODE != 1) {
        updateBackdropBlurCache(MONITOR, WALLPAPERMODE, NOW);
        if (backdropBlurFB && backdropBlurFB->isAllocated() && backdropBlurFB->getTexture())
            renderBackdropBlurCache(MONITOR);
        else
            renderGlobalWallpaper(MONITOR, NOW);
    } else if (WALLPAPERMODE == 0 || WALLPAPERMODE == 2) {
        renderGlobalWallpaper(MONITOR, NOW);
    } else
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 1.F}}, {});

    Event::bus()->m_events.render.stage.emit(RENDER_POST_WALLPAPER);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        renderWorkspaceBackground(MONITOR, workspaceIdx, ACTIVEIDX, PITCH, SCALE, WALLPAPERMODE, NOW);
    }

    if (workspaceInsertTransition.active && !workspaceInsertTransition.transitionFadeIn) {
        const auto GHOSTALPHA   = 1.F - std::clamp(workspaceInsertFadeProgress->value(), 0.F, 1.F);
        const auto GHOSTOFFSET = workspaceInsertTransition.transitionOldRelativeOffset * SCALE * MONITOR->m_scale;
        const auto GHOSTBOX     = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), GHOSTOFFSET, layout);

        if (GHOSTALPHA > 0.001F && overviewBoxIntersectsMonitor(GHOSTBOX, MONITOR)) {
            renderOverviewWorkspaceShadow(MONITOR, GHOSTBOX, SCALE, WALLPAPERMODE == 0, GHOSTALPHA);

            if (WALLPAPERMODE != 0)
                renderWallpaperLayers(MONITOR, GHOSTBOX, SCALE, NOW, GHOSTALPHA);

            renderOverviewLayerLevel(MONITOR, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, GHOSTBOX, SCALE, NOW);
        }
    }

    const bool NEEDS_PRECOMPUTED_BLUR = hasVisiblePrecomputedBlurWindow(MONITOR, ACTIVEIDX, PITCH, SCALE);
    if (NEEDS_PRECOMPUTED_BLUR && overviewBlurDirty)
        g_pHyprRenderer->m_renderPass.add(makeUnique<CPreBlurElement>());

    OverviewRender::flushPass(MONITOR);

    if (NEEDS_PRECOMPUTED_BLUR)
        overviewBlurDirty = false;

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        renderWorkspaceLive(MONITOR, workspaceIdx, ACTIVEIDX, PITCH, SCALE, WALLPAPERMODE, NOW);
    }

    renderDraggedWindow(MONITOR, ACTIVEIDX, PITCH, SCALE, NOW);
    renderPinnedFloatingWindows(MONITOR, SCALE, NOW);

    for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
        for (auto const& ls : MONITOR->m_layerSurfaceLayers[LAYER]) {
            if (!Desktop::View::validMapped(ls.lock()))
                continue;

            g_pHyprRenderer->renderLayer(ls.lock(), MONITOR, NOW);
        }
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
    if (closing) {
        removeFromCrossMonitorDragSession(this);
        cancelWindowDrag();
        transferSharedStateOwnership();
        inputFramePending = false;
        if (scrollingPanPointerDown)
            endScrollingPan();
        releaseTopLayerPointerButtons(Time::millis(Time::steadyNow()));
        restoreSubmapIfActive();
    } else
        applyWorkspaceAnimationOverrides();
}

void CScrollOverview::releaseInputListeners() {
    if (scrollingPanPointerDown)
        endScrollingPan();
    releaseTopLayerPointerButtons(Time::millis(Time::steadyNow()));
    cancelWindowDrag();
    clearDragPending();
    submapMouseClickPending = false;
    submapMouseClickButton  = 0;

    mouseMoveHook.reset();
    touchMoveHook.reset();
    mouseAxisHook.reset();
    mouseButtonHook.reset();
    touchDownHook.reset();
    keyboardKeyHook.reset();
    dragKeyboardKeyHook.reset();
}

void CScrollOverview::activateSubmapIfConfigured() {
    if (!sharedStateOwner || !usesSubmapKeybinds || !g_pKeybindManager)
        return;

    previousSubmapName = g_pKeybindManager->getCurrentSubmap().name;

    const auto DISPATCHER = g_pKeybindManager->m_dispatchers.find("submap");
    if (DISPATCHER == g_pKeybindManager->m_dispatchers.end()) {
        usesSubmapKeybinds = false;
        return;
    }

    const auto RESULT = DISPATCHER->second(OVERVIEW_SUBMAP);
    if (!RESULT.success) {
        usesSubmapKeybinds = false;
        return;
    }

    submapActive = true;
}

void CScrollOverview::restoreSubmapIfActive() {
    if (!submapActive || !g_pKeybindManager)
        return;

    const auto CURRENT = g_pKeybindManager->getCurrentSubmap().name;
    if (CURRENT == OVERVIEW_SUBMAP) {
        const auto DISPATCHER = g_pKeybindManager->m_dispatchers.find("submap");
        if (DISPATCHER != g_pKeybindManager->m_dispatchers.end())
            DISPATCHER->second(previousSubmapName.empty() ? "reset" : previousSubmapName);
    }

    submapActive = false;
}

bool CScrollOverview::dispatchSubmapMouseClick(uint32_t button) {
    if (!usesSubmapKeybinds || !isOverviewSubmapActive() || !g_pKeybindManager || !g_pInputManager)
        return false;

    const auto KEYNAME = "mouse:" + std::to_string(button);
    const auto MODS    = g_pInputManager->getModsFromAllKBs();

    const auto KEYBIND = std::ranges::find_if(g_pKeybindManager->m_keybinds, [&](const auto& keybind) {
        return keybind && keybind->enabled && !keybind->shadowed && keybind->key == KEYNAME && keybind->submap.name == OVERVIEW_SUBMAP &&
            (keybind->modmask == MODS || keybind->ignoreMods);
    });

    if (KEYBIND == g_pKeybindManager->m_keybinds.end())
        return false;

    const auto DISPATCHERNAME = (*KEYBIND)->mouse ? "mouse" : (*KEYBIND)->handler;
    const auto DISPATCHER     = g_pKeybindManager->m_dispatchers.find(DISPATCHERNAME);
    if (DISPATCHER == g_pKeybindManager->m_dispatchers.end())
        return false;

    const auto PREVIOUSKEYBIND = g_pKeybindManager->m_currentKeybind;
    g_pKeybindManager->m_currentKeybind = *KEYBIND;
    auto restoreKeybind = Hyprutils::Utils::CScopeGuard([PREVIOUSKEYBIND] { g_pKeybindManager->m_currentKeybind = PREVIOUSKEYBIND; });

    const int PREVIOUSPASSPRESSED = Config::Actions::state()->m_passPressed;
    Config::Actions::state()->m_passPressed = 0;
    auto restorePassPressed = Hyprutils::Utils::CScopeGuard([PREVIOUSPASSPRESSED] { Config::Actions::state()->m_passPressed = PREVIOUSPASSPRESSED; });

    DISPATCHER->second((*KEYBIND)->mouse ? "0" + (*KEYBIND)->arg : (*KEYBIND)->arg);
    return true;
}

void CScrollOverview::resetSwipe() {
    if (closing) {
        close();
        return;
    }

    (*scale)    = ScrollOverview::Config::getScale();
    m_isSwiping = false;
}

void CScrollOverview::onSwipeUpdate(double delta) {
    const int DISTANCE = ScrollOverview::Config::getGestureDistance();

    m_isSwiping = true;

    const float PERC = closing ? 1.0 - std::clamp(delta / sc<double>(DISTANCE), 0.0, 1.0) : std::clamp(delta / sc<double>(DISTANCE), 0.0, 1.0);

    scale->setValueAndWarp(hyprlerp(1.F, ScrollOverview::Config::getScale(), PERC));
}

void CScrollOverview::onSwipeEnd() {
    if (closing) {
        close();
        return;
    }

    (*scale)    = ScrollOverview::Config::getScale();
    m_isSwiping = false;
}
