#include "Window.hpp"
#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <functional>
#define private public
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
#include <hyprland/src/render/decorations/CHyprGroupBarDecoration.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#undef private
#include "OverviewPassElement.hpp"
#include "OverviewRender.hpp"
#include "RendererAccess.hpp"

using Render::SRenderModifData;
using enum Render::eRenderPassMode;

namespace OverviewWindow {
namespace {

static bool getHyprlandBlurNewOptimizations() {
    static auto* const* PNEWOPTIMIZATIONS = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(SCROLLOVERVIEW_HANDLE, "decoration:blur:new_optimizations")->getDataStaticPtr();
    return **PNEWOPTIMIZATIONS;
}

static int getHyprlandDecorationRounding() {
    static auto* const* PROUNDING = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(SCROLLOVERVIEW_HANDLE, "decoration:rounding")->getDataStaticPtr();
    return std::max<int>(0, **PROUNDING);
}

static float getHyprlandDecorationRoundingPower() {
    static auto* const* PROUNDINGPOWER = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(SCROLLOVERVIEW_HANDLE, "decoration:rounding_power")->getDataStaticPtr();
    return **PROUNDINGPOWER;
}

struct SSurfaceOpacityOverride {
    WP<CWLSurfaceResource> surface;
    float                  opacity = 1.F;
};

struct SOverviewCustomDecorationRenderState {
    bool                                queuedAny = false;
    std::vector<std::function<void()>> restoreFns;
};

struct SHyprbarButtonMirror {
    std::string  cmd     = "";
    bool         userfg  = false;
    CHyprColor   fgcol   = CHyprColor(0, 0, 0, 0);
    CHyprColor   bgcol   = CHyprColor(0, 0, 0, 0);
    float        size    = 10.F;
    std::string  icon    = "";
    SP<Render::GL::CGLTexture> iconTex = makeShared<Render::GL::CGLTexture>();
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

    static auto* const* PACTIVEOPACITY     = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(SCROLLOVERVIEW_HANDLE, "decoration:active_opacity")->getDataStaticPtr();
    static auto* const* PINACTIVEOPACITY   = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(SCROLLOVERVIEW_HANDLE, "decoration:inactive_opacity")->getDataStaticPtr();
    static auto* const* PFULLSCREENOPACITY = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(SCROLLOVERVIEW_HANDLE, "decoration:fullscreen_opacity")->getDataStaticPtr();

    const bool  fullscreen     = window->isFullscreen();
    const bool  active         = Desktop::focusState()->window() == window;
    float       targetOpacity  = fullscreen ? **PFULLSCREENOPACITY : active ? **PACTIVEOPACITY : **PINACTIVEOPACITY;
    const auto& ruleOpacityVar = fullscreen ? window->m_ruleApplicator->alphaFullscreen() : active ? window->m_ruleApplicator->alpha() : window->m_ruleApplicator->alphaInactive();

    targetOpacity = ruleOpacityVar.valueOr(Desktop::Types::SAlphaValue{}).applyAlpha(targetOpacity);

    return std::clamp(targetOpacity, 0.F, 1.F);
}


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

    const auto STATEPTR = reinterpret_cast<UP<SHyprbarGlobalStateMirror>*>(symbol);
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

static bool shouldOverviewShadowIncludeHyprbar(const PHLWINDOW& window) {
    const float hyprbarHeight = getOverviewHyprbarLogicalHeight(window);
    if (hyprbarHeight <= 0.F)
        return false;

    static auto PHYPRBARPARTOFWINDOW = CConfigValue<Hyprlang::INT>("plugin:hyprbars:bar_part_of_window");
    return *PHYPRBARPARTOFWINDOW;
}

static bool shouldOverviewBorderIncludeHyprbar(const PHLWINDOW& window) {
    const float hyprbarHeight = getOverviewHyprbarLogicalHeight(window);
    if (hyprbarHeight <= 0.F)
        return false;

    static auto PHYPRBARPRECEDENCEOVERBORDER = CConfigValue<Hyprlang::INT>("plugin:hyprbars:bar_precedence_over_border");
    return *PHYPRBARPRECEDENCEOVERBORDER;
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
    metrics.shadowIncludesHyprbar   = shouldOverviewShadowIncludeHyprbar(window);
    metrics.borderIncludesHyprbar   = shouldOverviewBorderIncludeHyprbar(window);
    metrics.hyprbarHeightPx         = sc<int>(std::round(metrics.hyprbarLogicalHeight * metrics.pxScale));
    metrics.hyprbarHeightPxScaled   = metrics.hyprbarLogicalHeight * metrics.pxScale;
    metrics.hyprbarTopOffsetLogical = metrics.hyprbarLogicalHeight > 0.F ? metrics.hyprbarLogicalHeight + metrics.borderSize : 0.F;

    return metrics;
}

static std::optional<SDecorationPositioningReply> getOverviewTopStickyDecorationReply(IHyprWindowDecoration* decoration, const PHLWINDOW& window) {
    if (!decoration || !window)
        return std::nullopt;

    const auto INFO = decoration->getPositioningInfo();
    if (INFO.policy != DECORATION_POSITION_STICKY || INFO.edges != DECORATION_EDGE_TOP)
        return std::nullopt;

    const float HEIGHT  = std::max(0.F, sc<float>(INFO.desiredExtents.topLeft.y));
    const float BORDER  = sc<float>(window->getRealBorderSize());
    const bool  PRECEDE = shouldOverviewBorderIncludeHyprbar(window);
    const float WIDTH   = PRECEDE ? window->m_realSize->value().x : window->m_realSize->value().x + BORDER * 2.F;
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

    static auto PHYPRBARHEIGHT        = CConfigValue<Hyprlang::INT>("plugin:hyprbars:bar_height");
    static auto PHYPRBARTEXTSIZE      = CConfigValue<Hyprlang::INT>("plugin:hyprbars:bar_text_size");
    static auto PHYPRBARPADDING       = CConfigValue<Hyprlang::INT>("plugin:hyprbars:bar_padding");
    static auto PHYPRBARBUTTONPADDING = CConfigValue<Hyprlang::INT>("plugin:hyprbars:bar_button_padding");
    auto* const HYPRBARGLOBALSTATE    = getOverviewHyprbarGlobalState();

    const int previousBarHeight        = *PHYPRBARHEIGHT.ptr();
    const int previousBarTextSize      = *PHYPRBARTEXTSIZE.ptr();
    const int previousBarPadding       = *PHYPRBARPADDING.ptr();
    const int previousBarButtonPadding = *PHYPRBARBUTTONPADDING.ptr();
    std::vector<float> previousButtonSizes;
    if (HYPRBARGLOBALSTATE) {
        previousButtonSizes.reserve(HYPRBARGLOBALSTATE->buttons.size());
        for (auto& button : HYPRBARGLOBALSTATE->buttons) {
            previousButtonSizes.push_back(button.size);
            button.size *= metrics.renderScale;
            if (button.iconTex && button.iconTex->m_texID != 0)
                button.iconTex.reset();
        }
    }

    const Vector2D previousWindowPos       = window->m_realPosition->value();
    const Vector2D previousWindowSize      = window->m_realSize->value();
    const auto     WORKSPACE               = window->m_workspace;
    const bool     OVERRIDEWORKSPACEOFFSET = WORKSPACE && !window->m_pinned;
    const Vector2D previousWorkspaceOffset = OVERRIDEWORKSPACEOFFSET ? WORKSPACE->m_renderOffset->value() : Vector2D{};
    const auto     previousReplyData       = g_pDecorationPositioner->getDataFor(decoration, window);
    const auto     previousReply           = previousReplyData ? previousReplyData->lastReply : SDecorationPositioningReply{};

    window->m_realPosition->value() = monitor->m_position + windowBox.pos() / monitor->m_scale - window->m_floatingOffset;
    window->m_realSize->value()     = windowBox.size() / monitor->m_scale;

    *PHYPRBARHEIGHT.ptr()        = std::max(1, sc<int>(std::round(previousBarHeight * metrics.renderScale)));
    *PHYPRBARTEXTSIZE.ptr()      = std::max(1, sc<int>(std::round(previousBarTextSize * metrics.renderScale)));
    *PHYPRBARPADDING.ptr()       = std::max(0, sc<int>(std::round(previousBarPadding * metrics.renderScale)));
    *PHYPRBARBUTTONPADDING.ptr() = std::max(0, sc<int>(std::round(previousBarButtonPadding * metrics.renderScale)));

    const auto REPLY = getOverviewTopStickyDecorationReply(decoration, window);
    if (!REPLY) {
        window->m_realPosition->value() = previousWindowPos;
        window->m_realSize->value()     = previousWindowSize;
        *PHYPRBARHEIGHT.ptr()           = previousBarHeight;
        *PHYPRBARTEXTSIZE.ptr()         = previousBarTextSize;
        *PHYPRBARPADDING.ptr()          = previousBarPadding;
        *PHYPRBARBUTTONPADDING.ptr()    = previousBarButtonPadding;
        if (HYPRBARGLOBALSTATE) {
            for (size_t i = 0; i < previousButtonSizes.size() && i < HYPRBARGLOBALSTATE->buttons.size(); ++i) {
                HYPRBARGLOBALSTATE->buttons[i].size = previousButtonSizes[i];
                if (HYPRBARGLOBALSTATE->buttons[i].iconTex && HYPRBARGLOBALSTATE->buttons[i].iconTex->m_texID != 0)
                    HYPRBARGLOBALSTATE->buttons[i].iconTex.reset();
            }
        }
        return;
    }

    if (OVERRIDEWORKSPACEOFFSET)
        WORKSPACE->m_renderOffset->value() = {};

    if (previousReplyData)
        previousReplyData->lastReply = *REPLY;
    decoration->onPositioningReply(*REPLY);
    decoration->draw(monitor, metrics.targetOpacity);

    state.queuedAny = true;
    state.restoreFns.emplace_back([window, decoration, WORKSPACE, OVERRIDEWORKSPACEOFFSET, previousWindowPos, previousWindowSize, previousWorkspaceOffset, previousReply,
                                   previousReplyData, previousBarHeight, previousBarTextSize, previousBarPadding, previousBarButtonPadding, previousButtonSizes] {
        if (!window || !decoration)
            return;

        window->m_realPosition->value() = previousWindowPos;
        window->m_realSize->value()     = previousWindowSize;
        if (OVERRIDEWORKSPACEOFFSET && WORKSPACE)
            WORKSPACE->m_renderOffset->value() = previousWorkspaceOffset;

        if (previousReplyData)
            previousReplyData->lastReply = previousReply;
        decoration->onPositioningReply(previousReply);

        static auto PHYPRBARHEIGHT        = CConfigValue<Hyprlang::INT>("plugin:hyprbars:bar_height");
        static auto PHYPRBARTEXTSIZE      = CConfigValue<Hyprlang::INT>("plugin:hyprbars:bar_text_size");
        static auto PHYPRBARPADDING       = CConfigValue<Hyprlang::INT>("plugin:hyprbars:bar_padding");
        static auto PHYPRBARBUTTONPADDING = CConfigValue<Hyprlang::INT>("plugin:hyprbars:bar_button_padding");
        *PHYPRBARHEIGHT.ptr()             = previousBarHeight;
        *PHYPRBARTEXTSIZE.ptr()           = previousBarTextSize;
        *PHYPRBARPADDING.ptr()            = previousBarPadding;
        *PHYPRBARBUTTONPADDING.ptr()      = previousBarButtonPadding;
        if (auto* const HYPRBARGLOBALSTATE = getOverviewHyprbarGlobalState()) {
            for (size_t i = 0; i < previousButtonSizes.size() && i < HYPRBARGLOBALSTATE->buttons.size(); ++i) {
                HYPRBARGLOBALSTATE->buttons[i].size = previousButtonSizes[i];
                if (HYPRBARGLOBALSTATE->buttons[i].iconTex && HYPRBARGLOBALSTATE->buttons[i].iconTex->m_texID != 0)
                    HYPRBARGLOBALSTATE->buttons[i].iconTex.reset();
            }
        }
    });
}

static void renderOverviewWindowShadow(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, bool selected) {
    if (!monitor || !window || (!window->m_isMapped && !window->m_fadingOut))
        return;

    static auto PSHADOWS            = CConfigValue<Hyprlang::INT>("decoration:shadow:enabled");
    static auto PSHADOWSIZE         = CConfigValue<Hyprlang::INT>("decoration:shadow:range");
    static auto PSHADOWSHARP        = CConfigValue<Hyprlang::INT>("decoration:shadow:sharp");
    static auto PSHADOWSCALE        = CConfigValue<Hyprlang::FLOAT>("decoration:shadow:scale");
    static auto PSHADOWOFFSET       = CConfigValue<Hyprlang::VEC2>("decoration:shadow:offset");
    static auto PSHADOWCOL          = CConfigValue<Hyprlang::INT>("decoration:shadow:color");
    static auto PSHADOWCOLINACTIVE  = CConfigValue<Hyprlang::INT>("decoration:shadow:color_inactive");

    if (*PSHADOWS != 1 || *PSHADOWSIZE <= 0)
        return;

    if (window->isX11OverrideRedirect() || window->m_X11DoesntWantBorders || !window->m_ruleApplicator->decorate().valueOrDefault() ||
        window->m_ruleApplicator->noShadow().valueOrDefault())
        return;

    const int   rangePx          = sc<int>(std::round(*PSHADOWSIZE * monitor->m_scale * metrics.renderScale));
    const float shadowScale      = std::clamp(*PSHADOWSCALE, 0.F, 1.F);
    const auto  shadowOffset     = Vector2D{(*PSHADOWOFFSET).x, (*PSHADOWOFFSET).y} * monitor->m_scale * metrics.renderScale;

    if (rangePx <= 0)
        return;

    CBox outerBorderBox = getOverviewOuterBorderBox(windowBox, metrics);
    CBox shadowBox = outerBorderBox.copy().expand(rangePx).scaleFromCenter(shadowScale).translate(shadowOffset);
    shadowBox.round();

    if (shadowBox.width < 1 || shadowBox.height < 1)
        return;

    const auto shadowColor = CHyprColor(selected ? *PSHADOWCOL : *PSHADOWCOLINACTIVE != -1 ? *PSHADOWCOLINACTIVE : *PSHADOWCOL);
    if (shadowColor.a == 0.F)
        return;

    COverviewShadowPassElement::SData data;
    data.monitor       = monitor;
    data.fullBox       = shadowBox;
    data.cutoutBox     = outerBorderBox;
    data.rounding      = metrics.outerRoundPx;
    data.roundingPower = metrics.roundingPower;
    data.range         = rangePx;
    data.color         = shadowColor;
    data.alpha         = metrics.targetOpacity;
    data.ignoreWindow  = true; // decoration:shadow:ignore_window removed in Hyprland 0.55, defaults to enabled
    data.sharp         = *PSHADOWSHARP;
    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewShadowPassElement>(data));
}

static void renderOverviewWindowBorder(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, bool selected) {
    if (!monitor || !window || (!window->m_isMapped && !window->m_fadingOut))
        return;

    if (metrics.borderSize <= 0.F)
        return;

    static auto PACTIVECOL   = CConfigValue<Config::IComplexConfigValue>("general:col.active_border");
    static auto PINACTIVECOL = CConfigValue<Config::IComplexConfigValue>("general:col.inactive_border");
    auto* const ACTIVECOL    = reinterpret_cast<Config::CGradientValueData*>(PACTIVECOL.ptr());
    auto* const INACTIVECOL  = reinterpret_cast<Config::CGradientValueData*>(PINACTIVECOL.ptr());

    const auto& grad             = selected ? window->m_ruleApplicator->activeBorderColor().valueOr(*ACTIVECOL) : window->m_ruleApplicator->inactiveBorderColor().valueOr(*INACTIVECOL);

    const CBox borderBox = getOverviewBorderBox(windowBox, metrics);

    CBorderPassElement::SBorderData data;
    data.box           = borderBox;
    data.grad1         = grad;
    data.round         = metrics.roundingPx;
    data.outerRound    = metrics.outerRoundPx;
    data.roundingPower = metrics.roundingPower;
    data.a             = metrics.targetOpacity;
    data.borderSize    = metrics.borderSize;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(data));
}

static void renderOverviewGroupTabIndicators(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, float alpha) {
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
    static auto PGROUPCOLACTIVE         = CConfigValue<Config::IComplexConfigValue>("group:groupbar:col.active");
    static auto PGROUPCOLINACTIVE       = CConfigValue<Config::IComplexConfigValue>("group:groupbar:col.inactive");
    static auto PGROUPCOLACTIVELOCKED   = CConfigValue<Config::IComplexConfigValue>("group:groupbar:col.locked_active");
    static auto PGROUPCOLINACTIVELOCKED = CConfigValue<Config::IComplexConfigValue>("group:groupbar:col.locked_inactive");

    if (*PINDICATORHEIGHT <= 0)
        return;

    auto* const GROUPCOLACTIVE         = sc<Config::CGradientValueData*>(PGROUPCOLACTIVE.ptr());
    auto* const GROUPCOLINACTIVE       = sc<Config::CGradientValueData*>(PGROUPCOLINACTIVE.ptr());
    auto* const GROUPCOLACTIVELOCKED   = sc<Config::CGradientValueData*>(PGROUPCOLACTIVELOCKED.ptr());
    auto* const GROUPCOLINACTIVELOCKED = sc<Config::CGradientValueData*>(PGROUPCOLINACTIVELOCKED.ptr());

    const bool  groupLocked  = window->m_group->locked() || g_pKeybindManager->m_groupsLocked;
    const auto* colActive    = groupLocked ? GROUPCOLACTIVELOCKED : GROUPCOLACTIVE;
    const auto* colInactive  = groupLocked ? GROUPCOLINACTIVELOCKED : GROUPCOLINACTIVE;
    const auto  groupWindows = window->m_group->windows();
    const auto  groupCurrent = window->m_group->current();
    const float indicatorH    = sc<float>(*PINDICATORHEIGHT) * metrics.pxScale;
    const float outerGap      = sc<float>(*POUTERGAP) * metrics.pxScale;
    const float innerGap      = sc<float>(*PINNERGAP) * metrics.pxScale;
    const float oneBarHeight  = sc<float>(*POUTERGAP + *PINDICATORHEIGHT + *PINDICATORGAP + (*PGRADIENTS || *PRENDERTITLES ? *PHEIGHT : 0)) * metrics.pxScale;
    const int   rounding      = sc<int>(std::round(*PROUNDING * metrics.pxScale));
    CBox        indicatorArea = windowBox.copy().expand(metrics.borderPxScaled);
    indicatorArea.y -= metrics.hyprbarHeightPxScaled;

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

static void renderOverviewGroupTabs(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const CBox& workspaceBox,
                                    const SOverviewWindowMetrics& metrics) {
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

    const auto  ONEBARHEIGHT     = *POUTERGAP + *PINDICATORHEIGHT + *PINDICATORGAP + (*PGRADIENTS || *PRENDERTITLES ? *PHEIGHT : 0);
    const auto  DESIREDHEIGHT    = *PSTACKED ? (ONEBARHEIGHT * window->m_group->size()) + *POUTERGAP * *PKEEPUPPERGAP : *POUTERGAP * (1 + *PKEEPUPPERGAP) + ONEBARHEIGHT;
    const auto  EDGEPOINT        = g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, window);
    CBox        assignedBox      = {window->m_realPosition->value() - Vector2D{0.0, sc<double>(DESIREDHEIGHT) + metrics.hyprbarTopOffsetLogical},
                                    Vector2D{window->m_realSize->value().x, sc<double>(DESIREDHEIGHT)}};
    assignedBox.translate(-EDGEPOINT);

    if (window->m_workspace && !window->m_pinned)
        assignedBox.translate(-window->m_workspace->m_renderOffset->value());

    const auto PREVASSIGNEDBOX = GROUPBAR->m_assignedBox;
    GROUPBAR->m_assignedBox    = assignedBox;
    auto restoreAssignedBox    = Hyprutils::Utils::CScopeGuard([GROUPBAR, PREVASSIGNEDBOX] { GROUPBAR->m_assignedBox = PREVASSIGNEDBOX; });

    SRenderModifData modif;
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_SCALE, metrics.renderScale);
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, workspaceBox.pos());

    GROUPBAR->updateWindow(window);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
    GROUPBAR->draw(monitor, metrics.targetOpacity);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = SRenderModifData{}}));
    renderOverviewGroupTabIndicators(monitor, window, windowBox, metrics, metrics.targetOpacity);
}

static SOverviewCustomDecorationRenderState renderOverviewCustomDecorations(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& workspaceBox, const CBox& windowBox,
                                                                           const SOverviewWindowMetrics& metrics, eDecorationLayer layer) {
    SOverviewCustomDecorationRenderState state;

    if (!monitor || !window)
        return state;

    window->updateWindowDecos();

    SRenderModifData modif;
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_SCALE, metrics.renderScale);
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, workspaceBox.pos());
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
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = SRenderModifData{}}));

    return state;
}

} // namespace

bool shouldBlurBackground(const PHLWINDOW& window) {
    return window && ScrollOverview::RendererAccess::shouldBlur(window);
}

bool shouldUsePrecomputedBlur(const PHLWINDOW& window) {
    return getHyprlandBlurNewOptimizations() && shouldShowOverviewWindow(window) && !window->m_isFloating && shouldBlurBackground(window);
}

bool shouldUseBlurFramebuffer(const PHLWINDOW& window) {
    return shouldUsePrecomputedBlur(window) || (shouldShowOverviewWindow(window) && shouldBlurBackground(window) && window->m_ruleApplicator->xray().valueOr(false));
}

void renderOverviewWindow(const SRenderParams& params) {
    if (!params.window)
        return;

    const bool                   fullscreen   = params.window->isFullscreen();
    const bool                   shouldBlurBg = shouldBlurBackground(params.window);
    const SOverviewWindowMetrics metrics      = getOverviewWindowMetrics(params.monitor, params.window, params.renderScale);

    const auto underDecos =
        renderOverviewCustomDecorations(params.monitor, params.window, params.workspaceBox ? *params.workspaceBox : CBox{}, params.windowBox, metrics, DECORATION_LAYER_UNDER);
    if (underDecos.queuedAny) {
        OverviewRender::flushPass(params.monitor);
        for (auto it = underDecos.restoreFns.rbegin(); it != underDecos.restoreFns.rend(); ++it)
            (*it)();
    }

    if (!fullscreen)
        renderOverviewWindowShadow(params.monitor, params.window, params.windowBox, metrics, params.selected);

    if (shouldBlurBg) {
        OverviewRender::flushPass(params.monitor);

        const float blurAlpha     = std::sqrt(params.window->alphaValue(Desktop::View::WINDOW_ALPHA_FADE));
        const int   blurRounding  = fullscreen ? 0 : sc<int>(std::round(getHyprlandDecorationRounding() * metrics.pxScale));
        const float roundingPower = fullscreen ? 2.F : getHyprlandDecorationRoundingPower();
        OverviewRender::renderBlur(params.monitor, params.windowBox, blurRounding, roundingPower, blurAlpha,
                                   params.usePrecomputedBlur || params.window->m_ruleApplicator->xray().valueOr(false));
    }

    SRenderModifData modif;
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_SCALE, params.renderScale);
    modif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, params.windowBox.pos());

    std::vector<SSurfaceOpacityOverride> surfaceOpacityOverrides;
    surfaceOpacityOverrides.reserve(4);
    overrideWindowSurfaceOpacity(params.window, surfaceOpacityOverrides, metrics.targetOpacity);
    auto restoreSurfaceOpacities = Hyprutils::Utils::CScopeGuard([&surfaceOpacityOverrides] { restoreSurfaceOpacityOverrides(surfaceOpacityOverrides); });

    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
    const auto firstWindowPassElement = g_pHyprRenderer->m_renderPass.m_passElements.size();
    ScrollOverview::RendererAccess::renderWindow(params.window, params.monitor, params.now, true, RENDER_PASS_ALL, true, true);
    if (!fullscreen)
        roundStandaloneWindowPassElements(params.window, params.monitor, params.renderScale, firstWindowPassElement);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = SRenderModifData{}}));

    renderOverviewCustomDecorations(params.monitor, params.window, params.workspaceBox ? *params.workspaceBox : CBox{}, params.windowBox, metrics, DECORATION_LAYER_OVER);
    renderOverviewCustomDecorations(params.monitor, params.window, params.workspaceBox ? *params.workspaceBox : CBox{}, params.windowBox, metrics, DECORATION_LAYER_OVERLAY);

    if (!fullscreen) {
        renderOverviewWindowBorder(params.monitor, params.window, params.windowBox, metrics, params.selected);
        if (params.workspaceBox)
            renderOverviewGroupTabs(params.monitor, params.window, params.windowBox, *params.workspaceBox, metrics);
    }

    OverviewRender::flushPass(params.monitor);
}

} // namespace OverviewWindow
