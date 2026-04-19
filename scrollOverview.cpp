#include "scrollOverview.hpp"
#include <algorithm>
#include <any>
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
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/config/ConfigDataValues.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
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

static int getWallpaperMode() {
    static auto* const* PMODE = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:wallpaper")->getDataStaticPtr();
    return std::clamp<int>(**PMODE, 0, 2);
}

static bool getOverviewBlur() {
    static auto* const* PBLUR = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:blur")->getDataStaticPtr();
    return **PBLUR;
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

class COverviewWindowShadowPassElement : public IPassElement {
  public:
    struct SData {
        PHLMONITORREF monitor;
        CBox          fullBox;
        CBox          cutoutBox;
        int           rounding      = 0;
        float         roundingPower = 2.F;
        int           range         = 0;
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
            shadowDamage.subtract(data.cutoutBox.copy().expand(-data.rounding));

        if (shadowDamage.empty())
            return;

        const auto SAVEDDAMAGE = g_pHyprOpenGL->m_renderData.damage;
        g_pHyprOpenGL->m_renderData.damage = shadowDamage;
        auto restoreDamage = Hyprutils::Utils::CScopeGuard([SAVEDDAMAGE] { g_pHyprOpenGL->m_renderData.damage = SAVEDDAMAGE; });

        auto color = data.color;
        color.a *= data.alpha;

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

CScrollOverview::~CScrollOverview() {
    g_pHyprRenderer->makeEGLCurrent();
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

    g_pAnimationManager->createAnimation(1.F, scale, g_pConfigManager->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
    g_pAnimationManager->createAnimation({}, viewOffset, g_pConfigManager->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);

    scale->setUpdateCallback(damageMonitor);
    viewOffset->setUpdateCallback(damageMonitor);

    if (!swipe)
        *scale = getOverviewConfiguredScale();

    lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;

    auto onMouseMove = [this](Vector2D, Event::SCallbackInfo& info) {
        if (closing)
            return;

        lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;

        if (dragPointerDown && dragPendingWindow) {
            info.cancelled = true;

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

        if (window && window->m_monitor == pMonitor) {
            closeOnWindow = window;
            rememberSelection(window);

            for (size_t i = 0; i < images.size(); ++i) {
                if (images[i]->pWorkspace == window->m_workspace) {
                    viewportCurrentWorkspace = i;
                    break;
                }
            }
        }

        damage();
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

    static auto* const* PGLOBALRENDERPOWER = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "decoration:shadow:render_power")->getDataStaticPtr();

    const auto PREVRENDERPOWER = **PGLOBALRENDERPOWER;
    **PGLOBALRENDERPOWER       = SHADOW.renderPower;
    auto restoreRenderPower    = Hyprutils::Utils::CScopeGuard([PREVRENDERPOWER] {
        static auto* const* PGLOBALRENDERPOWER = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "decoration:shadow:render_power")->getDataStaticPtr();
        **PGLOBALRENDERPOWER                   = PREVRENDERPOWER;
    });

    const int RANGE = sc<int>(std::round(SHADOW.range * monitor->m_scale * overviewScale));
    if (RANGE <= 0)
        return;

    const auto FULLBOX = workspaceBox.copy().expand(RANGE);
    if (FULLBOX.width < 1 || FULLBOX.height < 1)
        return;

    if (!cutoutCenter) {
        g_pHyprOpenGL->renderRoundedShadow(FULLBOX, 0, 2.F, RANGE, SHADOW.color, 1.F);
        return;
    }

    const auto SAVEDDAMAGE = g_pHyprOpenGL->m_renderData.damage;
    g_pHyprOpenGL->m_renderData.damage = FULLBOX;
    g_pHyprOpenGL->m_renderData.damage.subtract(workspaceBox).intersect(SAVEDDAMAGE);

    g_pHyprOpenGL->renderRoundedShadow(FULLBOX, 0, 2.F, RANGE, SHADOW.color, 1.F);

    g_pHyprOpenGL->m_renderData.damage = SAVEDDAMAGE;
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
            const auto rememberedWindow = it->second.lock();
            if (rememberedWindow && rememberedWindow->m_workspace == img->pWorkspace && validMapped(rememberedWindow))
                continue;
        }

        const auto lastFocusedWindow = img->pWorkspace->getLastFocusedWindow();
        if (!lastFocusedWindow || lastFocusedWindow->m_workspace != img->pWorkspace || !validMapped(lastFocusedWindow))
            continue;

        rememberedSelection[WORKSPACEID] = lastFocusedWindow;
    }
}

void CScrollOverview::rememberSelection(PHLWINDOW window) {
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

        if (wimg->pWorkspace && wimg->pWorkspace->m_hasFullscreenWindow) {
            const auto fullscreenWindow = wimg->pWorkspace->getFullscreenWindow();
            if (fullscreenWindow && validMapped(fullscreenWindow)) {
                CBox texbox = {fullscreenWindow->m_realPosition->value() - pMonitor->m_position, fullscreenWindow->m_realSize->value()};

                texbox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(-viewOffset->value() * scale->value());
                texbox.translate({0.F, yoff});

                if (texbox.containsPoint(lastMousePosLocal)) {
                    return selectWindow(fullscreenWindow);
                }
            }
        }

        for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
            const auto window = it->lock();
            if (!window)
                continue;

            const auto texbox = getOverviewWindowBoxLogical(window, pMonitor.lock(), scale->value(), viewOffset->value(), yoff);

            if (texbox.containsPoint(lastMousePosLocal)) {
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

    for (auto it = images[workspaceIdx]->windows.rbegin(); it != images[workspaceIdx]->windows.rend(); ++it) {
        const auto WINDOW = it->lock();
        if (!WINDOW || WINDOW == ignoredWindow || !validMapped(WINDOW))
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

    if (!window)
        return;

    closeOnWindow            = window;
    viewportCurrentWorkspace = workspaceIdx;
    rememberSelection(window);
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
    const auto WINDOW  = dragActiveWindow.lock();
    const auto MONITOR = pMonitor.lock();
    if (!WINDOW || !MONITOR || workspaceIdx >= images.size())
        return {};

    const auto WORKSPACE_YOFFSET = (sc<long>(workspaceIdx) - sc<long>(activeWorkspaceIndex())) * getWorkspaceRenderedPitch(MONITOR, scale->value());
    auto       box               = getOverviewWindowBoxLogical(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACE_YOFFSET);
    box.translate(lastMousePosLocal - dragStartMouseLocal);

    return box;
}

void CScrollOverview::beginWindowDrag() {
    const auto WINDOW = dragPendingWindow.lock();
    if (!WINDOW || !validMapped(WINDOW) || !WINDOW->layoutTarget())
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
    const auto WINDOW = dragActiveWindow.lock();
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
            const auto OTHERWINDOW = windowRef.lock();
            if (!OTHERWINDOW || OTHERWINDOW == WINDOW || !validMapped(OTHERWINDOW))
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

    if (DROPANCHOR) {
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

        if (DROPANCHOR && !dropDirection.empty())
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

    dragPointerDown = false;
    dragPendingWindow.reset();
    dragActiveWindow.reset();
    dragOriginalWorkspace.reset();
    dragStartedTiled      = false;
    dragOriginalFloatSize = Vector2D{};
    dragOriginalBox       = CBox{};
    rebuildPending        = true;
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
        const auto rememberedWindow = it->second.lock();
        if (rememberedWindow && rememberedWindow->m_workspace == TARGETWORKSPACEIMAGE->pWorkspace && validMapped(rememberedWindow))
            closeOnWindow = rememberedWindow;
    }

    if (!closeOnWindow) {
        for (const auto& windowRef : TARGETWORKSPACEIMAGE->windows) {
            const auto window = windowRef.lock();
            if (!window || !validMapped(window))
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
        const auto selectedWindow = closeOnWindow.lock();
        for (const auto& windowRef : WSPACE->windows) {
            if (windowRef.lock() == selectedWindow) {
                rememberSelection(closeOnWindow.lock());
                syncFocusedSelection();
                return;
            }
        }
    }

    if (const auto it = rememberedSelection.find(WSPACE->pWorkspace->m_id); it != rememberedSelection.end()) {
        const auto rememberedWindow = it->second.lock();
        if (rememberedWindow && rememberedWindow->m_workspace == WSPACE->pWorkspace && validMapped(rememberedWindow)) {
            for (const auto& windowRef : WSPACE->windows) {
                if (windowRef.lock() == rememberedWindow) {
                    closeOnWindow = rememberedWindow;
                    syncFocusedSelection();
                    return;
                }
            }
        }
    }

    const auto focusedWindow = Desktop::focusState()->window();
    if (focusedWindow && focusedWindow->m_workspace == WSPACE->pWorkspace) {
        closeOnWindow = focusedWindow;
        rememberSelection(focusedWindow);
        syncFocusedSelection();
        return;
    }

    for (const auto& windowRef : WSPACE->windows) {
        const auto window = windowRef.lock();
        if (!window)
            continue;

        closeOnWindow = window;
        rememberSelection(window);
        syncFocusedSelection();
        return;
    }

    closeOnWindow.reset();
}

void CScrollOverview::syncFocusedSelection() {
    if (!closeOnWindow || !validMapped(closeOnWindow))
        return;

    if (Desktop::focusState()->window() == closeOnWindow && closeOnWindow->m_workspace == pMonitor->m_activeWorkspace)
        return;

    Desktop::focusState()->fullWindowFocus(closeOnWindow.lock(), Desktop::FOCUS_REASON_KEYBIND);
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

    if (!closeOnWindow || closeOnWindow->m_workspace != WORKSPACEIMAGE->pWorkspace || !validMapped(closeOnWindow)) {
        syncSelectionToViewport();
        if (!closeOnWindow || closeOnWindow->m_workspace != WORKSPACEIMAGE->pWorkspace || !validMapped(closeOnWindow))
            return false;
    }

    const auto CURRENT = closeOnWindow.lock();
    if (!CURRENT)
        return false;

    const auto CURRENTCENTER = CURRENT->middle();

    PHLWINDOW bestCandidate;
    float     bestPrimaryDistance   = std::numeric_limits<float>::max();
    float     bestSecondaryDistance = std::numeric_limits<float>::max();
    float     bestOverlap           = -1.F;
    bool      bestHasOverlap         = false;

    for (const auto& windowRef : WORKSPACEIMAGE->windows) {
        const auto WINDOW = windowRef.lock();
        if (!WINDOW || WINDOW == CURRENT || !validMapped(WINDOW))
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

            ls->m_aboveFullscreen = true;
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
    for (auto& entry : forcedWindowVisibility) {
        if (!entry.window)
            continue;

        entry.window->m_hidden = entry.hidden;
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

    renderOverviewWorkspaceShadow(monitor, WORKSPACEBOX, renderScale, wallpaperMode == 0);

    if (wallpaperMode != 0)
        renderWallpaperLayers(monitor, WORKSPACEBOX, renderScale, now);

    renderOverviewLayerLevel(monitor, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, WORKSPACEBOX, renderScale, now);

    const auto renderWindowsByFullscreenState = [&](bool fullscreen) {
        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = windowRef.lock();
            if (!window || (!window->m_isMapped && !window->m_fadingOut))
                continue;
            if (dragActiveWindow && window == dragActiveWindow)
                continue;
            if (window->isFullscreen() != fullscreen)
                continue;

            const auto windowBox = getOverviewWindowBox(window, monitor, renderScale, viewOffset->value(), WORKSPACEYOFFSET);
            if (!overviewBoxIntersectsMonitor(windowBox, monitor))
                continue;

            renderWindowLive(monitor, window, windowBox, renderScale, now);
        }
    };

    renderWindowsByFullscreenState(false);
    renderWindowsByFullscreenState(true);
}

void CScrollOverview::renderDraggedWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale, const Time::steady_tp& now) {
    const auto WINDOW = dragActiveWindow.lock();
    if (!WINDOW || !WINDOW->m_workspace || (!WINDOW->m_isMapped && !WINDOW->m_fadingOut))
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

void CScrollOverview::renderWindowLive(PHLMONITOR monitor, PHLWINDOW window, const CBox& windowBox, float renderScale, const Time::steady_tp& now) {
    if (!window)
        return;

    forceWindowVisible(window);
    forceWindowSurfaceVisibility(window);

    renderOverviewWindowShadow(monitor, window, windowBox, renderScale, closeOnWindow == window);

    if (g_pHyprRenderer->shouldBlur(window)) {
        CRectPassElement::SRectData blurData;
        blurData.box           = windowBox;
        blurData.color         = CHyprColor{0, 0, 0, 0};
        blurData.blur          = true;
        blurData.blurA         = std::sqrt(window->m_alpha->value());
        blurData.round         = sc<int>(std::round(window->rounding() * monitor->m_scale * renderScale));
        blurData.roundingPower = window->roundingPower();
        blurData.xray          = window->m_ruleApplicator->xray().valueOr(false);
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(blurData));
    }

    SRenderModifData modif;
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_SCALE, renderScale);
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, Vector2D{windowBox.x / monitor->m_scale, windowBox.y / monitor->m_scale});

    const float TARGETOPACITY = getOverviewWindowTargetOpacity(window);
    std::vector<SSurfaceOpacityOverride> surfaceOpacityOverrides;
    surfaceOpacityOverrides.reserve(4);
    overrideWindowSurfaceOpacity(window, surfaceOpacityOverrides, TARGETOPACITY);
    auto restoreSurfaceOpacities = Hyprutils::Utils::CScopeGuard([&surfaceOpacityOverrides] { restoreSurfaceOpacityOverrides(surfaceOpacityOverrides); });

    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
    const auto firstWindowPassElement = g_pHyprRenderer->m_renderPass.m_passElements.size();
    g_pHyprRenderer->renderWindow(window, monitor, now, true, RENDER_PASS_ALL, true, true);
    roundStandaloneWindowPassElements(window, monitor, renderScale, firstWindowPassElement);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = SRenderModifData{}}));
    renderOverviewWindowBorder(monitor, window, windowBox, renderScale, closeOnWindow == window);

    renderOverviewPass(monitor);
}

void CScrollOverview::redrawAll(bool forcelowres) {
    rebuildWorkspaceImages();
    seedRememberedSelections();

    for (const auto& img : images) {
        img->windows.clear();
    }

    std::unordered_map<WORKSPACEID, SP<SWorkspaceImage>> imagesByWorkspace;
    imagesByWorkspace.reserve(images.size());

    for (const auto& img : images) {
        if (img && img->pWorkspace)
            imagesByWorkspace.emplace(img->pWorkspace->m_id, img);
    }

    for (const auto& window : g_pCompositor->m_windows) {
        if (!validMapped(window) || !window->m_workspace)
            continue;

        const auto imageIt = imagesByWorkspace.find(window->m_workspace->m_id);
        if (imageIt == imagesByWorkspace.end())
            continue;

        imageIt->second->windows.emplace_back(window);
    }
}

void CScrollOverview::damage() {
    blockDamageReporting = true;
    g_pHyprRenderer->damageMonitor(pMonitor.lock());
    blockDamageReporting = false;
}

void CScrollOverview::onDamageReported() {
    if (closing)
        return;

    damage();
}

void CScrollOverview::close() {
    closing = true;

    const auto SELECTEDWORKSPACE =
        viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] ? images[viewportCurrentWorkspace]->pWorkspace : PHLWORKSPACE{};

    if (!closeOnWindow && (!SELECTEDWORKSPACE || SELECTEDWORKSPACE == pMonitor->m_activeWorkspace))
        closeOnWindow = Desktop::focusState()->window();

    if (!closeOnWindow) {
        if (SELECTEDWORKSPACE && SELECTEDWORKSPACE != pMonitor->m_activeWorkspace)
            pMonitor->changeWorkspace(SELECTEDWORKSPACE, false, true, true);

        *viewOffset = Vector2D{};
    } else if (closeOnWindow == Desktop::focusState()->window())
        *viewOffset = Vector2D{};
    else {

        if (closeOnWindow->m_workspace != pMonitor->m_activeWorkspace) {
            g_pDesktopAnimationManager->startAnimation(pMonitor->m_activeWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_OUT, true, true);
            g_pDesktopAnimationManager->startAnimation(closeOnWindow->m_workspace, CDesktopAnimationManager::ANIMATION_TYPE_IN, false, true);
            pMonitor->changeWorkspace(closeOnWindow->m_workspace, true, true, true);
        }

        Desktop::focusState()->fullWindowFocus(closeOnWindow.lock(), Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

        size_t activeIdx = activeWorkspaceIndex();

        const auto WORKSPACEPITCH = getWorkspaceRenderedPitch(pMonitor.lock(), scale->value());
        float      yoff           = -(float)activeIdx * WORKSPACEPITCH;
        bool  found = false;
        const auto selectedWindow = closeOnWindow.lock();
        for (const auto& wimg : images) {
            for (const auto& windowRef : wimg->windows) {
                const auto window = windowRef.lock();
                if (window == selectedWindow && window) {
                    Vector2D middleOfWindow = CBox{window->m_realPosition->value(), window->m_realSize->value()}.translate({0.F, yoff / scale->value()}).middle() -
                        CBox{pMonitor->m_position, pMonitor->m_size}.middle();

                    // we need to do this because the window doesnt have to be centered after click
                    *viewOffset = middleOfWindow +
                        (CBox{pMonitor->m_position, pMonitor->m_size}.middle() - CBox{window->m_realPosition->value(), window->m_realSize->value()}.middle());
                    found = true;
                    break;
                }
            }
            if (found)
                break;
            yoff += WORKSPACEPITCH;
        }
    }

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
        onWorkspaceChange();
        return;
    }

    if (rebuildPending) {
        rebuildPending = false;
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
    damage();
}

void CScrollOverview::render() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    const auto NOW       = Time::steadyNow();
    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE);

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
        renderWorkspaceLive(MONITOR, workspaceIdx, ACTIVEIDX, PITCH, SCALE, WALLPAPERMODE, NOW);
    }

    renderDraggedWindow(MONITOR, ACTIVEIDX, PITCH, SCALE, NOW);

    for (auto const& ls : MONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP]) {
        if (!Desktop::View::validMapped(ls.lock()))
            continue;

        g_pHyprRenderer->renderLayer(ls.lock(), MONITOR, NOW);
    }

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
