#pragma once

#define WLR_USE_UNSTABLE

#include "globals.hpp"
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <chrono>
#include <unordered_map>
#include <vector>

#include "IOverview.hpp"

class CMonitor;
struct wl_event_source;

class CScrollOverview : public IOverview {
  public:
    CScrollOverview(PHLWORKSPACE startedOn_, bool swipe = false);
    virtual ~CScrollOverview();

    virtual void render();
    virtual void damage();
    void         markBlurDirty();
    virtual void onDamageReported();
    virtual bool shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface);
    virtual bool shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now);
    virtual bool shouldAllowRealtimePreviewSchedule();
    virtual bool shouldSuppressRenderDamage() const;
    virtual void onPreRender();

    virtual void setClosing(bool closing);

    virtual void resetSwipe();
    virtual void onSwipeUpdate(double delta);
    virtual void onSwipeEnd();

    // close without a selection
    virtual void close();
    virtual void selectHoveredWorkspace();

    virtual void fullRender();

  private:
    void   rebuildWorkspaceImages();
    void   seedRememberedSelections();
    void   redrawAll(bool forcelowres = false);
    void   onWorkspaceChange();
    void   renderWallpaperLayers(PHLMONITOR monitor, const CBox& workspaceBox, float renderScale, const Time::steady_tp& now);
    void   renderWorkspaceBackground(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now);
    void   renderWorkspaceLive(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now);
    bool   hasVisiblePrecomputedBlurWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale) const;
    void   renderWindowLive(PHLMONITOR monitor, PHLWINDOW window, const CBox& windowBox, float renderScale, const Time::steady_tp& now, const CBox* workspaceBox = nullptr,
                             bool usePrecomputedBlur = false);
    void   renderDraggedWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale, const Time::steady_tp& now);
    void   renderPinnedFloatingWindows(PHLMONITOR monitor, float overviewScale, const Time::steady_tp& now);
    void   moveViewportWorkspace(bool up);
    bool   moveWindowSelection(const std::string& direction);
    void   rememberSelection(PHLWINDOW window);
    void   syncSelectionToViewport();
    void   syncFocusedSelection();
    PHLWINDOW windowAtOverviewCursor(size_t* workspaceIdx = nullptr);
    PHLWINDOW windowAtOverviewCursorOnWorkspace(size_t workspaceIdx, const PHLWINDOW& ignoredWindow = nullptr, CBox* windowBox = nullptr) const;
    PHLWORKSPACE workspaceAtOverviewCursor(size_t* workspaceIdx = nullptr) const;
    Vector2D  overviewPointToGlobal(size_t workspaceIdx, const Vector2D& pointLocal) const;
    CBox      draggedWindowBoxLogical(size_t workspaceIdx) const;
    void      beginWindowDrag();
    void      updateWindowDrag();
    void      endWindowDrag();
    void   forceSurfaceVisibility(SP<CWLSurfaceResource> surface);
    void   forceWindowSurfaceVisibility(PHLWINDOW window);
    void   forceWindowVisible(PHLWINDOW window);
    void   forceLayersAboveFullscreen();
    void   restoreForcedSurfaceVisibility();
    void   restoreForcedWindowVisibility();
    void   restoreForcedLayerVisibility();
    void   emitFullscreenVisibilityState(PHLWINDOW window, bool hideFullscreen);
    void   applyInputConfigOverrides();
    void   restoreInputConfigOverrides();
    size_t activeWorkspaceIndex() const;
    void   sendOverviewFrameCallbacks(const Time::steady_tp& now);
    bool   isVisibleRealtimePreviewWindow(const PHLWINDOW& window) const;
    bool   shouldAllowRealtimePreviewFrame() const;
    void   scheduleMinimumPreviewFrame();
    void   schedulePreviewFrameAfter(std::chrono::milliseconds delay);
    void   scheduleRealtimePreviewFrame();
    static int realtimePreviewTimerCallback(void* data);

    size_t viewportCurrentWorkspace = 0;
    bool   rebuildPending           = false;
    bool   workspaceSyncPending     = false;
    bool   overviewBlurDirty        = true;
    bool   overviewBlurStateValid   = false;
    float  lastOverviewBlurScale    = 1.F;
    Vector2D lastOverviewBlurViewOffset = Vector2D{};

    struct SWorkspaceImage {
        PHLWORKSPACE              pWorkspace;
        std::vector<PHLWINDOWREF> windows;
    };

    Vector2D                         lastMousePosLocal = Vector2D{};

    PHLWINDOWREF                     closeOnWindow;
    PHLWINDOWREF                     dragPendingWindow;
    PHLWINDOWREF                     dragActiveWindow;
    PHLWORKSPACEREF                  dragOriginalWorkspace;

    Vector2D                         dragStartMouseLocal   = Vector2D{};
    Vector2D                         dragOriginalFloatSize = Vector2D{};
    CBox                             dragOriginalBox        = CBox{};
    bool                             dragPointerDown       = false;
    bool                             dragStartedTiled      = false;
    bool                             emittingFullscreenVisibilityState = false;
    bool                             inputConfigOverridden = false;
    bool                             realtimePreviewTimerArmed = false;
    bool                             realtimePreviewFrameQueued = false;
    bool                             sendingOverviewFrameCallbacks = false;
    int                              previousNoWarps = 0;
    int                              previousWarpOnChangeWorkspace = 0;
    int                              previousWarpOnToggleSpecial = 0;
    int                              previousWarpBackAfterNonMouseInput = 0;
    int                              previousFollowMouse = 0;

    std::vector<SP<SWorkspaceImage>> images;
    std::vector<PHLWINDOWREF>        pinnedFloatingWindows;
    std::unordered_map<WORKSPACEID, PHLWINDOWREF> rememberedSelection;

    struct SForcedSurfaceVisibility {
        WP<CWLSurfaceResource> surface;
        CRegion               visibleRegion;
    };
    std::vector<SForcedSurfaceVisibility> forcedSurfaceVisibility;

    struct SForcedWindowVisibility {
        PHLWINDOWREF window;
        bool         hidden = false;
    };
    std::vector<SForcedWindowVisibility> forcedWindowVisibility;

    struct SForcedLayerVisibility {
        PHLLSREF layer;
        bool     aboveFullscreen = true;
        float    alpha           = 1.F;
    };
    std::vector<SForcedLayerVisibility> forcedLayerVisibility;

    PHLWORKSPACE                     startedOn;

    PHLANIMVAR<float>                scale;
    PHLANIMVAR<Vector2D>             viewOffset;
    Time::steady_tp                  lastRealtimePreviewFrame = {};
    Time::steady_tp                  realtimePreviewTimerDue = {};
    wl_event_source*                 realtimePreviewTimer = nullptr;

    bool                             closing = false;

    CHyprSignalListener             mouseMoveHook;
    CHyprSignalListener             mouseButtonHook;
    CHyprSignalListener             touchMoveHook;
    CHyprSignalListener             touchDownHook;
    CHyprSignalListener             mouseAxisHook;
    CHyprSignalListener             windowOpenHook;
    CHyprSignalListener             windowCloseHook;
    CHyprSignalListener             windowMoveHook;
    CHyprSignalListener             windowActiveHook;
    CHyprSignalListener             windowFullscreenHook;
    CHyprSignalListener             keyboardKeyHook;
    CHyprSignalListener             workspaceCreatedHook;
    CHyprSignalListener             workspaceRemovedHook;
    CHyprSignalListener             workspaceActiveHook;

    bool                             swipe = false;

    friend class CScrollOverviewPassElement;
};

inline std::unique_ptr<CScrollOverview> g_pScrollOverview;
