#include "OverviewPassElement.hpp"
#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#define private public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#undef private
#include <hyprutils/utils/ScopeGuard.hpp>
#include "IOverview.hpp"

using Render::GL::g_pHyprOpenGL;

static CRegion roundedRectRegion(const CBox& box, int rounding, float roundingPower) {
    const auto ROUNDEDBOX = box.copy().round();
    const int  x          = sc<int>(ROUNDEDBOX.x);
    const int  y          = sc<int>(ROUNDEDBOX.y);
    const int  w          = sc<int>(ROUNDEDBOX.width);
    const int  h          = sc<int>(ROUNDEDBOX.height);

    if (w <= 0 || h <= 0)
        return {};

    const int radius = std::clamp(rounding, 0, std::min(w, h) / 2);
    if (radius <= 0)
        return CRegion{ROUNDEDBOX};

    CRegion    region;
    const auto power = std::max(roundingPower, 0.1F);

    auto insetForRow = [radius, h, power](int row) {
        const double rowCenter = row + 0.5;
        double       dy        = 0.0;

        if (rowCenter < radius)
            dy = radius - rowCenter;
        else if (rowCenter > h - radius)
            dy = rowCenter - (h - radius);

        if (dy <= 0.0)
            return 0;

        const double radiusPow = std::pow(radius, power);
        const double inner     = std::pow(std::max(0.0, radiusPow - std::pow(dy, power)), 1.0 / power);
        return std::clamp(sc<int>(std::ceil(radius - inner)), 0, radius);
    };

    int runStart = 0;
    int runInset = insetForRow(0);

    auto addRun = [&](int from, int to, int inset) {
        if (to <= from)
            return;

        const int width = w - inset * 2;
        if (width <= 0)
            return;

        region.add(CBox{x + inset, y + from, width, to - from});
    };

    for (int row = 1; row < h; ++row) {
        const int inset = insetForRow(row);
        if (inset == runInset)
            continue;

        addRun(runStart, row, runInset);
        runStart = row;
        runInset = inset;
    }

    addRun(runStart, h, runInset);

    return region;
}

CScrollOverviewPassElement::CScrollOverviewPassElement() {
    ;
}

void CScrollOverviewPassElement::draw(const CRegion& damage) {
    g_pScrollOverview->fullRender();
}

bool CScrollOverviewPassElement::needsLiveBlur() {
    return false;
}

bool CScrollOverviewPassElement::needsPrecomputeBlur() {
    return false;
}

std::optional<CBox> CScrollOverviewPassElement::boundingBox() {
    if (!g_pScrollOverview->pMonitor)
        return std::nullopt;

    return CBox{{}, g_pScrollOverview->pMonitor->m_size};
}

CRegion CScrollOverviewPassElement::opaqueRegion() {
    if (!g_pScrollOverview->pMonitor)
        return CRegion{};

    return CBox{{}, g_pScrollOverview->pMonitor->m_size};
}

COverviewShadowPassElement::COverviewShadowPassElement(const SData& data_) : data(data_) {
    ;
}

void COverviewShadowPassElement::draw(const CRegion& damage) {
    if (!data.monitor || data.fullBox.width < 1 || data.fullBox.height < 1 || data.range <= 0 || data.color.a == 0.F || data.alpha <= 0.F)
        return;

    CRegion shadowDamage = damage.copy().intersect(data.fullBox);
    if (data.ignoreWindow)
        shadowDamage.subtract(roundedRectRegion(data.cutoutBox, data.rounding + 1, data.roundingPower));

    if (shadowDamage.empty())
        return;

    const auto SAVEDDAMAGE = g_pHyprRenderer->m_renderData.damage;
    g_pHyprRenderer->m_renderData.damage = shadowDamage;
    auto restoreDamage = Hyprutils::Utils::CScopeGuard([SAVEDDAMAGE] { g_pHyprRenderer->m_renderData.damage = SAVEDDAMAGE; });

    auto color = data.color;
    color.a *= data.alpha;

    static auto* const* PGLOBALRENDERPOWER = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(SCROLLOVERVIEW_HANDLE, "decoration:shadow:render_power")->getDataStaticPtr();

    const auto PREVRENDERPOWER = data.renderPower > 0 ? std::optional<int>(**PGLOBALRENDERPOWER) : std::nullopt;
    if (data.renderPower > 0)
        **PGLOBALRENDERPOWER = data.renderPower;
    auto restoreRenderPower = Hyprutils::Utils::CScopeGuard([PREVRENDERPOWER] {
        if (PREVRENDERPOWER)
            **PGLOBALRENDERPOWER = *PREVRENDERPOWER;
    });

    if (data.sharp)
        g_pHyprOpenGL->renderRect(data.fullBox, color, {.damage = &shadowDamage, .round = data.rounding, .roundingPower = data.roundingPower});
    else
        g_pHyprOpenGL->renderRoundedShadow(data.fullBox, data.rounding, data.roundingPower, data.range, color, 1.F);
}

bool COverviewShadowPassElement::needsLiveBlur() {
    return false;
}

bool COverviewShadowPassElement::needsPrecomputeBlur() {
    return false;
}

std::optional<CBox> COverviewShadowPassElement::boundingBox() {
    const auto MONITOR = data.monitor.lock();
    if (!MONITOR)
        return std::nullopt;

    return data.fullBox.copy().scale(1.F / MONITOR->m_scale).round();
}

CRegion COverviewShadowPassElement::opaqueRegion() {
    return {};
}
