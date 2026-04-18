#include "scrollOverview.hpp"
#include <algorithm>
#include <any>
#include <limits>
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

static CBox getOverviewWindowBox(const PHLWINDOW& window, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float yoff) {
    const auto VIEWPORT_CENTER = CBox{{}, monitor->m_size}.middle();

    CBox       box            = {window->m_realPosition->value() - monitor->m_position, window->m_realSize->value()};
    box.translate(-VIEWPORT_CENTER).scale(scale).translate(VIEWPORT_CENTER).translate(-viewOffset * scale).translate({0.F, yoff});
    box.scale(monitor->m_scale).round();

    return box;
}

static CBox getOverviewWorkspaceBox(PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float yoff) {
    const auto VIEWPORT_CENTER = CBox{{}, monitor->m_size}.middle();

    CBox       box            = {{}, monitor->m_size};
    box.translate(-VIEWPORT_CENTER).scale(scale).translate(VIEWPORT_CENTER).translate(-viewOffset * scale).translate({0.F, yoff});
    box.scale(monitor->m_scale).round();

    return box;
}

static bool overviewBoxIntersectsMonitor(const CBox& box, PHLMONITOR monitor) {
    if (!monitor || box.width <= 0 || box.height <= 0)
        return false;

    return box.x < monitor->m_pixelSize.x && box.x + box.width > 0 && box.y < monitor->m_pixelSize.y && box.y + box.height > 0;
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
    static auto* const* PSCALE       = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:scale")->getDataStaticPtr();
    static auto* const* PDEFAULTZOOM = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:scrolling:default_zoom")->getDataStaticPtr();

    const float configuredScale = **PSCALE;
    const float fallbackScale   = **PDEFAULTZOOM;

    return std::clamp(configuredScale >= 0.1F ? configuredScale : fallbackScale, 0.1F, 0.9F);
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

    auto onMouseMove = [this](Vector2D, Event::SCallbackInfo&) {
        if (closing)
            return;

        lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;

        //  highlightHoverDebug();
    };

    auto onTouchMove = [this](ITouch::SMotionEvent, Event::SCallbackInfo& info) {
        if (closing)
            return;

        info.cancelled    = true;
        lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;
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

        static auto* const* PZOOM = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scrolloverview:scrolling:scroll_moves_up_down")->getDataStaticPtr();

        if (!**PZOOM) {
            const auto VAL = std::clamp(sc<float>(scale->value() + e.delta / -500.F), 0.05F, 0.95F);
            *scale         = VAL;
        } else
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

    mouseButtonHook = Event::bus()->m_events.input.mouse.button.listen(onCursorSelect);
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

    SRenderModifData modif;
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_SCALE, renderScale);
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, Vector2D{workspaceBox.x / monitor->m_scale, workspaceBox.y / monitor->m_scale});

    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));

    for (auto const& ls : monitor->m_layerSurfaceLayers[layer]) {
        if (!Desktop::View::validMapped(ls.lock()))
            continue;

        g_pHyprRenderer->renderLayer(ls.lock(), monitor, now);
    }

    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = SRenderModifData{}}));
}

void CScrollOverview::renderWallpaperLayers(const CBox& workspaceBox, float renderScale, const Time::steady_tp& now) {
    if (!pMonitor)
        return;

    renderOverviewLayerLevel(pMonitor.lock(), ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, workspaceBox, renderScale, now);
}

static void renderOverviewWorkspaceShadow(PHLMONITOR monitor, const CBox& workspaceBox, float overviewScale) {
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

    if (!SHADOW.ignoreWindow) {
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

void CScrollOverview::selectHoveredWorkspace() {
    size_t activeIdx = activeWorkspaceIndex();

    const auto VIEWPORT_CENTER = CBox{{}, pMonitor->m_size}.middle();

    const auto WORKSPACEPITCH = getWorkspaceRenderedPitch(pMonitor.lock(), scale->value());
    float      yoff           = -(float)activeIdx * WORKSPACEPITCH;
    bool       found = false;
    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];

        const auto selectWindow = [&](const PHLWINDOW& window) {
            closeOnWindow             = window;
            viewportCurrentWorkspace  = workspaceIdx;
            rememberSelection(window);
            found = true;
        };

        if (wimg->pWorkspace && wimg->pWorkspace->m_hasFullscreenWindow) {
            const auto fullscreenWindow = wimg->pWorkspace->getFullscreenWindow();
            if (fullscreenWindow && validMapped(fullscreenWindow)) {
                CBox texbox = {fullscreenWindow->m_realPosition->value() - pMonitor->m_position, fullscreenWindow->m_realSize->value()};

                texbox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(-viewOffset->value() * scale->value());
                texbox.translate({0.F, yoff});

                if (texbox.containsPoint(lastMousePosLocal)) {
                    selectWindow(fullscreenWindow);
                    break;
                }
            }
        }

        for (auto it = wimg->windowImages.rbegin(); it != wimg->windowImages.rend(); ++it) {
            const auto& img = *it;
            CBox        texbox = {img->pWindow->m_realPosition->value() - pMonitor->m_position, img->pWindow->m_realSize->value()};

            // scale the box to the viewport center
            texbox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(-viewOffset->value() * scale->value());

            texbox.translate({0.F, yoff});

            if (texbox.containsPoint(lastMousePosLocal)) {
                selectWindow(img->pWindow.lock());
                break;
            }
        }
        if (found)
            break;
        yoff += WORKSPACEPITCH;
    }
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
        for (const auto& img : TARGETWORKSPACEIMAGE->windowImages) {
            if (!img->pWindow || !validMapped(img->pWindow))
                continue;

            closeOnWindow = img->pWindow;
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
        for (const auto& img : WSPACE->windowImages) {
            if (img->pWindow == closeOnWindow) {
                rememberSelection(closeOnWindow.lock());
                syncFocusedSelection();
                return;
            }
        }
    }

    if (const auto it = rememberedSelection.find(WSPACE->pWorkspace->m_id); it != rememberedSelection.end()) {
        const auto rememberedWindow = it->second.lock();
        if (rememberedWindow && rememberedWindow->m_workspace == WSPACE->pWorkspace && validMapped(rememberedWindow)) {
            for (const auto& img : WSPACE->windowImages) {
                if (img->pWindow == rememberedWindow) {
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

    for (const auto& img : WSPACE->windowImages) {
        if (!img->pWindow)
            continue;

        closeOnWindow = img->pWindow;
        rememberSelection(img->pWindow.lock());
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

    for (const auto& img : WORKSPACEIMAGE->windowImages) {
        const auto WINDOW = img->pWindow.lock();
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

void CScrollOverview::renderWorkspaceLive(size_t workspaceIdx, size_t activeIdx, float renderScale, const Time::steady_tp& now, std::vector<SRenderedWindow>& renderedWindows) {
    const auto& workspaceImage = images[workspaceIdx];
    if (!workspaceImage || !workspaceImage->pWorkspace)
        return;

    const auto WORKSPACEYOFFSET = (sc<long>(workspaceIdx) - sc<long>(activeIdx)) * getWorkspaceRenderedPitch(pMonitor.lock(), renderScale);
    const auto WORKSPACEBOX     = getOverviewWorkspaceBox(pMonitor.lock(), renderScale, viewOffset->value(), WORKSPACEYOFFSET);

    if (!overviewBoxIntersectsMonitor(WORKSPACEBOX, pMonitor.lock()))
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

    renderOverviewWorkspaceShadow(pMonitor.lock(), WORKSPACEBOX, renderScale);

    if (getWallpaperMode() != 0)
        renderWallpaperLayers(WORKSPACEBOX, renderScale, now);

    renderOverviewLayerLevel(pMonitor.lock(), ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, WORKSPACEBOX, renderScale, now);

    for (const auto& img : workspaceImage->windowImages) {
        if (!img->pWindow || (!img->pWindow->m_isMapped && !img->pWindow->m_fadingOut))
            continue;

        const auto window    = img->pWindow.lock();
        const auto windowBox = getOverviewWindowBox(window, pMonitor.lock(), renderScale, viewOffset->value(), WORKSPACEYOFFSET);
        if (!overviewBoxIntersectsMonitor(windowBox, pMonitor.lock()))
            continue;

        renderWindowLive(window, windowBox, renderScale, now);
        renderedWindows.push_back({window, windowBox});
    }
}

void CScrollOverview::renderWindowLive(PHLWINDOW window, const CBox& windowBox, float renderScale, const Time::steady_tp& now) {
    if (!window)
        return;

    forceWindowVisible(window);
    forceWindowSurfaceVisibility(window);

    if (g_pHyprRenderer->shouldBlur(window)) {
        CRectPassElement::SRectData blurData;
        blurData.box           = windowBox;
        blurData.color         = CHyprColor{0, 0, 0, 0};
        blurData.blur          = true;
        blurData.blurA         = std::sqrt(window->m_alpha->value());
        blurData.round         = sc<int>(std::round(window->rounding() * pMonitor->m_scale * renderScale));
        blurData.roundingPower = window->roundingPower();
        blurData.xray          = window->m_ruleApplicator->xray().valueOr(false);
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(blurData));
    }

    SRenderModifData modif;
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_SCALE, renderScale);
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, Vector2D{windowBox.x / pMonitor->m_scale, windowBox.y / pMonitor->m_scale});

    const float TARGETOPACITY = getOverviewWindowTargetOpacity(window);
    std::vector<SSurfaceOpacityOverride> surfaceOpacityOverrides;
    overrideWindowSurfaceOpacity(window, surfaceOpacityOverrides, TARGETOPACITY);
    auto restoreSurfaceOpacities = Hyprutils::Utils::CScopeGuard([&surfaceOpacityOverrides] { restoreSurfaceOpacityOverrides(surfaceOpacityOverrides); });

    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
    g_pHyprRenderer->renderWindow(window, pMonitor.lock(), now, true, RENDER_PASS_ALL, true, true);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = SRenderModifData{}}));

    if (!g_pHyprRenderer->m_renderPass.empty()) {
        g_pHyprRenderer->m_renderPass.render(CRegion{CBox{{}, pMonitor->m_size}});
        g_pHyprRenderer->m_renderPass.clear();
    }
}

void CScrollOverview::redrawAll(bool forcelowres) {
    rebuildWorkspaceImages();
    seedRememberedSelections();

    for (const auto& img : images) {
        img->windowImages.clear();
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

        auto img     = imageIt->second->windowImages.emplace_back(makeShared<SWindowImage>());
        img->pWindow = window;
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
        for (const auto& wimg : images) {
            for (const auto& img : wimg->windowImages) {
                if (img->pWindow == closeOnWindow && closeOnWindow) {
                    Vector2D middleOfWindow = CBox{img->pWindow->m_realPosition->value(), img->pWindow->m_realSize->value()}.translate({0.F, yoff / scale->value()}).middle() -
                        CBox{pMonitor->m_position, pMonitor->m_size}.middle();

                    // we need to do this because the window doesnt have to be centered after click
                    *viewOffset = middleOfWindow +
                        (CBox{pMonitor->m_position, pMonitor->m_size}.middle() - CBox{img->pWindow->m_realPosition->value(), img->pWindow->m_realSize->value()}.middle());
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
    const auto NOW       = Time::steadyNow();
    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();

    const auto WALLPAPERMODE = getWallpaperMode();

    if (WALLPAPERMODE == 0) {
        g_pHyprRenderer->renderBackground(pMonitor.lock());

        for (auto const& ls : pMonitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]) {
            g_pHyprRenderer->renderLayer(ls.lock(), pMonitor.lock(), NOW);
        }
    } else if (WALLPAPERMODE == 2) {
        renderWallpaperLayers(getOverviewWorkspaceBox(pMonitor.lock(), 1.F, Vector2D{}, 0.F), 1.F, NOW);
    } else
        g_pHyprOpenGL->clear(CHyprColor{0.F, 0.F, 0.F, 1.F});

    if (getOverviewBlur() && WALLPAPERMODE != 1) {
        CRectPassElement::SRectData blurData;
        blurData.box           = CBox{{}, pMonitor->m_size * pMonitor->m_scale};
        blurData.color         = CHyprColor{0.F, 0.F, 0.F, 0.F};
        blurData.blur          = true;
        blurData.blurA         = 1.F;
        blurData.round         = 0;
        blurData.roundingPower = 2.F;
        blurData.xray          = false;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(blurData));
        g_pHyprRenderer->m_renderPass.render(CRegion{CBox{{}, pMonitor->m_size}});
        g_pHyprRenderer->m_renderPass.clear();
    }

    Event::bus()->m_events.render.stage.emit(RENDER_POST_WALLPAPER);

    std::vector<SRenderedWindow> renderedWindows;
    renderedWindows.reserve(g_pCompositor->m_windows.size());

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        renderWorkspaceLive(workspaceIdx, ACTIVEIDX, SCALE, NOW, renderedWindows);
    }

    static auto PACTIVECOL   = CConfigValue<Hyprlang::CUSTOMTYPE>("general:col.active_border");
    static auto PINACTIVECOL = CConfigValue<Hyprlang::CUSTOMTYPE>("general:col.inactive_border");
    auto* const ACTIVECOL    = reinterpret_cast<CGradientValueData*>((PACTIVECOL.ptr())->getData());
    auto* const INACTIVECOL  = reinterpret_cast<CGradientValueData*>((PINACTIVECOL.ptr())->getData());

    for (const auto& renderedWindow : renderedWindows) {
        const auto window = renderedWindow.window.lock();
        if (!window || (!window->m_isMapped && !window->m_fadingOut))
            continue;

        const auto borderSize = window->getRealBorderSize();
        if (borderSize <= 0)
            continue;

        const bool  selected         = closeOnWindow == window;
        const float targetOpacity    = getOverviewWindowTargetOpacity(window);
        const auto& grad             = selected ? window->m_ruleApplicator->activeBorderColor().valueOr(*ACTIVECOL)
                                                : window->m_ruleApplicator->inactiveBorderColor().valueOr(*INACTIVECOL);
        const auto  roundingBase     = window->rounding();
        const auto  roundingPower    = window->roundingPower();
        const auto  correctionOffset = (borderSize * (M_SQRT2 - 1) * std::max(2.0 - roundingPower, 0.0));
        const auto  outerRound       = ((roundingBase + borderSize) - correctionOffset) * pMonitor->m_scale * SCALE;
        const auto  rounding         = roundingBase * pMonitor->m_scale * SCALE;

        CBorderPassElement::SBorderData data;
        data.box           = renderedWindow.box;
        data.grad1         = grad;
        data.round         = sc<int>(std::round(rounding));
        data.outerRound    = sc<int>(std::round(outerRound));
        data.roundingPower = roundingPower;
        data.a             = targetOpacity;
        data.borderSize    = borderSize;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(data));
    }

    if (!g_pHyprRenderer->m_renderPass.empty()) {
        g_pHyprRenderer->m_renderPass.render(CRegion{CBox{{}, pMonitor->m_size}});
        g_pHyprRenderer->m_renderPass.clear();
    }

    for (auto const& ls : pMonitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP]) {
        g_pHyprRenderer->renderLayer(ls.lock(), pMonitor.lock(), NOW);
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
