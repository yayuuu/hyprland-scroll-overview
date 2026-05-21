#include "OverviewRender.hpp"
#include <chrono>
#include <cmath>
#include <sstream>
#define private public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>
#include <hyprland/src/helpers/MonitorResources.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#undef private

using Render::GL::g_pHyprOpenGL;
using Render::GL::CHyprOpenGLImpl;

namespace OverviewRender {

void flushPass(PHLMONITOR monitor) {
    if (!monitor || g_pHyprRenderer->m_renderPass.empty())
        return;

    g_pHyprRenderer->m_renderPass.render(CRegion{CBox{{}, monitor->m_transformedSize}});
    g_pHyprRenderer->m_renderPass.clear();
}

void renderBlur(PHLMONITOR monitor, const CBox& windowBox, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur) {
    if (!monitor || alpha <= 0.F)
        return;

    CRegion blurDamage{windowBox};
    if (blurDamage.empty())
        return;

    CRegion drawDamage{CBox{{}, monitor->m_transformedSize}};

    const auto SAVEDFB        = g_pHyprRenderer->m_renderData.currentFB;
    const auto MONITORRES     = monitor->resources().lock();
    const auto PRECOMPUTEDFB  = (usePrecomputedBlur && MONITORRES) ? MONITORRES->m_blurFB : nullptr;

    SP<Render::ITexture> BLURREDTEXTURE;
    if (PRECOMPUTEDFB)
        BLURREDTEXTURE = PRECOMPUTEDFB->getTexture();
    else if (g_pHyprRenderer->m_renderData.mainFB)
        BLURREDTEXTURE = g_pHyprRenderer->blurMainFramebuffer(alpha, &blurDamage);

    if (SAVEDFB)
        SAVEDFB->bind();

    if (!BLURREDTEXTURE)
        return;

    CBox transformedBox = windowBox;
    transformedBox.transform(Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform)), monitor->m_transformedSize.x, monitor->m_transformedSize.y);

    const CBox monitorSpaceBox = {transformedBox.pos().x / monitor->m_pixelSize.x * monitor->m_transformedSize.x,
                                  transformedBox.pos().y / monitor->m_pixelSize.y * monitor->m_transformedSize.y,
                                  transformedBox.width / monitor->m_pixelSize.x * monitor->m_transformedSize.x,
                                  transformedBox.height / monitor->m_pixelSize.y * monitor->m_transformedSize.y};

    CHyprOpenGLImpl::STextureRenderData renderData;
    renderData.damage                               = &drawDamage;
    renderData.a                                    = alpha;
    renderData.round                                = rounding;
    renderData.roundingPower                        = roundingPower;
    renderData.allowCustomUV                        = true;
    renderData.allowDim                             = false;

    g_pHyprRenderer->pushMonitorTransformEnabled(true);
    const auto SAVEDRENDERMODIF                     = g_pHyprRenderer->m_renderData.renderModif;
    const auto SAVEDUVTOPLEFT                       = g_pHyprRenderer->m_renderData.primarySurfaceUVTopLeft;
    const auto SAVEDUVBOTTOMRIGHT                   = g_pHyprRenderer->m_renderData.primarySurfaceUVBottomRight;
    g_pHyprRenderer->m_renderData.renderModif         = {};
    g_pHyprRenderer->m_renderData.primarySurfaceUVTopLeft     = monitorSpaceBox.pos() / monitor->m_transformedSize;
    g_pHyprRenderer->m_renderData.primarySurfaceUVBottomRight = (monitorSpaceBox.pos() + monitorSpaceBox.size()) / monitor->m_transformedSize;
    g_pHyprOpenGL->renderTexture(BLURREDTEXTURE, windowBox, renderData);
    g_pHyprRenderer->m_renderData.primarySurfaceUVTopLeft     = SAVEDUVTOPLEFT;
    g_pHyprRenderer->m_renderData.primarySurfaceUVBottomRight = SAVEDUVBOTTOMRIGHT;
    g_pHyprRenderer->m_renderData.renderModif                 = SAVEDRENDERMODIF;
    g_pHyprRenderer->popMonitorTransformEnabled();
}

}
