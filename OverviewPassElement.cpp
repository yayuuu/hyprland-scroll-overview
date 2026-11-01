#include "OverviewPassElement.hpp"
#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <helpers/Color.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#define private public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#undef protected
#undef private
#include <hyprutils/utils/ScopeGuard.hpp>
#include "Config.hpp"
#include "IOverview.hpp"

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

std::vector<UP<IPassElement>> CScrollOverviewPassElement::draw(Render::CRenderContext& ctx) {
    if (const auto overview = activeScrollOverview())
        overview->fullRender();
    return {};
}

bool CScrollOverviewPassElement::needsLiveBlur(Render::CRenderContext& ctx) {
    return false;
}

bool CScrollOverviewPassElement::needsPrecomputeBlur(Render::CRenderContext& ctx) {
    return false;
}

std::optional<CBox> CScrollOverviewPassElement::boundingBox(Render::CRenderContext& ctx) {
    const auto overview = activeScrollOverview();
    if (!overview || !overview->pMonitor)
        return std::nullopt;

    return CBox{{}, overview->pMonitor->m_size};
}

CRegion CScrollOverviewPassElement::opaqueRegion(Render::CRenderContext& ctx) {
    const auto overview = activeScrollOverview();
    if (!overview || !overview->pMonitor)
        return CRegion{};

    return CBox{{}, overview->pMonitor->m_size};
}

COverviewShadowPassElement::COverviewShadowPassElement(const SData& data_) : data(data_) {
    ;
}

std::vector<UP<IPassElement>> COverviewShadowPassElement::draw(Render::CRenderContext& ctx) {
    const auto MONITOR = data.monitor.lock();
    const bool HASVISIBLECOLOR = std::ranges::any_of(data.color.m_colors, [](const CHyprColor& color) { return color.a > 0.F; });
    if (!MONITOR || data.fullBox.width < 1 || data.fullBox.height < 1 || data.range <= 0 || !HASVISIBLECOLOR || data.alpha <= 0.F)
        return {};

    CRegion shadowDamage = ctx.m_data.damage.copy().intersect(data.fullBox);
    if (data.ignoreWindow)
        shadowDamage.subtract(roundedRectRegion(data.cutoutBox, data.rounding + 1, data.roundingPower));

    if (shadowDamage.empty())
        return {};

    auto savedDrawState = ctx.saveDrawState();
    ctx.m_data.damage   = shadowDamage;
    ctx.m_data.currentWindow.reset();

    std::optional<int> previousRenderPower;
    if (data.renderPower > 0) {
        previousRenderPower = ScrollOverview::Config::getValue<int>("decoration:shadow:render_power");
        ScrollOverview::Config::setValue("decoration:shadow:render_power", data.renderPower);
    }

    auto restoreRenderPower = Hyprutils::Utils::CScopeGuard([previousRenderPower] {
        if (!previousRenderPower)
            return;

        ScrollOverview::Config::setValue("decoration:shadow:render_power", *previousRenderPower);
    });

    // Overview shadows use precomputed boxes and no current window, so no workspace presentation is needed.
    Render::GL::g_pHyprOpenGL->renderRoundedShadow(ctx, data.fullBox, data.rounding, data.roundingPower, data.range, data.color, std::clamp(data.alpha, 0.F, 1.F), {});

    return {};
}

bool COverviewShadowPassElement::needsLiveBlur(Render::CRenderContext& ctx) {
    return false;
}

bool COverviewShadowPassElement::needsPrecomputeBlur(Render::CRenderContext& ctx) {
    return false;
}

std::optional<CBox> COverviewShadowPassElement::boundingBox(Render::CRenderContext& ctx) {
    const auto MONITOR = data.monitor.lock();
    if (!MONITOR)
        return std::nullopt;

    return data.fullBox.copy().scale(1.F / MONITOR->m_scale).round();
}

CRegion COverviewShadowPassElement::opaqueRegion(Render::CRenderContext& ctx) {
    return {};
}
