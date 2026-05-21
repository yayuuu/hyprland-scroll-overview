#pragma once

// Hyprland 0.55 moved several CHyprRenderer render entry points into the
// protected section of Render::IHyprRenderer. A derived class may legally
// name a protected base member to form a pointer-to-member; invoking
// through that pointer performs no access check. Standard-compliant C++,
// and avoids the `#define protected public` shim.

#include <hyprland/src/render/Renderer.hpp>

namespace ScrollOverview::RendererAccess {
    struct SExposed : public Render::IHyprRenderer {
        using Render::IHyprRenderer::renderBackground;
        using Render::IHyprRenderer::renderLayer;
        using Render::IHyprRenderer::renderWindow;
        using Render::IHyprRenderer::shouldBlur;
    };

    inline void renderBackground(PHLMONITOR monitor) {
        (g_pHyprRenderer.get()->*&SExposed::renderBackground)(monitor);
    }

    inline void renderLayer(PHLLS layer, PHLMONITOR monitor, const Time::steady_tp& now, bool popups = false, bool lockscreen = false) {
        (g_pHyprRenderer.get()->*&SExposed::renderLayer)(layer, monitor, now, popups, lockscreen);
    }

    inline void renderWindow(PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& now, bool decorate, Render::eRenderPassMode mode, bool ignorePosition = false, bool standalone = false) {
        (g_pHyprRenderer.get()->*&SExposed::renderWindow)(window, monitor, now, decorate, mode, ignorePosition, standalone);
    }

    inline bool shouldBlur(PHLWINDOW window) {
        return (g_pHyprRenderer.get()->*static_cast<bool (Render::IHyprRenderer::*)(PHLWINDOW)>(&SExposed::shouldBlur))(window);
    }
}
