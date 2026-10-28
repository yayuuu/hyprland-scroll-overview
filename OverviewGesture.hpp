#pragma once

#include <hyprland/src/managers/input/trackpad/gestures/ITrackpadGesture.hpp>
#include "IOverview.hpp"
#include "GestureProgress.hpp"
#include <vector>

class COverviewGesture : public ITrackpadGesture {
  public:
    COverviewGesture(bool closeOnly = false, bool all = false) : m_closeOnly(closeOnly), m_all(all) {}
    ~COverviewGesture() override = default;

    void begin(const ITrackpadGesture::STrackpadGestureBegin& e) override;
    void update(const ITrackpadGesture::STrackpadGestureUpdate& e) override;
    void end(const ITrackpadGesture::STrackpadGestureEnd& e) override;

  private:
    bool m_closeOnly = false;
    bool m_all = false;
    struct Entry { WP<IOverview> overview; bool startedOpen; };
    std::vector<Entry> m_entries;
    GestureProgress m_progress;
    bool  m_firstUpdate = false;

};
