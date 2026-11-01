#pragma once

#include "globals.hpp"

namespace OverviewRender {

void flushPass(PHLMONITOR monitor);
void queueBlur(Render::CRenderContext& ctx, const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur);
void renderBlur(Render::CRenderContext& ctx, PHLMONITOR monitor, const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur);

}
