#include "scrollOverview.hpp"
#include <any>
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
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#undef private
#include "OverviewPassElement.hpp"

static uint32_t getOverviewFramebufferFormat(PHLMONITOR monitor) {
    if (!monitor || !monitor->m_output)
        return DRM_FORMAT_ARGB8888;

    return monitor->inHDR() ? DRM_FORMAT_ABGR16161616F : monitor->m_output->state->state().drmFormat;
}

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

static CBox getOverviewWindowBox(const PHLWINDOW& window, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float yoff) {
    const auto VIEWPORT_CENTER = CBox{{}, monitor->m_size}.middle();

    CBox       box            = {window->m_realPosition->value() - monitor->m_position, window->m_realSize->value()};
    box.translate(-VIEWPORT_CENTER).scale(scale).translate(VIEWPORT_CENTER).translate(-viewOffset * scale).translate({0.F, yoff});
    box.scale(monitor->m_scale).round();

    return box;
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
    static auto* const* PDEFAULTZOOM = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:scrolling:default_zoom")->getDataStaticPtr();

    const auto          PMONITOR = Desktop::focusState()->monitor();
    pMonitor                     = PMONITOR;

    rebuildWorkspaceImages();

    g_pAnimationManager->createAnimation(1.F, scale, g_pConfigManager->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
    g_pAnimationManager->createAnimation({}, viewOffset, g_pConfigManager->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);

    scale->setUpdateCallback(damageMonitor);
    viewOffset->setUpdateCallback(damageMonitor);

    if (!swipe)
        *scale = std::clamp(**PDEFAULTZOOM, 0.1F, 0.9F);

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

        static auto* const* PZOOM = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:scrolling:scroll_moves_up_down")->getDataStaticPtr();

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
            case XKB_KEY_KP_Up: moveViewportWorkspace(false); break;
            case XKB_KEY_Down:
            case XKB_KEY_KP_Down: moveViewportWorkspace(true); break;
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

    float      yoff  = -(float)activeIdx * pMonitor->m_size.y * scale->value();
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
        yoff += pMonitor->m_size.y * scale->value();
    }
}

void CScrollOverview::moveViewportWorkspace(bool up) {
    if (images.empty())
        return;

    size_t activeIdx = activeWorkspaceIndex();

    if (viewportCurrentWorkspace == 0 && !up)
        return;
    if (viewportCurrentWorkspace == images.size() - 1 && up)
        return;

    if (up)
        viewportCurrentWorkspace++;
    else
        viewportCurrentWorkspace--;

    *viewOffset = {viewOffset->value().x, (sc<long>(viewportCurrentWorkspace) - sc<long>(activeIdx)) * pMonitor->m_size.y};
    syncSelectionToViewport();
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

void CScrollOverview::moveWindowSelection(const std::string& direction) {
    if (images.empty() || viewportCurrentWorkspace >= images.size() || direction.empty())
        return;

    syncFocusedSelection();

    const auto LASTWINDOW = Desktop::focusState()->window();
    const auto RESULT     = CKeybindManager::moveFocusTo(direction);

    if (!RESULT.success)
        return;

    const auto FOCUSED = Desktop::focusState()->window();
    if (!FOCUSED || FOCUSED == LASTWINDOW)
        return;

    closeOnWindow = FOCUSED;
    rememberSelection(FOCUSED);
    damage();
}

SP<CScrollOverview::SWorkspaceImage> CScrollOverview::imageForWorkspace(PHLWORKSPACE w) {
    for (const auto& i : images) {
        if (i->pWorkspace == w)
            return i;
    }
    return nullptr;
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

void CScrollOverview::renderWorkspaceLive(PHLWORKSPACE workspace, const Time::steady_tp& now) {
    if (!workspace)
        return;

    const bool WASVISIBLE        = workspace->m_visible;
    const bool WASFORCERENDERING = workspace->m_forceRendering;
    workspace->m_visible         = true;
    workspace->m_forceRendering  = true;

    auto restoreWorkspaceState = Hyprutils::Utils::CScopeGuard([workspace, WASVISIBLE, WASFORCERENDERING] {
        workspace->m_visible        = WASVISIBLE;
        workspace->m_forceRendering = WASFORCERENDERING;
    });

    size_t workspaceIdx = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == workspace) {
            workspaceIdx = i;
            break;
        }
    }

    const auto ACTIVEIDX        = activeWorkspaceIndex();
    const auto WORKSPACEYOFFSET = (sc<long>(workspaceIdx) - sc<long>(ACTIVEIDX)) * pMonitor->m_size.y * scale->value();
    const auto WORKSPACEIMAGE   = imageForWorkspace(workspace);

    if (!WORKSPACEIMAGE)
        return;

    for (const auto& img : WORKSPACEIMAGE->windowImages) {
        if (!img->pWindow || (!img->pWindow->m_isMapped && !img->pWindow->m_fadingOut))
            continue;

        renderWindowLive(img->pWindow.lock(), WORKSPACEYOFFSET, now);
    }
}

void CScrollOverview::renderWindowLive(PHLWINDOW window, float workspaceYOffset, const Time::steady_tp& now) {
    if (!window)
        return;

    forceWindowVisible(window);
    forceWindowSurfaceVisibility(window);

    const auto WINDOWBOX = getOverviewWindowBox(window, pMonitor.lock(), scale->value(), viewOffset->value(), workspaceYOffset);

    if (g_pHyprRenderer->shouldBlur(window)) {
        CRectPassElement::SRectData blurData;
        blurData.box           = WINDOWBOX;
        blurData.color         = CHyprColor{0, 0, 0, 0};
        blurData.blur          = true;
        blurData.blurA         = std::sqrt(window->m_alpha->value());
        blurData.round         = sc<int>(std::round(window->rounding() * pMonitor->m_scale * scale->value()));
        blurData.roundingPower = window->roundingPower();
        blurData.xray          = window->m_ruleApplicator->xray().valueOr(false);
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(blurData));
    }

    SRenderModifData modif;
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_SCALE, scale->value());
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, Vector2D{WINDOWBOX.x / pMonitor->m_scale, WINDOWBOX.y / pMonitor->m_scale});

    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
    g_pHyprRenderer->renderWindow(window, pMonitor.lock(), now, true, RENDER_PASS_ALL, true, true);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = SRenderModifData{}}));
}

void CScrollOverview::redrawWorkspace(PHLWORKSPACE workspace, bool forcelowres) {
    auto image = imageForWorkspace(workspace);

    if (!image)
        return;

    image->windowImages.clear();

    std::vector<PHLWINDOW> windows;
    for (const auto& w : g_pCompositor->m_windows) {
        if (!validMapped(w) || w->m_workspace != workspace)
            continue;
        windows.emplace_back(w);
    }

    for (const auto& w : windows) {
        auto img     = image->windowImages.emplace_back(makeShared<SWindowImage>());
        img->pWindow = w;
    }
}

void CScrollOverview::redrawWindowImage(SP<SWindowImage> img) {
    if (!img->pWindow)
        return;

    const auto FBFORMAT = getOverviewFramebufferFormat(pMonitor.lock());
    if (img->fb.m_size != pMonitor->m_pixelSize || img->fb.m_drmFormat != FBFORMAT) {
        img->fb.release();
        img->fb.alloc(pMonitor->m_pixelSize.x, pMonitor->m_pixelSize.y, FBFORMAT);
    }

    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->beginRender(pMonitor.lock(), fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &img->fb);

    g_pHyprOpenGL->clear(CHyprColor{0, 0, 0, 0});

    g_pHyprRenderer->renderWindow(img->pWindow.lock(), pMonitor.lock(), Time::steadyNow(), true, RENDER_PASS_ALL, true, true);

    g_pHyprOpenGL->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();

    img->lastWindowPosition = img->pWindow->m_realPosition->value();
    img->lastWindowSize     = img->pWindow->m_realSize->value();
}

void CScrollOverview::redrawAll(bool forcelowres) {
    rebuildWorkspaceImages();
    seedRememberedSelections();

    for (const auto& img : images) {
        redrawWorkspace(img->pWorkspace);
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

    if (!closeOnWindow)
        closeOnWindow = Desktop::focusState()->window();

    if (closeOnWindow == Desktop::focusState()->window())
        *viewOffset = Vector2D{};
    else {

        if (closeOnWindow->m_workspace != pMonitor->m_activeWorkspace) {
            g_pDesktopAnimationManager->startAnimation(pMonitor->m_activeWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_OUT, true, true);
            g_pDesktopAnimationManager->startAnimation(closeOnWindow->m_workspace, CDesktopAnimationManager::ANIMATION_TYPE_IN, false, true);
            pMonitor->changeWorkspace(closeOnWindow->m_workspace, true, true, true);
        }

        Desktop::focusState()->fullWindowFocus(closeOnWindow.lock(), Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

        size_t activeIdx = activeWorkspaceIndex();

        float yoff  = -(float)activeIdx * pMonitor->m_size.y * scale->value();
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
            yoff += pMonitor->m_size.y * scale->value();
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
    viewOffset->setValueAndWarp(Vector2D{0.F, (sc<long>(previousActiveIdx) - sc<long>(viewportCurrentWorkspace)) * pMonitor->m_size.y});
    *viewOffset = Vector2D{};
    syncSelectionToViewport();
    damage();
}

void CScrollOverview::render() {
    const auto NOW          = Time::steadyNow();
    const auto ACTIVEIDX    = activeWorkspaceIndex();
    const auto SCALE        = scale->value();
    const auto VIEWOFFSETPX = viewOffset->value() * SCALE;
    const auto VIEWCENTER   = CBox{{}, pMonitor->m_size}.middle();

    g_pHyprRenderer->renderBackground(pMonitor.lock());

    for (auto const& ls : pMonitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]) {
        g_pHyprRenderer->renderLayer(ls.lock(), pMonitor.lock(), NOW);
    }

    Event::bus()->m_events.render.stage.emit(RENDER_POST_WALLPAPER);

    for (auto const& ls : pMonitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM]) {
        g_pHyprRenderer->renderLayer(ls.lock(), pMonitor.lock(), NOW);
    }

    for (const auto& workspaceImage : images) {
        renderWorkspaceLive(workspaceImage->pWorkspace, NOW);
    }

    for (auto const& ls : pMonitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP]) {
        g_pHyprRenderer->renderLayer(ls.lock(), pMonitor.lock(), NOW);
    }

    if (!closeOnWindow || !validMapped(closeOnWindow))
        return;

    static auto PACTIVECOL = CConfigValue<Hyprlang::CUSTOMTYPE>("general:col.active_border");
    auto* const ACTIVECOL  = reinterpret_cast<CGradientValueData*>((PACTIVECOL.ptr())->getData());
    const auto  grad       = closeOnWindow->m_ruleApplicator->activeBorderColor().valueOr(*ACTIVECOL);
    const auto  borderSize = closeOnWindow->getRealBorderSize();

    if (borderSize <= 0)
        return;

    size_t workspaceIdx = 0;
    bool   found        = false;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == closeOnWindow->m_workspace) {
            workspaceIdx = i;
            found        = true;
            break;
        }
    }

    if (!found)
        return;

    const auto  ROUNDINGBASE     = closeOnWindow->rounding();
    const auto  ROUNDINGPOWER    = closeOnWindow->roundingPower();
    const auto  CORRECTIONOFFSET = (borderSize * (M_SQRT2 - 1) * std::max(2.0 - ROUNDINGPOWER, 0.0));
    const auto  OUTERROUND       = ((ROUNDINGBASE + borderSize) - CORRECTIONOFFSET) * pMonitor->m_scale * SCALE;
    const auto  ROUNDING         = ROUNDINGBASE * pMonitor->m_scale * SCALE;
    const float selectedYOff     = (sc<long>(workspaceIdx) - sc<long>(ACTIVEIDX)) * pMonitor->m_size.y * SCALE;
    const auto  WINDOWBOX        = getOverviewWindowBox(closeOnWindow.lock(), pMonitor.lock(), SCALE, viewOffset->value(), selectedYOff);

    CBorderPassElement::SBorderData data;
    data.box           = WINDOWBOX;
    data.grad1         = grad;
    data.round         = sc<int>(std::round(ROUNDING));
    data.outerRound    = sc<int>(std::round(OUTERROUND));
    data.roundingPower = ROUNDINGPOWER;
    data.a             = 1.F;
    data.borderSize    = borderSize;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(data));
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
    static auto* const* PDEFAULTZOOM = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:scrolling:default_zoom")->getDataStaticPtr();

    if (closing) {
        close();
        return;
    }

    (*scale)    = **PDEFAULTZOOM;
    m_isSwiping = false;
}

void CScrollOverview::onSwipeUpdate(double delta) {
    static auto* const* PDEFAULTZOOM = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:scrolling:default_zoom")->getDataStaticPtr();
    static auto* const* PDISTANCE    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:gesture_distance")->getDataStaticPtr();

    m_isSwiping = true;

    const float PERC = closing ? std::clamp(delta / (double)**PDISTANCE, 0.0, 1.0) : 1.0 - std::clamp(delta / (double)**PDISTANCE, 0.0, 1.0);

    scale->setValueAndWarp(hyprlerp(1.F, **PDEFAULTZOOM, PERC));
}

void CScrollOverview::onSwipeEnd() {
    static auto* const* PDEFAULTZOOM = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:scrolling:default_zoom")->getDataStaticPtr();

    if (closing) {
        close();
        return;
    }

    (*scale)    = **PDEFAULTZOOM;
    m_isSwiping = false;
}
