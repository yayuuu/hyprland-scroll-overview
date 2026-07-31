#pragma once

#include <hyprland/src/managers/input/trackpad/gestures/ITrackpadGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/WorkspaceSwipeGesture.hpp>
#include "IOverview.hpp"

// Continuous carousel navigation inside the overview.
//
// Hyprland's gesture manager refuses two gestures on the same finger count + axis, so this
// cannot coexist with a native `gesture = N, horizontal, workspace`. Instead, when no overview
// is open the gesture forwards to a native workspace swipe, so one binding covers both:
// carousel while the overview is up, plain workspace swipe on the desktop.
class COverviewNavigateGesture : public ITrackpadGesture {
  public:
    COverviewNavigateGesture()           = default;
    ~COverviewNavigateGesture() override = default;

    void begin(const ITrackpadGesture::STrackpadGestureBegin& e) override;
    void update(const ITrackpadGesture::STrackpadGestureUpdate& e) override;
    void end(const ITrackpadGesture::STrackpadGestureEnd& e) override;

    bool isDirectionSensitive() override;

  private:
    WP<IOverview>          m_overview;
    bool                   m_fallbackActive = false;
    CWorkspaceSwipeGesture m_fallback;
};
