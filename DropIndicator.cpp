#include "DropIndicator.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#define private public
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#undef private

namespace {

static constexpr float INDICATOR_BASE_SIZE_PX = 200.F;
static constexpr float INDICATOR_ALPHA   = 0.3F;

static CHyprColor indicatorColor() {
    auto* const ACTIVEBORDER = sc<Config::CGradientValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("general:col.active_border").ptr());
    if (!ACTIVEBORDER || ACTIVEBORDER->m_colors.empty())
        return Colors::WHITE.modifyA(INDICATOR_ALPHA);

    return ACTIVEBORDER->m_colors[0].modifyA(INDICATOR_ALPHA);
}

static bool isHorizontalSide(const std::string& side) {
    return side == "l" || side == "r";
}

static bool isVerticalSide(const std::string& side) {
    return side == "u" || side == "d";
}

static Layout::Tiled::CScrollingAlgorithm* scrollingAlgorithmForWorkspace(const PHLWORKSPACE& workspace) {
    if (!workspace || !workspace->m_space || !workspace->m_space->algorithm())
        return nullptr;

    return dc<Layout::Tiled::CScrollingAlgorithm*>(workspace->m_space->algorithm()->m_tiled.get());
}

static bool scrollingPrimaryHorizontal(const PHLWORKSPACE& workspace) {
    const auto ALGO = scrollingAlgorithmForWorkspace(workspace);
    if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller)
        return true;

    return ALGO->m_scrollingData->controller->isPrimaryHorizontal();
}

static CBox scrollingIndicatorBox(const CDropIndicator::SRenderParams& params) {
    if (!params.anchor.window)
        return params.workspaceUsableBox;

    const bool  HORIZONTALSIDE = isHorizontalSide(params.anchor.direction);
    const bool  PRIMARYHORIZONTAL = scrollingPrimaryHorizontal(params.workspace);
    const float MONITOR_SCALE = std::max<float>(params.monitor ? params.monitor->m_scale : 1.F, 0.01F);
    const float SIZE = INDICATOR_BASE_SIZE_PX * std::max(params.renderScale, 0.01F) * MONITOR_SCALE;
    const auto& EDGEBOX = params.anchor.logicalBox.empty() ? params.anchor.box : params.anchor.logicalBox;
    CBox        box;

    if (HORIZONTALSIDE) {
        const float CENTERX = params.anchor.direction == "l" ? EDGEBOX.x : EDGEBOX.x + EDGEBOX.width;
        box.width  = SIZE;
        box.height = PRIMARYHORIZONTAL ? params.workspaceUsableBox.height : params.anchor.box.height;
        box.x      = CENTERX - SIZE / 2.F;
        box.y      = PRIMARYHORIZONTAL ? params.workspaceUsableBox.y : params.anchor.box.y;
    } else {
        const float CENTERY = params.anchor.direction == "u" ? EDGEBOX.y : EDGEBOX.y + EDGEBOX.height;
        box.width  = PRIMARYHORIZONTAL ? params.anchor.box.width : params.workspaceUsableBox.width;
        box.height = SIZE;
        box.x      = PRIMARYHORIZONTAL ? params.anchor.box.x : params.workspaceUsableBox.x;
        box.y      = CENTERY - SIZE / 2.F;
    }

    box.round();
    return box;
}

static CBox indicatorBox(const CDropIndicator::SRenderParams& params) {
    if (!params.anchor.window)
        return params.workspaceUsableBox;

    if (params.anchor.direction.empty())
        return params.anchor.box;

    if (!scrollingAlgorithmForWorkspace(params.workspace))
        return params.anchor.box;

    return scrollingIndicatorBox(params);
}

static CBox clipScrollingIndicatorBox(CBox box, const CDropIndicator::SRenderParams& params) {
    if (!scrollingAlgorithmForWorkspace(params.workspace))
        return box;

    const bool PRIMARYHORIZONTAL = scrollingPrimaryHorizontal(params.workspace);

    if (PRIMARYHORIZONTAL)
        return box.intersection({{box.x, params.workspaceUsableBox.y}, {box.width, params.workspaceUsableBox.height}});

    return box.intersection({{params.workspaceUsableBox.x, box.y}, {params.workspaceUsableBox.width, box.height}});
}

static int indicatorRounding(const CDropIndicator::SRenderParams& params) {
    const float ROUNDING = params.anchor.window ? params.anchor.window->presentation().rounding() : ScrollOverview::Config::getValue<int>("decoration:rounding");
    const float SCALE    = std::max(params.renderScale, 0.01F) * std::max<float>(params.monitor ? params.monitor->m_scale : 1.F, 0.01F);

    return std::max(0, sc<int>(std::round(ROUNDING * SCALE)));
}

static float indicatorRoundingPower(const CDropIndicator::SRenderParams& params) {
    if (params.anchor.window)
        return params.anchor.window->presentation().roundingPower();

    return ScrollOverview::Config::getValue<float>("decoration:rounding_power");
}

}

void CDropIndicator::renderDropIndicator(const SRenderParams& params) {
    if (!params.monitor || !params.workspace)
        return;

    if (!params.anchor.window && params.floating && params.workspaceFullyVisible)
        return;

    if (params.anchor.window && !params.anchor.direction.empty() && !isHorizontalSide(params.anchor.direction) && !isVerticalSide(params.anchor.direction))
        return;

    const auto BOX = clipScrollingIndicatorBox(indicatorBox(params), params);
    if (BOX.empty())
        return;

    CRectPassElement::SRectData data;
    data.box           = BOX;
    data.color         = indicatorColor();
    data.round         = indicatorRounding(params);
    data.roundingPower = indicatorRoundingPower(params);

    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
}
