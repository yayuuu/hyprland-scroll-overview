#pragma once

#include "IOverview.hpp"

struct SOverviewOpenResult {
    SP<IOverview> overview;
    bool          created = false;
};

SOverviewOpenResult openOverview(PHLMONITOR monitor, bool swipe = false);
bool                adoptNativeWindowDragIntoOverview();
