#pragma once
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/time/Time.hpp>

#include <string>
#include <vector>

class CWLSurfaceResource;

class IOverview {
  public:
    IOverview()          = default;
    virtual ~IOverview() = default;

    virtual void  render()           = 0;
    virtual void  damage()           = 0;
    virtual void  requestInputFrame() = 0;
    virtual void  onDamageReported() = 0;
    virtual bool  shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface) = 0;
    virtual bool  shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now) = 0;
    virtual bool  shouldAllowRealtimePreviewSchedule() = 0;
    virtual bool  shouldSuppressRenderDamage() const = 0;
    virtual void  onPreRender()      = 0;

    virtual void  setClosing(bool closing) = 0;

    virtual void  resetSwipe()                = 0;
    virtual void  onSwipeUpdate(double delta) = 0;
    virtual void  onSwipeEnd()                = 0;

    virtual void  close()                  = 0;
    virtual void  dismissTransient()                                = 0;
    virtual bool  isClosing() const        = 0;
    virtual void  reopen()                 = 0;
    virtual void  selectHoveredWorkspace() = 0;
    virtual bool  moveSelection(const std::string& direction) = 0;
    virtual bool  windowDispatcherAction(const std::string& action) = 0;

    virtual void  fullRender() = 0;

    bool          blockDamageReporting   = false;

    PHLMONITORREF pMonitor;
    bool          m_isSwiping = false;
};

const std::vector<SP<IOverview>>& scrollOverviews();
SP<IOverview>                     scrollOverviewForMonitor(PHLMONITOR monitor);
SP<IOverview>                     scrollOverviewAt(const Vector2D& point);
SP<IOverview>                     activeScrollOverview();
void                              closeAll();
void                              registerScrollOverview(const SP<IOverview>& overview);
void                              unregisterScrollOverview(IOverview* overview);
void                              clearScrollOverviews();
void                              markCrossMonitorDragSession(IOverview* source, const SP<IOverview>& destination);
bool                              closeCrossMonitorDragSession();
void                              removeFromCrossMonitorDragSession(IOverview* overview);

// Current interaction/render context. The authoritative state is the registry.
inline SP<IOverview> g_pScrollOverview;
