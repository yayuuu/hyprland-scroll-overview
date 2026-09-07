#include "Window.hpp"
#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <functional>
#define private public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/types.hpp>
#include <hyprland/src/render/decorations/CHyprGroupBarDecoration.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#undef protected
#undef private
#include "Config.hpp"
#include "OverviewPassElement.hpp"
#include "OverviewRender.hpp"

namespace OverviewWindow {
namespace {

struct SOverviewCustomDecorationRenderState {
    bool                                queuedAny = false;
    std::vector<std::function<void()>> restoreFns;
};

struct SHyprbarButtonMirror {
    std::string         cmd     = "";
    bool                userfg  = false;
    CHyprColor          fgcol   = CHyprColor(0, 0, 0, 0);
    CHyprColor          bgcol   = CHyprColor(0, 0, 0, 0);
    float               size    = 10.F;
    std::string         icon    = "";
    SP<Render::ITexture> iconTex;
};

struct SHyprbarGlobalStateMirror {
    std::vector<SHyprbarButtonMirror> buttons;
};

struct SOverviewWindowMetrics {
    float renderScale             = 1.F;
    float pxScale                 = 1.F;
    float targetOpacity           = 1.F;
    float borderSize              = 0.F;
    int   borderPx                = 0;
    float borderPxScaled          = 0.F;
    float roundingBase            = 0.F;
    float roundingPower           = 2.F;
    float correctionOffset        = 0.F;
    float outerRound              = 0.F;
    int   roundingPx              = 0;
    int   outerRoundPx            = 0;
    float hyprbarLogicalHeight    = 0.F;
    int   hyprbarHeightPx         = 0;
    float hyprbarHeightPxScaled   = 0.F;
    float hyprbarTopOffsetLogical = 0.F;
    bool  shadowIncludesHyprbar   = false;
    bool  borderIncludesHyprbar   = false;
};

// SurfacePassElement only scales subsurface textures while the window size is
// marked as animated. Overview temporarily changes the current window size
// without starting a real animation, so expose that single bit for the duration
// of the synchronous overview render and restore it immediately afterwards.
struct COverviewAnimatedVariableAccess : Hyprutils::Animation::CBaseAnimatedVariable {
    static void setBeingAnimated(Hyprutils::Animation::CBaseAnimatedVariable* variable, bool animated) {
        if (!variable)
            return;

        rc<COverviewAnimatedVariableAccess*>(variable)->m_bIsBeingAnimated = animated;
    }
};

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

static float getOverviewWindowTargetOpacity(const PHLWINDOW& window) {
    if (!window)
        return 1.F;

    const bool  fullscreen     = Fullscreen::controller()->isFullscreen(window);
    const bool  active         = Desktop::focusState()->window() == window;
    float       targetOpacity  = fullscreen ? ScrollOverview::Config::getValue<float>("decoration:fullscreen_opacity") :
        active ? ScrollOverview::Config::getValue<float>("decoration:active_opacity") : ScrollOverview::Config::getValue<float>("decoration:inactive_opacity");
    const auto& ruleOpacityVar = fullscreen ? window->m_ruleApplicator->alphaFullscreen() : active ? window->m_ruleApplicator->alpha() : window->m_ruleApplicator->alphaInactive();

    targetOpacity = ruleOpacityVar.valueOr(Desktop::Types::SAlphaValue{}).applyAlpha(targetOpacity);

    return std::clamp(targetOpacity, 0.F, 1.F);
}

struct SOverviewPseudoFocusState {
    PHLWINDOW                     window;
    SP<Desktop::CFocusState>       focusState;
    PHLWINDOWREF                  previousFocusWindow;
    WP<CWLSurfaceResource>         previousFocusSurface;
    Config::CGradientValueData     previousBorderColor;
    Config::CGradientValueData     previousBorderColorPrevious;
    Config::CGradientValueData     previousShadowColor;
    Config::CGradientValueData     previousShadowColorPrevious;
    Config::CGradientValueData     previousGlowColor;
    Config::CGradientValueData     previousGlowColorPrevious;
    float                          previousOpacity = 1.F;
    float                          previousDim     = 0.F;
    bool                           active          = false;

    SOverviewPseudoFocusState(const PHLWINDOW& window_, const PHLWINDOW& pseudoFocusWindow) : window(window_) {
        if (!window || !pseudoFocusWindow)
            return;

        active                      = true;
        focusState                  = Desktop::focusState();
        previousFocusWindow         = focusState->m_focusWindow;
        previousFocusSurface        = focusState->m_focusSurface;
        previousBorderColor         = window->m_realBorderColor;
        previousBorderColorPrevious = window->m_realBorderColorPrevious;
        previousShadowColor         = window->m_realShadowColor;
        previousShadowColorPrevious = window->m_realShadowColorPrevious;
        previousGlowColor           = window->m_realGlowColor;
        previousGlowColorPrevious   = window->m_realGlowColorPrevious;
        previousOpacity             = window->alpha(Desktop::View::WINDOW_ALPHA_ACTIVE)->value();
        previousDim                 = window->m_dimPercent->value();

        focusState->m_focusWindow = pseudoFocusWindow;
        if (pseudoFocusWindow->wlSurface() && pseudoFocusWindow->wlSurface()->resource())
            focusState->m_focusSurface = pseudoFocusWindow->wlSurface()->resource();

        const bool ISACTIVE    = window == pseudoFocusWindow;
        const bool GROUPLOCKED = window->m_group ? window->m_group->locked() || g_pKeybindManager->m_groupsLocked : g_pKeybindManager->m_groupsLocked;
        const bool NOGROUP     = window->m_groupRules & Desktop::View::GROUP_DENY;
        const auto BORDERKEY   = window->m_group ?
            (ISACTIVE ? (GROUPLOCKED ? "group:col.border_locked_active" : "group:col.border_active") :
                        (GROUPLOCKED ? "group:col.border_locked_inactive" : "group:col.border_inactive")) :
            (ISACTIVE ? (NOGROUP ? "general:col.nogroup_border_active" : "general:col.active_border") :
                        (NOGROUP ? "general:col.nogroup_border" : "general:col.inactive_border"));
        const auto BORDERCOLOR = ScrollOverview::Config::getValue<Config::CGradientValueData>(BORDERKEY);

        window->m_realBorderColor =
            ISACTIVE ? window->m_ruleApplicator->activeBorderColor().valueOr(BORDERCOLOR) : window->m_ruleApplicator->inactiveBorderColor().valueOr(BORDERCOLOR);
        window->m_realBorderColorPrevious = window->m_realBorderColor;

        const auto SHADOWACTIVE   = ScrollOverview::Config::getValue<Config::CGradientValueData>("decoration:shadow:color");
        const auto SHADOWINACTIVE = Config::mgr()->getConfigValue("decoration:shadow:color_inactive").setByUser ?
            ScrollOverview::Config::getValue<Config::CGradientValueData>("decoration:shadow:color_inactive") :
            SHADOWACTIVE;
        const auto GLOWACTIVE     = ScrollOverview::Config::getValue<Config::CGradientValueData>("decoration:glow:color");
        const auto GLOWINACTIVE   = Config::mgr()->getConfigValue("decoration:glow:color_inactive").setByUser ?
            ScrollOverview::Config::getValue<Config::CGradientValueData>("decoration:glow:color_inactive") :
            GLOWACTIVE;

        if (window->isX11OverrideRedirect() || window->m_X11DoesntWantBorders) {
            const Config::CGradientValueData TRANSPARENT{CHyprColor{0, 0, 0, 0}};
            window->m_realShadowColor = TRANSPARENT;
            window->m_realGlowColor   = TRANSPARENT;
        } else {
            window->m_realShadowColor = ISACTIVE ? SHADOWACTIVE : SHADOWINACTIVE;
            window->m_realGlowColor   = ISACTIVE ? GLOWACTIVE : GLOWINACTIVE;
        }
        window->m_realShadowColorPrevious = window->m_realShadowColor;
        window->m_realGlowColorPrevious   = window->m_realGlowColor;

        window->alpha(Desktop::View::WINDOW_ALPHA_ACTIVE)->value() = getOverviewWindowTargetOpacity(window);

        const bool ISMODALSHADOWED = window->m_xdgSurface && window->m_xdgSurface->m_toplevel && window->m_xdgSurface->m_toplevel->anyChildModal();
        float      dim             = ISACTIVE || window->m_ruleApplicator->noDim().valueOrDefault() || !ScrollOverview::Config::getValue<bool>("decoration:dim_inactive") ?
                 0.F :
                 ScrollOverview::Config::getValue<float>("decoration:dim_strength");
        if (ISMODALSHADOWED && ScrollOverview::Config::getValue<bool>("decoration:dim_modal"))
            dim += (1.F - dim) / 2.F;
        window->m_dimPercent->value() = dim;
    }

    ~SOverviewPseudoFocusState() {
        if (!active)
            return;

        window->m_realBorderColor         = previousBorderColor;
        window->m_realBorderColorPrevious = previousBorderColorPrevious;
        window->m_realShadowColor         = previousShadowColor;
        window->m_realShadowColorPrevious = previousShadowColorPrevious;
        window->m_realGlowColor           = previousGlowColor;
        window->m_realGlowColorPrevious   = previousGlowColorPrevious;
        window->alpha(Desktop::View::WINDOW_ALPHA_ACTIVE)->value() = previousOpacity;
        window->m_dimPercent->value()                 = previousDim;
        focusState->m_focusWindow                     = previousFocusWindow;
        focusState->m_focusSurface                    = previousFocusSurface;
    }
};


static void roundStandaloneWindowPassElements(const PHLWINDOW& window, PHLMONITOR monitor, float renderScale, size_t firstElement) {
    if (!window || !monitor)
        return;

    if (Fullscreen::controller()->isFullscreen(window))
        return;

    const int   rounding      = sc<int>(std::round(window->rounding() * monitor->m_scale * renderScale));
    const float roundingPower = window->roundingPower();

    if (rounding <= 0)
        return;

    auto& passElements = g_pHyprRenderer->m_renderPass.m_passElements;
    for (size_t i = firstElement; i < passElements.size(); ++i) {
        const auto& passElement = passElements[i];
        if (!passElement.element)
            continue;

        auto* surfacePassElement = dc<CSurfacePassElement*>(passElement.element.get());
        if (!surfacePassElement || surfacePassElement->m_data.pWindow != window || surfacePassElement->m_data.popup)
            continue;

        surfacePassElement->m_data.dontRound     = false;
        surfacePassElement->m_data.rounding      = rounding;
        surfacePassElement->m_data.roundingPower = roundingPower;
    }
}

static bool isOverviewHyprbarDecoration(IHyprWindowDecoration* decoration) {
    return decoration && decoration->getDecorationType() == DECORATION_CUSTOM && decoration->getDisplayName() == "Hyprbar";
}

static SHyprbarGlobalStateMirror* getOverviewHyprbarGlobalState() {
    if (!g_pPluginSystem)
        return nullptr;

    HANDLE hyprbarsHandle = nullptr;
    for (const auto* plugin : g_pPluginSystem->getAllPlugins()) {
        if (!plugin)
            continue;
        if (plugin->m_name == "hyprbars" || plugin->m_path.contains("hyprbars")) {
            hyprbarsHandle = plugin->m_handle;
            break;
        }
    }

    if (!hyprbarsHandle)
        return nullptr;

    void* const symbol = dlsym(hyprbarsHandle, "g_pGlobalState");
    if (!symbol)
        return nullptr;

    const auto STATEPTR = sc<UP<SHyprbarGlobalStateMirror>*>(symbol);
    if (!STATEPTR || !STATEPTR->get())
        return nullptr;

    return STATEPTR->get();
}

static float getOverviewHyprbarLogicalHeight(const PHLWINDOW& window) {
    if (!window || !window->m_ruleApplicator->decorate().valueOrDefault())
        return 0.F;

    for (const auto& deco : window->m_windowDecorations) {
        if (!isOverviewHyprbarDecoration(deco.get()))
            continue;

        const auto INFO = deco->getPositioningInfo();
        if (INFO.policy != DECORATION_POSITION_STICKY || INFO.edges != DECORATION_EDGE_TOP)
            continue;

        return std::max(0.F, sc<float>(INFO.desiredExtents.topLeft.y));
    }

    return 0.F;
}

static bool shouldOverviewBorderIncludeHyprbar(const PHLWINDOW& window) {
    const float hyprbarHeight = getOverviewHyprbarLogicalHeight(window);
    if (hyprbarHeight <= 0.F)
        return false;

    return ScrollOverview::Config::getValue<bool>("plugin:hyprbars:bar_precedence_over_border");
}

static bool shouldOverviewShadowIncludeHyprbar(const PHLWINDOW& window, bool borderIncludesHyprbar) {
    const float hyprbarHeight = getOverviewHyprbarLogicalHeight(window);
    if (hyprbarHeight <= 0.F)
        return false;

    if (borderIncludesHyprbar)
        return true;

    return ScrollOverview::Config::getValue<bool>("plugin:hyprbars:bar_part_of_window");
}

static void blockOverviewWindowBlurOptimization(const PHLWINDOW& window, size_t firstElement) {
    if (!window)
        return;

    auto& passElements = g_pHyprRenderer->m_renderPass.m_passElements;
    for (size_t i = firstElement; i < passElements.size(); ++i) {
        const auto& passElement = passElements[i];
        if (!passElement.element)
            continue;

        auto* surfacePassElement = dc<CSurfacePassElement*>(passElement.element.get());
        if (!surfacePassElement || surfacePassElement->m_data.pWindow != window)
            continue;

        surfacePassElement->m_data.blockBlurOptimization = true;
    }
}

static void scaleOverviewChildSurfaceGeometry(const PHLWINDOW& window, const Vector2D& targetPosition, const Vector2D& targetSize,
                                              size_t firstElement) {
    if (!window)
        return;

    const auto REPORTEDSIZE = window->getReportedSize();
    if (REPORTEDSIZE.x <= 0 || REPORTEDSIZE.y <= 0)
        return;

    const Vector2D SURFACESCALE = targetSize / REPORTEDSIZE;
    auto&          passElements = g_pHyprRenderer->m_renderPass.m_passElements;

    for (size_t i = firstElement; i < passElements.size(); ++i) {
        const auto& passElement = passElements[i];
        if (!passElement.element)
            continue;

        auto* surfacePassElement = dc<CSurfacePassElement*>(passElement.element.get());
        if (!surfacePassElement || surfacePassElement->m_data.pWindow != window || surfacePassElement->m_data.mainSurface)
            continue;

        if (surfacePassElement->m_data.popup) {
            // Popup positions are relative to the unscaled top-level window,
            // while `pos` already contains the overview window position.
            // Scale that relative part, including the popup's own subsurface
            // offset, around the overview window origin.
            surfacePassElement->m_data.pos = targetPosition + (surfacePassElement->m_data.pos - targetPosition) * SURFACESCALE;
            surfacePassElement->m_data.localPos *= SURFACESCALE;

            // Hyprland deliberately disables squishing for popups, which also
            // disables its resize scaling. Re-enable it for this pass element,
            // but expand the clamp dimensions to the complete scaled surface
            // so the popup remains allowed to extend beyond the main window.
            const auto SURFACESIZE =
                surfacePassElement->m_data.surface->m_current.size.clamp({2.F, 2.F}, {INFINITY, INFINITY}) * SURFACESCALE;
            surfacePassElement->m_data.w                = std::max(1.0, surfacePassElement->m_data.localPos.x + SURFACESIZE.x + 1.0);
            surfacePassElement->m_data.h                = std::max(1.0, surfacePassElement->m_data.localPos.y + SURFACESIZE.y + 1.0);
            surfacePassElement->m_data.squishOversized = true;
            continue;
        }

        // Hyprland scales a regular subsurface texture during a resize, but
        // its cumulative position remains in the client's reported coordinate
        // space. Apply the same scale to the position so embedded dmabuf
        // surfaces (OBS preview, OrcaSlicer viewport, etc.) stay attached.
        surfacePassElement->m_data.localPos *= SURFACESCALE;
    }
}

static void raiseWindowPopups(const PHLWINDOW& window, size_t firstElement) {
    if (!window)
        return;

    auto& passElements = g_pHyprRenderer->m_renderPass.m_passElements;
    if (firstElement >= passElements.size())
        return;

    // renderWindow queues popups after the main surface, but overview adds its
    // own decorations and border afterwards. Keep every non-popup element in
    // its original order and move only this window's popup surfaces on top.
    std::stable_partition(passElements.begin() + firstElement, passElements.end(), [&window](const auto& passElement) {
        if (!passElement.element)
            return true;

        const auto* surfacePassElement = dc<const CSurfacePassElement*>(passElement.element.get());
        return !surfacePassElement || surfacePassElement->m_data.pWindow != window || !surfacePassElement->m_data.popup;
    });
}

static SOverviewWindowMetrics getOverviewWindowMetrics(PHLMONITOR monitor, const PHLWINDOW& window, float renderScale) {
    SOverviewWindowMetrics metrics;
    metrics.renderScale   = renderScale;
    metrics.targetOpacity = getOverviewWindowTargetOpacity(window);

    if (!monitor || !window)
        return metrics;

    metrics.pxScale                 = monitor->m_scale * renderScale;
    metrics.borderSize              = window->getRealBorderSize();
    metrics.borderPx                = sc<int>(std::round(metrics.borderSize * monitor->m_scale));
    metrics.borderPxScaled          = metrics.borderSize * metrics.pxScale;
    metrics.roundingBase            = window->rounding();
    metrics.roundingPower           = window->roundingPower();
    metrics.correctionOffset        = metrics.borderSize * (M_SQRT2 - 1) * std::max(2.0 - metrics.roundingPower, 0.0);
    metrics.outerRound              = metrics.roundingBase > 0 ? (metrics.roundingBase + metrics.borderSize) - metrics.correctionOffset : 0.F;
    metrics.roundingPx              = sc<int>(std::round(metrics.roundingBase * metrics.pxScale));
    metrics.outerRoundPx            = sc<int>(std::round(metrics.outerRound * metrics.pxScale));
    metrics.hyprbarLogicalHeight    = getOverviewHyprbarLogicalHeight(window);
    metrics.borderIncludesHyprbar   = shouldOverviewBorderIncludeHyprbar(window);
    metrics.shadowIncludesHyprbar   = shouldOverviewShadowIncludeHyprbar(window, metrics.borderIncludesHyprbar);
    metrics.hyprbarHeightPx         = sc<int>(std::round(metrics.hyprbarLogicalHeight * metrics.pxScale));
    metrics.hyprbarHeightPxScaled   = metrics.hyprbarLogicalHeight * metrics.pxScale;
    metrics.hyprbarTopOffsetLogical = metrics.hyprbarLogicalHeight > 0.F ? metrics.hyprbarLogicalHeight + metrics.borderSize : 0.F;

    return metrics;
}

static std::optional<SDecorationPositioningReply> getOverviewTopStickyDecorationReply(IHyprWindowDecoration* decoration, const PHLWINDOW& window, float heightScale = 1.F) {
    if (!decoration || !window)
        return std::nullopt;

    const auto INFO = decoration->getPositioningInfo();
    if (INFO.policy != DECORATION_POSITION_STICKY || INFO.edges != DECORATION_EDGE_TOP)
        return std::nullopt;

    const float HEIGHT  = std::max(0.F, sc<float>(INFO.desiredExtents.topLeft.y) * heightScale);
    const float BORDER  = sc<float>(window->getRealBorderSize());
    const bool  PRECEDE = shouldOverviewBorderIncludeHyprbar(window);
    const float WIDTH   = PRECEDE ? window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x : window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x + BORDER * 2.F;
    const float YOFFSET = PRECEDE ? 0.F : BORDER;

    return SDecorationPositioningReply{
        .assignedGeometry = CBox{{-WIDTH / 2.F, -HEIGHT - YOFFSET}, {WIDTH, HEIGHT}},
    };
}

static CBox getOverviewOuterBorderBox(const CBox& windowBox, const SOverviewWindowMetrics& metrics) {
    CBox box = windowBox.copy().expand(metrics.borderPx);
    if (metrics.shadowIncludesHyprbar && metrics.hyprbarHeightPx > 0) {
        box.y -= metrics.hyprbarHeightPx;
        box.height += metrics.hyprbarHeightPx;
    }

    return box;
}

static CBox getOverviewBorderBox(const CBox& windowBox, const SOverviewWindowMetrics& metrics) {
    CBox box = windowBox;
    if (metrics.borderIncludesHyprbar && metrics.hyprbarHeightPx > 0) {
        box.y -= metrics.hyprbarHeightPx;
        box.height += metrics.hyprbarHeightPx;
    }

    return box;
}

static void renderOverviewHyprbarDecoration(SOverviewCustomDecorationRenderState& state, PHLMONITOR monitor, const PHLWINDOW& window, IHyprWindowDecoration* decoration,
                                            const CBox& windowBox, const SOverviewWindowMetrics& metrics) {
    if (!monitor || !window || !decoration)
        return;

    auto* const HYPRBARGLOBALSTATE    = getOverviewHyprbarGlobalState();
    const bool   PARTOFWINDOW         = decoration->getDecorationFlags() & DECORATION_PART_OF_MAIN_WINDOW;
    const int   previousBarTextSize   = ScrollOverview::Config::getValue<int>("plugin:hyprbars:bar_text_size");
    const int   previousButtonPadding = ScrollOverview::Config::getValue<int>("plugin:hyprbars:bar_button_padding");
    std::vector<float> previousButtonSizes;
    if (HYPRBARGLOBALSTATE) {
        previousButtonSizes.reserve(HYPRBARGLOBALSTATE->buttons.size());
        for (auto& button : HYPRBARGLOBALSTATE->buttons) {
            previousButtonSizes.push_back(button.size);
            button.size *= metrics.renderScale;
        }
    }
    ScrollOverview::Config::setValue("plugin:hyprbars:bar_text_size", std::max(1, sc<int>(std::round(previousBarTextSize * metrics.renderScale))));
    ScrollOverview::Config::setValue("plugin:hyprbars:bar_button_padding", std::max(0, sc<int>(std::round(previousButtonPadding * metrics.renderScale))));

    const Vector2D previousWindowPos       = window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const Vector2D previousWindowSize      = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto     WORKSPACE               = window->m_workspace;
    const bool     OVERRIDEWORKSPACEOFFSET = WORKSPACE && !window->m_pinned;
    const Vector2D previousWorkspaceOffset = OVERRIDEWORKSPACEOFFSET ? WORKSPACE->m_renderOffset->value() : Vector2D{};
    const auto     previousReplyData       = g_pDecorationPositioner->getDataFor(decoration, window);
    const auto     previousReply           = previousReplyData ? previousReplyData->lastReply : SDecorationPositioningReply{};
    const auto     previousRounding        = window->m_ruleApplicator->rounding();
    const auto     previousBorderSize      = window->m_ruleApplicator->borderSize();
    const bool     previousBorderCacheDirty = window->m_borderSizeCacheDirty;
    const int      previousCachedBorderSize = window->m_cachedBorderSize;

    const auto scaledRounding   = std::max<Config::INTEGER>(0, sc<Config::INTEGER>(std::round(window->m_ruleApplicator->rounding().valueOrDefault() * metrics.renderScale)));
    const auto scaledBorderSize = std::max<Config::INTEGER>(0, sc<Config::INTEGER>(std::round(window->getRealBorderSize() * metrics.renderScale)));
    window->m_ruleApplicator->rounding().set(scaledRounding, Desktop::Types::PRIORITY_SET_PROP);
    window->m_ruleApplicator->borderSize().set(scaledBorderSize, Desktop::Types::PRIORITY_SET_PROP);
    window->m_borderSizeCacheDirty = true;

    window->positionAnimation()->value() = monitor->m_position + windowBox.pos() / monitor->m_scale - window->m_floatingOffset;
    window->sizeAnimation()->value()     = windowBox.size() / monitor->m_scale;

    const auto REPLY = getOverviewTopStickyDecorationReply(decoration, window, metrics.renderScale);
    if (!REPLY) {
        window->positionAnimation()->value() = previousWindowPos;
        window->sizeAnimation()->value()     = previousWindowSize;
        ScrollOverview::Config::setValue("plugin:hyprbars:bar_text_size", previousBarTextSize);
        ScrollOverview::Config::setValue("plugin:hyprbars:bar_button_padding", previousButtonPadding);
        window->m_ruleApplicator->roundingOverride(previousRounding);
        window->m_ruleApplicator->borderSizeOverride(previousBorderSize);
        window->m_borderSizeCacheDirty = previousBorderCacheDirty;
        window->m_cachedBorderSize     = previousCachedBorderSize;
        if (HYPRBARGLOBALSTATE) {
            for (size_t i = 0; i < previousButtonSizes.size() && i < HYPRBARGLOBALSTATE->buttons.size(); ++i)
                HYPRBARGLOBALSTATE->buttons[i].size = previousButtonSizes[i];
        }
        return;
    }

    if (OVERRIDEWORKSPACEOFFSET)
        WORKSPACE->m_renderOffset->value() = {};

    if (previousReplyData)
        previousReplyData->lastReply = *REPLY;
    decoration->onPositioningReply(*REPLY);

    decoration->draw(monitor, PARTOFWINDOW ? 1.F : metrics.targetOpacity);

    state.queuedAny = true;
    state.restoreFns.emplace_back([window, decoration, WORKSPACE, OVERRIDEWORKSPACEOFFSET, previousWindowPos, previousWindowSize, previousWorkspaceOffset, previousReply,
                                   previousReplyData, previousBarTextSize, previousButtonPadding, previousButtonSizes, previousRounding, previousBorderSize,
                                   previousBorderCacheDirty, previousCachedBorderSize] {
        if (!window || !decoration)
            return;

        window->positionAnimation()->value() = previousWindowPos;
        window->sizeAnimation()->value()     = previousWindowSize;
        if (OVERRIDEWORKSPACEOFFSET && WORKSPACE)
            WORKSPACE->m_renderOffset->value() = previousWorkspaceOffset;

        if (previousReplyData)
            previousReplyData->lastReply = previousReply;
        decoration->onPositioningReply(previousReply);

        ScrollOverview::Config::setValue("plugin:hyprbars:bar_text_size", previousBarTextSize);
        ScrollOverview::Config::setValue("plugin:hyprbars:bar_button_padding", previousButtonPadding);
        window->m_ruleApplicator->roundingOverride(previousRounding);
        window->m_ruleApplicator->borderSizeOverride(previousBorderSize);
        window->m_borderSizeCacheDirty = previousBorderCacheDirty;
        window->m_cachedBorderSize     = previousCachedBorderSize;
        if (auto* const HYPRBARGLOBALSTATE = getOverviewHyprbarGlobalState()) {
            for (size_t i = 0; i < previousButtonSizes.size() && i < HYPRBARGLOBALSTATE->buttons.size(); ++i)
                HYPRBARGLOBALSTATE->buttons[i].size = previousButtonSizes[i];
        }
    });
}

static void renderOverviewWindowShadow(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, bool selected) {
    if (!monitor || !window || (!window->m_isMapped))
        return;

    const auto PSHADOWS      = ScrollOverview::Config::getValue<int>("decoration:shadow:enabled");
    const auto PSHADOWSIZE   = ScrollOverview::Config::getValue<int>("decoration:shadow:range");
    const auto PSHADOWSCALE  = ScrollOverview::Config::getValue<float>("decoration:shadow:scale");
    const auto PSHADOWOFFSET = ScrollOverview::Config::getValue<Hyprlang::VEC2>("decoration:shadow:offset");

    if (PSHADOWS != 1 || PSHADOWSIZE <= 0)
        return;

    if (window->isX11OverrideRedirect() || window->m_X11DoesntWantBorders || !window->m_ruleApplicator->decorate().valueOrDefault() ||
        window->m_ruleApplicator->noShadow().valueOrDefault())
        return;

    const int   rangePx          = sc<int>(std::round(PSHADOWSIZE * monitor->m_scale * metrics.renderScale));
    const float shadowScale      = std::clamp(PSHADOWSCALE, 0.F, 1.F);
    const auto  shadowOffset     = Vector2D{PSHADOWOFFSET.x, PSHADOWOFFSET.y} * monitor->m_scale * metrics.renderScale;

    if (rangePx <= 0)
        return;

    CBox shadowBaseBox = getOverviewOuterBorderBox(windowBox, metrics);
    CBox cutoutBox     = windowBox;
    CBox shadowBox     = shadowBaseBox.copy().expand(rangePx).scaleFromCenter(shadowScale).translate(shadowOffset);
    shadowBox.round();

    if (shadowBox.width < 1 || shadowBox.height < 1)
        return;

    const auto& shadowColor = window->m_realShadowColor;
    if (std::ranges::none_of(shadowColor.m_colors, [](const CHyprColor& color) { return color.a > 0.F; }))
        return;

    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewShadowPassElement>(COverviewShadowPassElement::SData{
        .monitor       = monitor,
        .fullBox       = shadowBox,
        .cutoutBox     = cutoutBox.round(),
        .rounding      = metrics.outerRoundPx,
        .roundingPower = metrics.roundingPower,
        .range         = rangePx,
        .renderPower   = ScrollOverview::Config::getValue<int>("decoration:shadow:render_power"),
        .color         = shadowColor,
        .alpha         = metrics.targetOpacity,
        .ignoreWindow  = true,
    }));
}

static void renderOverviewWindowBorder(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics) {
    if (!monitor || !window || (!window->m_isMapped))
        return;

    if (metrics.borderSize <= 0.F)
        return;

    auto grad = window->m_realBorderColor;

    const CBox borderBox = getOverviewBorderBox(windowBox, metrics);

    CBorderPassElement::SBorderData data;
    data.box           = borderBox;
    data.grad1         = grad;
    data.round         = metrics.roundingPx;
    data.outerRound    = metrics.outerRoundPx;
    data.roundingPower = metrics.roundingPower;
    data.a             = metrics.targetOpacity;
    data.borderSize    = metrics.borderSize;
    if (window->m_borderFadeAnimationProgress->isBeingAnimated()) {
        data.hasGrad2 = true;
        data.grad1    = window->m_realBorderColorPrevious;
        data.grad2    = grad;
        data.lerp     = window->m_borderFadeAnimationProgress->value();
    } else
        data.grad1 = grad;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(data));
}

static void renderOverviewWindowTitle(const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics) {
    if (!window || !ScrollOverview::Config::getValue<bool>("plugin:scrolloverview:title:enabled"))
        return;

    const std::string& title = window->m_title.empty() ? window->m_class : window->m_title;
    if (title.empty())
        return;

    const float scale      = std::max(0.1F, metrics.pxScale);
    const float padding    = std::round(6.F * scale);
    const float titleHeight = std::round(24.F * scale);
    CBox        titleBox   = {windowBox.x + padding, windowBox.y + padding, windowBox.width - padding * 2.F, titleHeight};
    if (titleBox.empty())
        return;

    CHyprColor background = CHyprColor(sc<uint64_t>(ScrollOverview::Config::getValue<int>("plugin:scrolloverview:title:background_color")));
    background.a *= metrics.targetOpacity;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(CRectPassElement::SRectData{
        .box           = titleBox,
        .color         = background,
        .round         = std::max(0, sc<int>(std::round(4.F * scale))),
        .roundingPower = 2.F,
    }));

    CHyprColor textColor = CHyprColor(sc<uint64_t>(ScrollOverview::Config::getValue<int>("plugin:scrolloverview:title:text_color")));
    textColor.a *= metrics.targetOpacity;
    const int maxWidth = std::max(1, sc<int>(std::round(titleBox.width - padding * 2.F)));
    auto      texture  = g_pHyprRenderer->renderText(title, textColor,
        std::max(1, sc<int>(std::round(ScrollOverview::Config::getValue<int>("plugin:scrolloverview:title:font_size") * scale))), false,
        ScrollOverview::Config::getValue<std::string>("misc:font_family"), maxWidth, 700);
    if (!texture || !texture->ok())
        return;

    CBox textBox = titleBox;
    textBox.x += (titleBox.width - texture->m_size.x) / 2.F;
    textBox.y += (titleBox.height - texture->m_size.y) / 2.F;
    textBox.width  = texture->m_size.x;
    textBox.height = texture->m_size.y;
    textBox.round();
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(CTexPassElement::SRenderData{.tex = texture, .box = textBox, .a = 1.F}));
}

static void renderOverviewGroupTabIndicators(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, float alpha) {
    if (!monitor || !window || !window->m_group || window->m_group->size() < 1)
        return;

    const auto PINDICATORHEIGHT = ScrollOverview::Config::getValue<int>("group:groupbar:indicator_height");
    const auto PINDICATORGAP    = ScrollOverview::Config::getValue<int>("group:groupbar:indicator_gap");
    const auto PHEIGHT          = ScrollOverview::Config::getValue<int>("group:groupbar:height");
    const auto PGRADIENTS       = ScrollOverview::Config::getValue<bool>("group:groupbar:gradients");
    const auto PRENDERTITLES    = ScrollOverview::Config::getValue<bool>("group:groupbar:render_titles");
    const auto PSTACKED         = ScrollOverview::Config::getValue<bool>("group:groupbar:stacked");
    const auto PROUNDING        = ScrollOverview::Config::getValue<int>("group:groupbar:rounding");
    const auto PROUNDINGPOWER   = ScrollOverview::Config::getValue<float>("group:groupbar:rounding_power");
    const auto POUTERGAP        = ScrollOverview::Config::getValue<int>("group:groupbar:gaps_out");
    const auto PINNERGAP        = ScrollOverview::Config::getValue<int>("group:groupbar:gaps_in");

    if (PINDICATORHEIGHT <= 0)
        return;

    const bool  groupLocked  = window->m_group->locked() || g_pKeybindManager->m_groupsLocked;
    const auto  groupWindows = window->m_group->windows();
    const auto  focusedWindow = Desktop::focusState()->window();
    const float indicatorH   = sc<float>(PINDICATORHEIGHT) * metrics.pxScale;
    const float outerGap     = sc<float>(POUTERGAP) * metrics.pxScale;
    const float innerGap     = sc<float>(PINNERGAP) * metrics.pxScale;
    const float oneBarHeight = sc<float>(POUTERGAP + PINDICATORHEIGHT + PINDICATORGAP + ((PGRADIENTS || PRENDERTITLES) ? PHEIGHT : 0)) * metrics.pxScale;
    const int   rounding     = sc<int>(std::round(PROUNDING * metrics.pxScale));
    CBox        indicatorArea = windowBox.copy().expand(metrics.borderPxScaled);
    indicatorArea.y -= metrics.hyprbarHeightPxScaled;
    auto* const GROUPCOLACTIVE         = sc<Config::CGradientValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:col.active").ptr());
    auto* const GROUPCOLINACTIVE       = sc<Config::CGradientValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:col.inactive").ptr());
    auto* const GROUPCOLACTIVELOCKED   = sc<Config::CGradientValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:col.locked_active").ptr());
    auto* const GROUPCOLINACTIVELOCKED = sc<Config::CGradientValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:col.locked_inactive").ptr());
    const auto* const COLACTIVE        = groupLocked ? GROUPCOLACTIVELOCKED : GROUPCOLACTIVE;
    const auto* const COLINACTIVE      = groupLocked ? GROUPCOLINACTIVELOCKED : GROUPCOLINACTIVE;

    float xoff = 0.F;
    float yoff = 0.F;

    for (size_t i = 0; i < groupWindows.size(); ++i) {
        const size_t windowIdx = PSTACKED ? groupWindows.size() - i - 1 : i;
        const auto   member    = groupWindows[windowIdx].lock();
        if (!member)
            continue;

        CHyprColor color = member == focusedWindow ? COLACTIVE->m_colors[0] : COLINACTIVE->m_colors[0];

        color.a *= alpha;
        if (color.a <= 0.F)
            continue;

        CBox box;
        if (PSTACKED) {
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
        data.roundingPower = PROUNDINGPOWER;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
    }
}

static void renderOverviewGroupTabTitles(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, float alpha) {
    if (!monitor || !window || !window->m_group || window->m_group->size() < 1 || alpha <= 0.F)
        return;

    const auto PRENDERTITLES            = ScrollOverview::Config::getValue<bool>("group:groupbar:render_titles");
    const auto PTITLEFONTSIZE           = ScrollOverview::Config::getValue<int>("group:groupbar:font_size");
    const auto PHEIGHT                  = ScrollOverview::Config::getValue<int>("group:groupbar:height");
    const auto PINDICATORGAP            = ScrollOverview::Config::getValue<int>("group:groupbar:indicator_gap");
    const auto PINDICATORHEIGHT         = ScrollOverview::Config::getValue<int>("group:groupbar:indicator_height");
    const auto PSTACKED                 = ScrollOverview::Config::getValue<bool>("group:groupbar:stacked");
    const auto POUTERGAP                = ScrollOverview::Config::getValue<int>("group:groupbar:gaps_out");
    const auto PINNERGAP                = ScrollOverview::Config::getValue<int>("group:groupbar:gaps_in");
    const auto PTEXTOFFSET              = ScrollOverview::Config::getValue<int>("group:groupbar:text_offset");
    const auto PTEXTPADDING             = ScrollOverview::Config::getValue<int>("group:groupbar:text_padding");
    const auto PTEXTCOLORACTIVE         = ScrollOverview::Config::getValue<int>("group:groupbar:text_color");
    const auto PTEXTCOLORINACTIVE       = ScrollOverview::Config::getValue<int>("group:groupbar:text_color_inactive");
    const auto PTEXTCOLORLOCKEDACTIVE   = ScrollOverview::Config::getValue<int>("group:groupbar:text_color_locked_active");
    const auto PTEXTCOLORLOCKEDINACTIVE = ScrollOverview::Config::getValue<int>("group:groupbar:text_color_locked_inactive");

    if (!PRENDERTITLES || PHEIGHT <= 0)
        return;

    const bool  groupLocked  = window->m_group->locked() || g_pKeybindManager->m_groupsLocked;
    const auto  groupWindows = window->m_group->windows();
    const auto  focusedWindow = Desktop::focusState()->window();
    const float outerGap     = sc<float>(POUTERGAP) * metrics.pxScale;
    const float innerGap     = sc<float>(PINNERGAP) * metrics.pxScale;
    const float oneBarHeight = sc<float>(POUTERGAP + PINDICATORHEIGHT + PINDICATORGAP + PHEIGHT) * metrics.pxScale;
    const float titleHeight  = sc<float>(PHEIGHT) * metrics.pxScale;
    const float textPadding  = sc<float>(PTEXTPADDING) * metrics.pxScale;
    const float textOffset   = sc<float>(PTEXTOFFSET) * metrics.pxScale;
    const int   fontSizePx   = std::max(1, sc<int>(std::round(PTITLEFONTSIZE * metrics.pxScale)));

    CBox        titleArea = windowBox.copy().expand(metrics.borderPxScaled);
    titleArea.y -= metrics.hyprbarHeightPxScaled;

    const CHyprColor COLORACTIVE         = CHyprColor(PTEXTCOLORACTIVE);
    const CHyprColor COLORINACTIVE       = PTEXTCOLORINACTIVE == -1 ? COLORACTIVE : CHyprColor(PTEXTCOLORINACTIVE);
    const CHyprColor COLORLOCKEDACTIVE   = PTEXTCOLORLOCKEDACTIVE == -1 ? COLORACTIVE : CHyprColor(PTEXTCOLORLOCKEDACTIVE);
    const CHyprColor COLORLOCKEDINACTIVE = PTEXTCOLORLOCKEDINACTIVE == -1 ? COLORINACTIVE : CHyprColor(PTEXTCOLORLOCKEDINACTIVE);
    const auto       FONTWEIGHTACTIVE    = sc<Config::CFontWeightConfigValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:font_weight_active").ptr());
    const auto       FONTWEIGHTINACTIVE  = sc<Config::CFontWeightConfigValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:font_weight_inactive").ptr());
    const auto       TITLEFONTFAMILY     = ScrollOverview::Config::getValue<std::string>("group:groupbar:font_family");
    const auto       FONTFAMILY          = TITLEFONTFAMILY != STRVAL_EMPTY ? TITLEFONTFAMILY : ScrollOverview::Config::getValue<std::string>("misc:font_family");

    float xoff = 0.F;
    float yoff = 0.F;

    for (size_t i = 0; i < groupWindows.size(); ++i) {
        const size_t windowIdx = PSTACKED ? groupWindows.size() - i - 1 : i;
        const auto   member    = groupWindows[windowIdx].lock();
        if (!member)
            continue;

        const float barWidth = PSTACKED ? titleArea.width : (titleArea.width - innerGap * (groupWindows.size() - 1)) / groupWindows.size();

        CBox box;
        if (PSTACKED) {
            box = {titleArea.x, titleArea.y - yoff - outerGap - titleHeight - sc<float>(PINDICATORHEIGHT) * metrics.pxScale - sc<float>(PINDICATORGAP) * metrics.pxScale,
                   titleArea.width, titleHeight};
            yoff += oneBarHeight;
        } else {
            box = {titleArea.x + xoff, titleArea.y - outerGap - titleHeight - sc<float>(PINDICATORHEIGHT) * metrics.pxScale - sc<float>(PINDICATORGAP) * metrics.pxScale,
                   barWidth, titleHeight};
            xoff += innerGap + barWidth;
        }

        box.round();
        if (box.empty())
            continue;

        CHyprColor textColor;
        const bool isCurrent = member == focusedWindow;
        if (groupLocked)
            textColor = isCurrent ? COLORLOCKEDACTIVE : COLORLOCKEDINACTIVE;
        else
            textColor = isCurrent ? COLORACTIVE : COLORINACTIVE;

        textColor.a *= alpha;
        if (textColor.a <= 0.F)
            continue;

        const int maxWidth = std::max(1, sc<int>(std::round(box.width - textPadding * 2.F)));
        auto      titleTex =
            g_pHyprRenderer->renderText(member->m_title, textColor, fontSizePx, false, FONTFAMILY, maxWidth, isCurrent ? FONTWEIGHTACTIVE->m_value : FONTWEIGHTINACTIVE->m_value);
        if (!titleTex || !titleTex->ok())
            continue;

        CBox textBox = box;
        textBox.y += std::ceil((box.height - titleTex->m_size.y) / 2.0 - textOffset);
        textBox.height = titleTex->m_size.y;
        textBox.width  = titleTex->m_size.x;
        textBox.x += std::round((box.width + textPadding) / 2.0 - (titleTex->m_size.x + textPadding) / 2.0);
        textBox.round();

        CTexPassElement::SRenderData data;
        data.tex = titleTex;
        data.box = textBox;
        data.a   = 1.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
    }
}

static void renderOverviewGroupTabs(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const CBox& workspaceBox,
                                    const SOverviewWindowMetrics& metrics) {
    if (!monitor || !window || !window->m_group || window->m_group->size() < 1)
        return;

    auto* const GROUPBAR = dc<CHyprGroupBarDecoration*>(window->getDecorationByType(DECORATION_GROUPBAR));
    if (!GROUPBAR)
        return;

    const auto  PHEIGHT          = ScrollOverview::Config::getValue<int>("group:groupbar:height");
    const auto  PINDICATORGAP    = ScrollOverview::Config::getValue<int>("group:groupbar:indicator_gap");
    const auto  PINDICATORHEIGHT = ScrollOverview::Config::getValue<int>("group:groupbar:indicator_height");
    const auto  PRENDERTITLES    = ScrollOverview::Config::getValue<bool>("group:groupbar:render_titles");
    const auto  PGRADIENTS       = ScrollOverview::Config::getValue<bool>("group:groupbar:gradients");
    const auto  PSTACKED         = ScrollOverview::Config::getValue<bool>("group:groupbar:stacked");
    const auto  POUTERGAP        = ScrollOverview::Config::getValue<int>("group:groupbar:gaps_out");
    const auto  PKEEPUPPERGAP    = ScrollOverview::Config::getValue<int>("group:groupbar:keep_upper_gap");

    const auto  ONEBARHEIGHT     = POUTERGAP + PINDICATORHEIGHT + PINDICATORGAP + (PGRADIENTS || PRENDERTITLES ? PHEIGHT : 0);
    const auto  DESIREDHEIGHT    = PSTACKED ? (ONEBARHEIGHT * window->m_group->size()) + POUTERGAP * PKEEPUPPERGAP : POUTERGAP * (1 + PKEEPUPPERGAP) + ONEBARHEIGHT;
    const auto  EDGEPOINT        = g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, window);
    CBox        assignedBox      = {window->m_realPosition->value() - Vector2D{0.0, sc<double>(DESIREDHEIGHT) + metrics.hyprbarTopOffsetLogical},
                                    Vector2D{window->m_realSize->value().x, sc<double>(DESIREDHEIGHT)}};
    assignedBox.translate(-EDGEPOINT);

    if (window->m_workspace && !window->m_pinned)
        assignedBox.translate(-window->m_workspace->m_renderOffset->value());

    const auto PREVASSIGNEDBOX = GROUPBAR->m_assignedBox;
    GROUPBAR->m_assignedBox    = assignedBox;
    auto restoreAssignedBox    = Hyprutils::Utils::CScopeGuard([GROUPBAR, PREVASSIGNEDBOX] { GROUPBAR->m_assignedBox = PREVASSIGNEDBOX; });

    Render::SRenderModifData modif;
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, metrics.renderScale);
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, workspaceBox.pos());

    GROUPBAR->updateWindow(window);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
    GROUPBAR->draw(monitor, metrics.targetOpacity);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));
    renderOverviewGroupTabIndicators(monitor, window, windowBox, metrics, metrics.targetOpacity);
}

static SOverviewCustomDecorationRenderState renderOverviewCustomDecorations(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& workspaceBox, const CBox& windowBox,
                                                                           const SOverviewWindowMetrics& metrics, eDecorationLayer layer) {
    SOverviewCustomDecorationRenderState state;

    if (!monitor || !window)
        return state;

    window->updateWindowDecos();

    Render::SRenderModifData modif;
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, metrics.renderScale);
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, workspaceBox.pos());
    bool        drewAny = false;

    for (const auto& deco : window->m_windowDecorations) {
        if (!deco || deco->getDecorationType() != DECORATION_CUSTOM || deco->getDecorationLayer() != layer)
            continue;

        if (layer == DECORATION_LAYER_UNDER && isOverviewHyprbarDecoration(deco.get())) {
            renderOverviewHyprbarDecoration(state, monitor, window, deco.get(), windowBox, metrics);
            continue;
        }

        if (!drewAny) {
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
            drewAny = true;
        }

        deco->updateWindow(window);
        deco->draw(monitor, metrics.targetOpacity);
        state.queuedAny = true;
    }

    if (drewAny)
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));

    return state;
}

} // namespace

bool shouldBlurBackground(const PHLWINDOW& window) {
    return window && g_pHyprRenderer && g_pHyprRenderer->shouldBlur(window);
}

static bool overviewBoxContains(const CBox& outer, const CBox& inner) {
    constexpr float EPSILON = 0.5F;
    return inner.x >= outer.x - EPSILON && inner.y >= outer.y - EPSILON && inner.x + inner.width <= outer.x + outer.width + EPSILON &&
        inner.y + inner.height <= outer.y + outer.height + EPSILON;
}

static bool overviewWindowFitsWorkspaceBox(const CBox* workspaceBox, const CBox* windowBox) {
    return !workspaceBox || !windowBox || overviewBoxContains(*workspaceBox, *windowBox);
}

static bool overviewWorkspaceBoxReadyForPrecomputedBlur(PHLMONITOR monitor, const CBox* workspaceBox) {
    if (!monitor || !workspaceBox)
        return true;

    constexpr float EPSILON    = 0.5F;
    const auto      RENDERSIZE = monitor->m_size * monitor->m_scale;

    return workspaceBox->x >= -EPSILON && workspaceBox->y >= -EPSILON && workspaceBox->x + workspaceBox->width <= RENDERSIZE.x + EPSILON &&
        workspaceBox->y + workspaceBox->height <= RENDERSIZE.y + EPSILON;
}

bool shouldUsePrecomputedBlur(const PHLWINDOW& window, PHLMONITOR monitor, const CBox* workspaceBox, const CBox* windowBox, bool dragged) {
    return !dragged && ScrollOverview::Config::getValue<bool>("decoration:blur:new_optimizations") && shouldShowOverviewWindow(window) && !window->m_isFloating && shouldBlurBackground(window) &&
        overviewWindowFitsWorkspaceBox(workspaceBox, windowBox) && overviewWorkspaceBoxReadyForPrecomputedBlur(monitor, workspaceBox);
}

bool shouldUseBlurFramebuffer(const PHLWINDOW& window, PHLMONITOR monitor, const CBox* workspaceBox, const CBox* windowBox) {
    return shouldUsePrecomputedBlur(window, monitor, workspaceBox, windowBox) ||
        (shouldShowOverviewWindow(window) && shouldBlurBackground(window) && window->m_ruleApplicator->xray().valueOr(false) &&
         overviewWindowFitsWorkspaceBox(workspaceBox, windowBox) && overviewWorkspaceBoxReadyForPrecomputedBlur(monitor, workspaceBox));
}

void forceDecoRecalc(const PHLWINDOW& window) {
    const auto WINDOW = getOverviewWindowToShow(window);
    if (!validMapped(WINDOW))
        return;

    g_pDecorationPositioner->forceRecalcFor(WINDOW);
    WINDOW->updateWindowDecos();
}

void renderOverviewWindow(const SRenderParams& params) {
    if (!params.window)
        return;

    const SOverviewPseudoFocusState pseudoFocusState{params.window, params.pseudoFocusWindow};
    const bool                   fullscreen   = Fullscreen::controller()->isFullscreen(params.window);
    const SOverviewWindowMetrics metrics      = getOverviewWindowMetrics(params.monitor, params.window, params.renderScale);

    if (!fullscreen)
        renderOverviewWindowShadow(params.monitor, params.window, params.windowBox, metrics, params.selected);

    const auto underDecos =
        renderOverviewCustomDecorations(params.monitor, params.window, params.workspaceBox ? *params.workspaceBox : CBox{}, params.windowBox, metrics, DECORATION_LAYER_UNDER);
    if (underDecos.queuedAny) {
        OverviewRender::flushPass(params.monitor);
        for (auto it = underDecos.restoreFns.rbegin(); it != underDecos.restoreFns.rend(); ++it)
            (*it)();
    }

    const Vector2D previousWindowPos       = params.window->m_realPosition->value();
    const Vector2D previousWindowSize      = params.window->m_realSize->value();
    const bool     previousAnimatingIn     = params.window->m_animatingIn;
    const bool     previousSizeAnimating   = params.window->sizeAnimation()->isBeingAnimated();
    const auto     previousBorderSize      = params.window->m_ruleApplicator->borderSize();
    const bool     previousBorderCacheDirty = params.window->m_borderSizeCacheDirty;
    const int      previousCachedBorderSize = params.window->m_cachedBorderSize;
    const auto     WORKSPACE               = params.window->m_workspace;
    const bool     OVERRIDEWORKSPACEOFFSET = WORKSPACE && !params.window->m_pinned;
    const Vector2D previousWorkspaceOffset = OVERRIDEWORKSPACEOFFSET ? WORKSPACE->m_renderOffset->value() : Vector2D{};

    params.window->positionAnimation()->value() = params.monitor->m_position + params.windowBox.pos() / params.monitor->m_scale - params.window->m_floatingOffset;
    params.window->sizeAnimation()->value()     = params.windowBox.size() / params.monitor->m_scale;
    params.window->m_animatingIn                 = true;
    params.window->m_ruleApplicator->borderSize().set(0, Desktop::Types::PRIORITY_SET_PROP);
    params.window->m_borderSizeCacheDirty = true;
    COverviewAnimatedVariableAccess::setBeingAnimated(params.window->sizeAnimation().get(), true);
    if (OVERRIDEWORKSPACEOFFSET)
        WORKSPACE->m_renderOffset->value() = {};

    auto restoreWindowGeometry = Hyprutils::Utils::CScopeGuard([&] {
        params.window->positionAnimation()->value() = previousWindowPos;
        params.window->sizeAnimation()->value()     = previousWindowSize;
        params.window->m_animatingIn                 = previousAnimatingIn;
        params.window->m_ruleApplicator->borderSizeOverride(previousBorderSize);
        params.window->m_borderSizeCacheDirty = previousBorderCacheDirty;
        params.window->m_cachedBorderSize     = previousCachedBorderSize;
        COverviewAnimatedVariableAccess::setBeingAnimated(params.window->sizeAnimation().get(), previousSizeAnimating);
        if (OVERRIDEWORKSPACEOFFSET && WORKSPACE)
            WORKSPACE->m_renderOffset->value() = previousWorkspaceOffset;
    });

    const size_t firstWindowPassElement = g_pHyprRenderer->m_renderPass.m_passElements.size();
    const bool   usePrecomputedBlur     = shouldUsePrecomputedBlur(params.window, params.monitor, params.workspaceBox, &params.windowBox, params.dragged);
    g_pHyprRenderer->renderWindow(params.window, params.monitor, params.now, false, Render::RENDER_PASS_ALL, false, false);
    const Vector2D targetWindowPosition = params.monitor->m_position + params.windowBox.pos() / params.monitor->m_scale;
    scaleOverviewChildSurfaceGeometry(params.window, targetWindowPosition, params.window->sizeAnimation()->value(), firstWindowPassElement);
    if (!usePrecomputedBlur)
        blockOverviewWindowBlurOptimization(params.window, firstWindowPassElement);
    roundStandaloneWindowPassElements(params.window, params.monitor, params.renderScale, firstWindowPassElement);

    renderOverviewCustomDecorations(params.monitor, params.window, params.workspaceBox ? *params.workspaceBox : CBox{}, params.windowBox, metrics, DECORATION_LAYER_OVER);
    renderOverviewCustomDecorations(params.monitor, params.window, params.workspaceBox ? *params.workspaceBox : CBox{}, params.windowBox, metrics, DECORATION_LAYER_OVERLAY);

    if (!fullscreen)
        renderOverviewGroupTabIndicators(params.monitor, params.window, params.windowBox, metrics, metrics.targetOpacity);
    if (!fullscreen)
        renderOverviewGroupTabTitles(params.monitor, params.window, params.windowBox, metrics, metrics.targetOpacity);

    if (!fullscreen)
        renderOverviewWindowBorder(params.monitor, params.window, params.windowBox, metrics);

    renderOverviewWindowTitle(params.window, params.windowBox, metrics);

    raiseWindowPopups(params.window, firstWindowPassElement);
    OverviewRender::flushPass(params.monitor);
}

} // namespace OverviewWindow
