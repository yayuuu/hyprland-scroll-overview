#include "Window.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>
#define private public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/BackdropScopePassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/types.hpp>
#include <hyprland/src/render/decorations/CHyprGroupBarDecoration.hpp>
#include <hyprland/src/render/decorations/CHyprInnerGlowDecoration.hpp>
#include <hyprland/src/render/decorations/CHyprDropShadowDecoration.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/decorations/CHyprBorderDecoration.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#undef protected
#undef private
#include "Config.hpp"
#include "OverviewPassElement.hpp"
#include "OverviewRender.hpp"

namespace OverviewWindow {
namespace {

using GLRenderer = Render::GL::CHyprOpenGLImpl;
using RectData = GLRenderer::SRectRenderData;
using TextureData = GLRenderer::STextureRenderData;
using RectFn = void (*)(GLRenderer*, Render::CRenderContext&, const CBox&, const CHyprColor&, RectData);
using TextureFn = void (*)(GLRenderer*, Render::CRenderContext&, SP<Render::ITexture>, const CBox&, TextureData);
using ScissorFn = void (*)(GLRenderer*, Render::CRenderContext&, const CBox&, bool);

CFunctionHook *rectHook = nullptr, *textureHook = nullptr, *scissorHook = nullptr;

struct SDecorationTransform {
    Render::CRenderContext* context;
    float scale;
    CBox sourceBounds;
    CBox targetBounds;

    CBox box(CBox source) const {
        // The preview's left and right edges have already been rounded. Map
        // both edges exactly instead of scaling its native width independently.
        // Keep the vertical scale and the decoration's finished layout intact.
        const auto mapX = [&](double x) {
            return std::lerp(targetBounds.x, targetBounds.x + targetBounds.width, (x - sourceBounds.x) / sourceBounds.width);
        };
        const auto left  = mapX(source.x);
        const auto right = mapX(source.x + source.width);
        source.x         = left;
        source.width     = right - left;
        source.y         = targetBounds.y + (source.y - sourceBounds.y) * scale;
        source.height   *= scale;
        return source;
    }

    CRegion region(const CRegion& source) const {
        CRegion result;
        source.forEachRect([&](const auto& rect) {
            auto target = box(CBox{rect.x1, rect.y1, rect.x2 - rect.x1, rect.y2 - rect.y1});
            result.add(CBox{std::floor(target.x), std::floor(target.y),
                            std::ceil(target.x + target.width) - std::floor(target.x),
                            std::ceil(target.y + target.height) - std::floor(target.y)});
        });
        return result;
    }
};

thread_local const SDecorationTransform* decorationTransform = nullptr;

// Transform the decoration's finished, pixel-aligned layout exactly once.
// Nested renderer calls (including blur and its UVs) already use destination
// coordinates and must go through the native renderer without another transform.
template <typename Data, typename Paint>
void drawDecoration(Render::CRenderContext& ctx, const CBox& box, Data data, Paint paint) {
    const auto transform = decorationTransform;
    if (!transform || transform->context != &ctx) {
        paint(box, data);
        return;
    }
    decorationTransform = nullptr;
    auto restore = Hyprutils::Utils::CScopeGuard([&] { decorationTransform = transform; });
    data.round = sc<int>(std::round(data.round * transform->scale));
    if constexpr (requires { data.blurPatternBox; }) {
        if (data.blurPatternBox)
            data.blurPatternBox = transform->box(*data.blurPatternBox);
    }
    if constexpr (requires { data.clipRegion; }) {
        if (!data.clipRegion.empty())
            data.clipRegion = transform->region(data.clipRegion);
    }
    CRegion damage;
    if (data.damage) {
        damage = transform->region(*data.damage);
        data.damage = &damage;
    }
    paint(transform->box(box), data);
}

void renderDecorationRect(GLRenderer* self, Render::CRenderContext& ctx, const CBox& box, const CHyprColor& color, RectData data) {
    drawDecoration(ctx, box, std::move(data), [&](const CBox& target, RectData adjusted) {
        reinterpret_cast<RectFn>(rectHook->m_original)(self, ctx, target, color, std::move(adjusted));
    });
}

void renderDecorationTexture(GLRenderer* self, Render::CRenderContext& ctx, SP<Render::ITexture> texture, const CBox& box, TextureData data) {
    drawDecoration(ctx, box, std::move(data), [&](const CBox& target, TextureData adjusted) {
        reinterpret_cast<TextureFn>(textureHook->m_original)(self, ctx, std::move(texture), target, std::move(adjusted));
    });
}

void scissorDecoration(GLRenderer* self, Render::CRenderContext& ctx, const CBox& box, bool transformOutput) {
    const auto original = reinterpret_cast<ScissorFn>(scissorHook->m_original);
    const auto transform = decorationTransform;
    if (!transform || transform->context != &ctx) {
        original(self, ctx, box, transformOutput);
        return;
    }
    auto target = transform->box(box);
    // Scissors are integral; round outwards without snapping the painted boxes.
    target = CBox{std::floor(target.x), std::floor(target.y),
                  std::ceil(target.x + target.width) - std::floor(target.x),
                  std::ceil(target.y + target.height) - std::floor(target.y)};
    original(self, ctx, target, transformOutput);
}


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

    if (window->grouping().group())
        return window->grouping().group()->current();

    return window;
}

static bool shouldShowOverviewWindow(const PHLWINDOW& window) {
    const auto WINDOW = getOverviewWindowToShow(window);

    if (!validMapped(WINDOW))
        return false;

    if ((WINDOW->m_state & Desktop::View::WINDOW_STATE_PINNED) && WINDOW->isFloating())
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
    CHyprBorderDecoration*         borderDecoration = nullptr;
    CHyprDropShadowDecoration*     shadowDecoration = nullptr;
    CHyprInnerGlowDecoration*      glowDecoration = nullptr;
    Config::CGradientValueData     previousBorderColor;
    Config::CGradientValueData     previousBorderColorPrevious;
    Config::CGradientValueData     previousShadowColor;
    Config::CGradientValueData     previousShadowColorPrevious;
    Config::CGradientValueData     previousGlowColor;
    Config::CGradientValueData     previousGlowColorPrevious;
    float                          previousOpacity = 1.F;
    float                          previousDim     = 0.F;
    bool                           active          = false;

    static void setGradientState(CAnimatedDecorationGradient& gradient, const Config::CGradientValueData& value) {
        gradient.m_current  = value;
        gradient.m_previous = value;
    }

    SOverviewPseudoFocusState(const PHLWINDOW& window_, const PHLWINDOW& pseudoFocusWindow) : window(window_) {
        if (!window || !pseudoFocusWindow)
            return;

        active               = true;
        focusState           = Desktop::focusState();
        previousFocusWindow  = focusState->m_focusWindow;
        previousFocusSurface = focusState->m_focusSurface;
        previousOpacity      = window->presentation().alpha(Desktop::View::WINDOW_ALPHA_ACTIVE)->value();
        previousDim          = window->presentation().dimPercent();

        borderDecoration = dc<CHyprBorderDecoration*>(window->presentation().decoration(DECORATION_BORDER).get());
        shadowDecoration = dc<CHyprDropShadowDecoration*>(window->presentation().decoration(DECORATION_SHADOW).get());
        glowDecoration   = dc<CHyprInnerGlowDecoration*>(window->presentation().decoration(DECORATION_INNER_GLOW).get());

        if (borderDecoration) {
            previousBorderColor         = borderDecoration->m_gradient.m_current;
            previousBorderColorPrevious = borderDecoration->m_gradient.m_previous;
        }
        if (shadowDecoration) {
            previousShadowColor         = shadowDecoration->m_gradient.m_current;
            previousShadowColorPrevious = shadowDecoration->m_gradient.m_previous;
        }
        if (glowDecoration) {
            previousGlowColor         = glowDecoration->m_gradient.m_current;
            previousGlowColorPrevious = glowDecoration->m_gradient.m_previous;
        }

        focusState->m_focusWindow = pseudoFocusWindow;
        if (pseudoFocusWindow->wlSurface() && pseudoFocusWindow->wlSurface()->resource())
            focusState->m_focusSurface = pseudoFocusWindow->wlSurface()->resource();

        const bool ISACTIVE    = window == pseudoFocusWindow;
        const bool GROUPLOCKED = (window->grouping().group() && window->grouping().group()->locked()) || Desktop::windowState()->groupsLocked();
        const bool NOGROUP     = window->grouping().rules() & Desktop::View::GROUP_DENY;
        const auto BORDERKEY   = window->grouping().group() ?
            (ISACTIVE ? (GROUPLOCKED ? "group:col.border_locked_active" : "group:col.border_active") :
                        (GROUPLOCKED ? "group:col.border_locked_inactive" : "group:col.border_inactive")) :
            (ISACTIVE ? (NOGROUP ? "general:col.nogroup_border_active" : "general:col.active_border") :
                        (NOGROUP ? "general:col.nogroup_border" : "general:col.inactive_border"));
        const auto BORDERCOLOR = ScrollOverview::Config::getValue<Config::CGradientValueData>(BORDERKEY);
        const auto BORDER       = ISACTIVE ? window->m_ruleApplicator->activeBorderColor().valueOr(BORDERCOLOR) :
            window->m_ruleApplicator->inactiveBorderColor().valueOr(BORDERCOLOR);

        const auto SHADOWACTIVE   = ScrollOverview::Config::getValue<Config::CGradientValueData>("decoration:shadow:color");
        const auto SHADOWINACTIVE = ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("decoration:shadow:color_inactive").setByUser() ?
            ScrollOverview::Config::getValue<Config::CGradientValueData>("decoration:shadow:color_inactive") : SHADOWACTIVE;
        const auto GLOWACTIVE     = ScrollOverview::Config::getValue<Config::CGradientValueData>("decoration:glow:color");
        const auto GLOWINACTIVE   = Config::mgr()->getConfigValue("decoration:glow:color_inactive").setByUser ?
            ScrollOverview::Config::getValue<Config::CGradientValueData>("decoration:glow:color_inactive") : GLOWACTIVE;
        const Config::CGradientValueData TRANSPARENT{CHyprColor{0, 0, 0, 0}};

        if (borderDecoration)
            setGradientState(borderDecoration->m_gradient, BORDER);
        if (shadowDecoration)
            setGradientState(shadowDecoration->m_gradient, window->backend().traits().overrideRedirect || window->backend().traits().suggestsNoBorder ? TRANSPARENT : (ISACTIVE ? SHADOWACTIVE : SHADOWINACTIVE));
        if (glowDecoration)
            setGradientState(glowDecoration->m_gradient, window->backend().traits().overrideRedirect || window->backend().traits().suggestsNoBorder ? TRANSPARENT : (ISACTIVE ? GLOWACTIVE : GLOWINACTIVE));

        window->presentation().alpha(Desktop::View::WINDOW_ALPHA_ACTIVE)->value() = getOverviewWindowTargetOpacity(window);

        const bool ISMODALSHADOWED = window->backend().traits().hasModalChild;
        float      dim             = ISACTIVE || window->m_ruleApplicator->noDim().valueOrDefault() || !ScrollOverview::Config::getValue<bool>("decoration:dim_inactive") ?
                 0.F :
                 ScrollOverview::Config::getValue<float>("decoration:dim_strength");
        if (ISMODALSHADOWED && ScrollOverview::Config::getValue<bool>("decoration:dim_modal"))
            dim += (1.F - dim) / 2.F;
        window->presentation().m_dimPercent->value() = dim;
    }

    ~SOverviewPseudoFocusState() {
        if (!active)
            return;

        if (borderDecoration) {
            borderDecoration->m_gradient.m_current  = previousBorderColor;
            borderDecoration->m_gradient.m_previous = previousBorderColorPrevious;
        }
        if (shadowDecoration) {
            shadowDecoration->m_gradient.m_current  = previousShadowColor;
            shadowDecoration->m_gradient.m_previous = previousShadowColorPrevious;
        }
        if (glowDecoration) {
            glowDecoration->m_gradient.m_current  = previousGlowColor;
            glowDecoration->m_gradient.m_previous = previousGlowColorPrevious;
        }
        window->presentation().alpha(Desktop::View::WINDOW_ALPHA_ACTIVE)->value() = previousOpacity;
        window->presentation().m_dimPercent->value() = previousDim;
        focusState->m_focusWindow  = previousFocusWindow;
        focusState->m_focusSurface = previousFocusSurface;
    }
};


static void roundStandaloneWindowPassElements(const PHLWINDOW& window, PHLMONITOR monitor, float renderScale, size_t firstElement) {
    if (!window || !monitor)
        return;

    if (Fullscreen::controller()->isFullscreen(window))
        return;

    const int   rounding      = sc<int>(std::round(window->presentation().rounding() * monitor->m_scale * renderScale));
    const float roundingPower = window->presentation().roundingPower();

    if (rounding <= 0)
        return;

    auto& passElements = Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).m_passElements;
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

static float getOverviewHyprbarLogicalHeight(const PHLWINDOW& window) {
    if (!window || !window->m_ruleApplicator->decorate().valueOrDefault())
        return 0.F;

    for (const auto& deco : window->presentation().decorations()) {
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

    auto& passElements = Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).m_passElements;
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

    const auto REPORTEDSIZE = window->backend().reportedSize();
    if (REPORTEDSIZE.x <= 0 || REPORTEDSIZE.y <= 0)
        return;

    const Vector2D SURFACESCALE = targetSize / REPORTEDSIZE;
    auto&          passElements = Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).m_passElements;

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

    auto& passElements = Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).m_passElements;
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
    metrics.borderSize              = window->presentation().borderSize();
    metrics.borderPx                = sc<int>(std::round(metrics.borderSize * monitor->m_scale));
    metrics.borderPxScaled          = metrics.borderSize * metrics.pxScale;
    metrics.roundingBase            = window->presentation().rounding();
    metrics.roundingPower           = window->presentation().roundingPower();
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

static void renderOverviewHyprbarDecoration(PHLMONITOR monitor, const PHLWINDOW& window, IHyprWindowDecoration* decoration,
                                          const CBox& windowBox, const SOverviewWindowMetrics& metrics) {
    if (!monitor || !window || !decoration || !std::isfinite(metrics.renderScale) || metrics.renderScale <= 0.F || windowBox.empty())
        return;

    auto& ctx = g_pHyprRenderer->context();
    OverviewRender::flushPass(ctx, monitor);
    auto savedDrawState = ctx.saveDrawState();
    ctx.m_data.currentWindow = window;

    const auto WORKSPACE = window->m_workspace;
    const bool OVERRIDEWORKSPACEOFFSET = WORKSPACE && !(window->m_state & Desktop::View::WINDOW_STATE_PINNED);
    const auto previousWorkspaceOffset = OVERRIDEWORKSPACEOFFSET ? WORKSPACE->m_renderOffset->value() : Vector2D{};
    auto restoreOffset = Hyprutils::Utils::CScopeGuard([&] {
        if (OVERRIDEWORKSPACEOFFSET)
            WORKSPACE->m_renderOffset->value() = previousWorkspaceOffset;
    });
    if (OVERRIDEWORKSPACEOFFSET)
        WORKSPACE->m_renderOffset->value() = {};

    const auto nativeBounds = CBox{(window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + window->presentation().floatingOffset() - monitor->m_position) * monitor->m_scale,
                                  window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT) * monitor->m_scale}.round();
    const auto presentation = window->presentation().renderPresentation(dynamicPointerCast<Workspace::CWorkspacePresentable>(WORKSPACE));
    if (nativeBounds.width <= 0)
        return;
    ctx.m_data.renderModif = {};
    ctx.m_data.clipBox = {};
    ctx.m_data.noSimplify = true; // The decoration reports bounds in native coordinates.
    const SDecorationTransform transform{&ctx, metrics.renderScale, nativeBounds, windowBox};
    const auto previous = std::exchange(decorationTransform, &transform);
    auto restoreTransform = Hyprutils::Utils::CScopeGuard([&] { decorationTransform = previous; });
    decoration->draw(ctx, monitor, presentation.alpha * presentation.fadeAlpha, presentation);
    OverviewRender::flushPass(ctx, monitor);
}

static void renderOverviewWindowShadow(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, bool selected) {
    if (!monitor || !window || (!window->mapped()))
        return;

    const auto PSHADOWS      = ScrollOverview::Config::getValue<int>("decoration:shadow:enabled");
    const auto PSHADOWSIZE   = ScrollOverview::Config::getValue<int>("decoration:shadow:range");
    const auto PSHADOWSCALE  = ScrollOverview::Config::getValue<float>("decoration:shadow:scale");
    const auto PSHADOWOFFSET = ScrollOverview::Config::getValue<Hyprlang::VEC2>("decoration:shadow:offset");

    if (PSHADOWS != 1 || PSHADOWSIZE <= 0)
        return;

    if (window->backend().traits().overrideRedirect || window->backend().traits().suggestsNoBorder || !window->m_ruleApplicator->decorate().valueOrDefault() ||
        window->m_ruleApplicator->noShadow().valueOrDefault())
        return;

    const int   rangePx          = sc<int>(std::round(PSHADOWSIZE * metrics.pxScale));
    const float shadowScale      = std::clamp(PSHADOWSCALE, 0.F, 1.F);
    const auto  shadowOffset     = Vector2D{PSHADOWOFFSET.x, PSHADOWOFFSET.y} * metrics.pxScale;

    if (rangePx <= 0)
        return;

    CBox shadowBaseBox = getOverviewOuterBorderBox(windowBox, metrics);
    CBox cutoutBox     = windowBox;
    CBox shadowBox     = shadowBaseBox.copy().expand(rangePx).scaleFromCenter(shadowScale).translate(shadowOffset);
    shadowBox.round();

    if (shadowBox.width < 1 || shadowBox.height < 1)
        return;

    auto* const SHADOWDECO = dc<CHyprDropShadowDecoration*>(window->presentation().decoration(DECORATION_SHADOW).get());
    if (!SHADOWDECO)
        return;
    const auto  shadowState = SHADOWDECO->m_gradient.renderState();
    const auto& shadowColor = shadowState.current;
    if (std::ranges::none_of(shadowColor.m_colors, [](const CHyprColor& color) { return color.a > 0.F; }))
        return;

    Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).add(makeUnique<COverviewShadowPassElement>(COverviewShadowPassElement::SData{
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

static void renderOverviewWindowBorder(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, bool selected) {
    if (!monitor || !window || (!window->mapped()))
        return;

    if (metrics.borderSize <= 0.F)
        return;

    auto* const BORDERDECO = dc<CHyprBorderDecoration*>(window->presentation().decoration(DECORATION_BORDER).get());
    if (!BORDERDECO)
        return;

    const auto GRADIENT = BORDERDECO->m_gradient.renderState();
    auto       grad     = GRADIENT.current;

    const CBox borderBox = getOverviewBorderBox(windowBox, metrics);

    CBorderPassElement::SBorderData data;
    data.box           = borderBox;
    data.grad1         = grad;
    data.round         = metrics.roundingPx;
    data.outerRound    = metrics.outerRoundPx;
    data.roundingPower = metrics.roundingPower;
    data.a             = metrics.targetOpacity;
    data.borderSize    = metrics.borderSize;
    if (GRADIENT.transitioning) {
        data.hasGrad2 = true;
        data.grad1    = GRADIENT.previous;
        data.grad2    = grad;
        data.lerp     = GRADIENT.progress;
    } else
        data.grad1 = grad;
    Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).add(makeUnique<CBorderPassElement>(data));
}

static void renderOverviewGroupTabIndicators(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, float alpha) {
    if (!monitor || !window || !window->grouping().group() || window->grouping().group()->size() < 1)
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

    const bool  groupLocked  = window->grouping().group()->locked() || Desktop::windowState()->groupsLocked();
    const auto  groupWindows = window->grouping().group()->windows();
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
        Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).add(makeUnique<CRectPassElement>(data));
    }
}

static void renderOverviewGroupTabTitles(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, float alpha) {
    if (!monitor || !window || !window->grouping().group() || window->grouping().group()->size() < 1 || alpha <= 0.F)
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

    const bool  groupLocked  = window->grouping().group()->locked() || Desktop::windowState()->groupsLocked();
    const auto  groupWindows = window->grouping().group()->windows();
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
            g_pHyprRenderer->renderText(member->metadata().title(), textColor, fontSizePx, false, FONTFAMILY, maxWidth, isCurrent ? FONTWEIGHTACTIVE->m_value : FONTWEIGHTINACTIVE->m_value);
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
        Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).add(makeUnique<CTexPassElement>(data));
    }
}

static void renderOverviewGroupTabs(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const CBox& workspaceBox,
                                    const SOverviewWindowMetrics& metrics) {
    if (!monitor || !window || !window->grouping().group() || window->grouping().group()->size() < 1)
        return;

    auto* const GROUPBAR = dc<CHyprGroupBarDecoration*>(window->presentation().decoration(DECORATION_GROUPBAR).get());
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

    const auto   ONEBARHEIGHT  = POUTERGAP + PINDICATORHEIGHT + PINDICATORGAP + (PGRADIENTS || PRENDERTITLES ? PHEIGHT : 0);
    const double DESIREDHEIGHT = sc<double>(PSTACKED ? (ONEBARHEIGHT * window->grouping().group()->size()) + POUTERGAP * PKEEPUPPERGAP : POUTERGAP * (1 + PKEEPUPPERGAP) + ONEBARHEIGHT);
    const auto   EDGEPOINT     = g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, window);
    CBox         assignedBox   = {window->positionAnimation()->value() - Vector2D(0.0, DESIREDHEIGHT + metrics.hyprbarTopOffsetLogical),
                          Vector2D(window->sizeAnimation()->value().x, DESIREDHEIGHT)};
    assignedBox.translate(-EDGEPOINT);

    if (window->m_workspace && !(window->m_state & Desktop::View::WINDOW_STATE_PINNED))
        assignedBox.translate(-window->m_workspace->m_renderOffset->value());

    const auto PREVASSIGNEDBOX = GROUPBAR->m_assignedBox;
    GROUPBAR->m_assignedBox    = assignedBox;
    auto restoreAssignedBox    = Hyprutils::Utils::CScopeGuard([GROUPBAR, PREVASSIGNEDBOX] { GROUPBAR->m_assignedBox = PREVASSIGNEDBOX; });

    Render::SRenderModifData modif;
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, metrics.renderScale);
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, workspaceBox.pos());

    GROUPBAR->updateWindow(window);
    Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
    GROUPBAR->draw(g_pHyprRenderer->context(), monitor, metrics.targetOpacity, window->presentation().renderPresentation(dynamicPointerCast<Workspace::CWorkspacePresentable>(window->m_workspace)));
    Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));
    renderOverviewGroupTabIndicators(monitor, window, windowBox, metrics, metrics.targetOpacity);
}

static bool renderOverviewCustomDecorations(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& workspaceBox, const CBox& windowBox,
                                           const SOverviewWindowMetrics& metrics, eDecorationLayer layer) {
    if (!monitor || !window)
        return false;

    window->presentation().updateDecorations();

    Render::SRenderModifData modif;
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, metrics.renderScale);
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, workspaceBox.pos());
    bool        drewAny = false;

    for (const auto& deco : window->presentation().decorations()) {
        if (!deco || deco->getDecorationType() != DECORATION_CUSTOM || deco->getDecorationLayer() != layer)
            continue;

        if (layer == DECORATION_LAYER_UNDER && isOverviewHyprbarDecoration(deco.get())) {
            if (drewAny) {
                Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));
                OverviewRender::flushPass(g_pHyprRenderer->context(), monitor);
                drewAny = false;
            }
            renderOverviewHyprbarDecoration(monitor, window, deco.get(), windowBox, metrics);
            continue;
        }

        if (!drewAny) {
            Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
            drewAny = true;
        }

        deco->updateWindow(window);
        deco->draw(g_pHyprRenderer->context(), monitor, metrics.targetOpacity, window->presentation().renderPresentation(dynamicPointerCast<Workspace::CWorkspacePresentable>(window->m_workspace)));
    }

    if (drewAny)
        Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));

    return drewAny;
}

} // namespace

void initDecorationRenderer() {
    const auto install = [](CFunctionHook*& hook, const char* name, const char* signature, void* replacement) {
        hook = HyprlandAPI::createFunctionHook(SCROLLOVERVIEW_HANDLE, findFnOrThrow(name, {signature}), replacement);
        if (!hook || !hook->hook())
            throw std::runtime_error(std::string{"[scrolloverview] Failed enabling decoration hook: "} + name);
    };
    install(rectHook, "renderRect", "Render::GL::CHyprOpenGLImpl::renderRect(", reinterpret_cast<void*>(renderDecorationRect));
    install(textureHook, "renderTexture", "Render::GL::CHyprOpenGLImpl::renderTexture(", reinterpret_cast<void*>(renderDecorationTexture));
    install(scissorHook, "scissor", "Render::GL::CHyprOpenGLImpl::scissor(Render::CRenderContext&, Hyprutils::Math::CBox const&, bool)",
            reinterpret_cast<void*>(scissorDecoration));
}

void cleanupDecorationRenderer() {
    for (auto hook : {&scissorHook, &textureHook, &rectHook}) {
        if (*hook)
            HyprlandAPI::removeFunctionHook(SCROLLOVERVIEW_HANDLE, *hook);
        *hook = nullptr;
    }
}

bool shouldBlurBackground(const PHLWINDOW& window) {
    return window && window->shouldBlur();
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
    return !dragged && ScrollOverview::Config::getValue<bool>("decoration:blur:new_optimizations") && shouldShowOverviewWindow(window) && !window->isFloating() && shouldBlurBackground(window) &&
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
    WINDOW->presentation().updateDecorations();
}

void renderOverviewWindow(const SRenderParams& params) {
    if (!params.window)
        return;

    const SOverviewPseudoFocusState pseudoFocusState{params.window, params.pseudoFocusWindow};
    const bool                   fullscreen   = Fullscreen::controller()->isFullscreen(params.window);
    const SOverviewWindowMetrics metrics      = getOverviewWindowMetrics(params.monitor, params.window, params.renderScale);

    auto& ctx = g_pHyprRenderer->context();
    SP<SBackdropScope> backdropScope;
    if (metrics.hyprbarLogicalHeight > 0.F && ScrollOverview::Config::getValue<bool>("decoration:blur:enabled") &&
        (ScrollOverview::Config::getValue<bool>("plugin:hyprbars:bar_blur") || params.window->shouldBlur())) {
        OverviewRender::flushPass(ctx, params.monitor);
        backdropScope           = makeShared<SBackdropScope>();
        backdropScope->required = true;
        backdropScope->damage   = CRegion{CBox{{}, params.monitor->m_transformedSize}};
        // Keep one native backdrop alive across the separate decoration and
        // surface passes, just as renderWindow does for a normally drawn window.
        g_pHyprRenderer->beginBackdropScope(ctx, backdropScope);
    }
    auto restoreBackdrop = Hyprutils::Utils::CScopeGuard([&] {
        if (backdropScope)
            g_pHyprRenderer->endBackdropScope(ctx, backdropScope);
    });

    if (!fullscreen)
        renderOverviewWindowShadow(params.monitor, params.window, params.windowBox, metrics, params.selected);

    if (renderOverviewCustomDecorations(params.monitor, params.window, params.workspaceBox ? *params.workspaceBox : CBox{}, params.windowBox, metrics, DECORATION_LAYER_UNDER))
        OverviewRender::flushPass(g_pHyprRenderer->context(), params.monitor);

    const Vector2D previousWindowPos       = params.window->positionAnimation()->value();
    const Vector2D previousWindowSize      = params.window->sizeAnimation()->value();
    const bool     previousAnimatingIn     = params.window->presentation().animatingIn();
    const bool     previousSizeAnimating   = params.window->sizeAnimation()->isBeingAnimated();
    const auto     WORKSPACE               = params.window->m_workspace;
    const bool     OVERRIDEWORKSPACEOFFSET = WORKSPACE && !(params.window->m_state & Desktop::View::WINDOW_STATE_PINNED);
    const Vector2D previousWorkspaceOffset = OVERRIDEWORKSPACEOFFSET ? WORKSPACE->m_renderOffset->value() : Vector2D{};

    params.window->positionAnimation()->value() = params.monitor->m_position + params.windowBox.pos() / params.monitor->m_scale - params.window->presentation().floatingOffset();
    params.window->sizeAnimation()->value()     = params.windowBox.size() / params.monitor->m_scale;
    params.window->presentation().setAnimatingIn(true);
    COverviewAnimatedVariableAccess::setBeingAnimated(params.window->sizeAnimation().get(), true);
    if (OVERRIDEWORKSPACEOFFSET)
        WORKSPACE->m_renderOffset->value() = {};

    auto restoreWindowGeometry = Hyprutils::Utils::CScopeGuard([&] {
        params.window->positionAnimation()->value() = previousWindowPos;
        params.window->sizeAnimation()->value()     = previousWindowSize;
        params.window->presentation().setAnimatingIn(previousAnimatingIn);
        COverviewAnimatedVariableAccess::setBeingAnimated(params.window->sizeAnimation().get(), previousSizeAnimating);
        if (OVERRIDEWORKSPACEOFFSET && WORKSPACE)
            WORKSPACE->m_renderOffset->value() = previousWorkspaceOffset;
    });

    const size_t firstWindowPassElement = Render::IHyprRenderer::currentPass(g_pHyprRenderer->context()).m_passElements.size();
    const bool   usePrecomputedBlur     = shouldUsePrecomputedBlur(params.window, params.monitor, params.workspaceBox, &params.windowBox, params.dragged);
    // Keep the workspace presentation so floating offsets, alpha and blur match the window.
    g_pHyprRenderer->renderWindow(g_pHyprRenderer->context(), params.window, params.monitor, params.window->presentation().renderPresentation(dynamicPointerCast<Workspace::CWorkspacePresentable>(WORKSPACE)), params.now, false, Render::RENDER_PASS_ALL, false, false);
    if (backdropScope) {
        // The normal surface pass adds its own backdrop markers. Here that would
        // capture the bar and shadow again, contaminating blur along their edges.
        Render::IHyprRenderer::currentPass(ctx).removeAllOfType("CBackdropScopePassElement");
    }
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
        renderOverviewWindowBorder(params.monitor, params.window, params.windowBox, metrics, params.selected);

    raiseWindowPopups(params.window, firstWindowPassElement);
    OverviewRender::flushPass(g_pHyprRenderer->context(), params.monitor);
}

} // namespace OverviewWindow
