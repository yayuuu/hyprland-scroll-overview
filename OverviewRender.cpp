#include "OverviewRender.hpp"
#include <chrono>
#include <cmath>
#include <sstream>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>

namespace OverviewRender {

void flushPass(PHLMONITOR monitor) {
    if (!monitor)
        return;

    auto& pass = Render::IHyprRenderer::currentPass(g_pHyprRenderer->context());
    if (pass.empty())
        return;

    pass.render(g_pHyprRenderer->context(), CRegion{CBox{{}, monitor->m_transformedSize}});
    pass.clear();
}

void queueBlur(Render::CRenderContext& ctx, const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur) {
    if (alpha <= 0.F)
        return;

    if (box.empty())
        return;

    CRectPassElement::SRectData data;
    data.box           = box;
    data.color         = CHyprColor{0.F, 0.F, 0.F, 0.F};
    data.round         = rounding;
    data.roundingPower = roundingPower;
    data.blur          = true;
    data.blurA         = alpha;
    data.xray          = usePrecomputedBlur;
    Render::IHyprRenderer::currentPass(ctx).add(makeUnique<CRectPassElement>(data));
}

void renderBlur(Render::CRenderContext& ctx, PHLMONITOR monitor, const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur) {
    if (!monitor)
        return;

    queueBlur(ctx, box, rounding, roundingPower, alpha, usePrecomputedBlur);
    flushPass(monitor);
}

}
