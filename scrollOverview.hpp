#pragma once

#define WLR_USE_UNSTABLE

#include "globals.hpp"
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/types.hpp>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "Config.hpp"
#include "DropIndicator.hpp"
#include "IOverview.hpp"

class CMonitor;
struct wl_event_source;

class CScrollOverview : public IOverview {
  public:
    CScrollOverview(PHLWORKSPACE startedOn_, bool swipe = false, PHLMONITOR monitor = {});
    ~CScrollOverview() override;

    void         render() override;
    void         damage() override;
    void         requestInputFrame() override;
    void         markBlurDirty();
    void         markBackdropBlurDirty();
    void         onDamageReported() override;
    bool         shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface) override;
    bool         shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now) override;
    bool         shouldAllowRealtimePreviewSchedule() override;
    bool         shouldSuppressRenderDamage() const override;
    void         onPreRender() override;

    void         setClosing(bool closing) override;

    void         resetSwipe() override;
    void         onSwipeUpdate(double delta) override;
    void         onSwipeEnd() override;

    void         onNavigateSwipeBegin() override;
    void         onNavigateSwipeUpdate(const Vector2D& delta) override;
    void         onNavigateSwipeEnd() override;

    // close without a selection
    void         close() override;
    void         selectHoveredWorkspace() override;
    bool         moveSelection(const std::string& direction) override;
    bool         windowDispatcherAction(const std::string& action) override;

    void         fullRender() override;

  private:
    void   rebuildWorkspaceImages();
    void   seedRememberedSelections();
    void   redrawAll(bool forcelowres = false);
    void   onWorkspaceChange();
    void   renderGlobalWallpaper(PHLMONITOR monitor, const Time::steady_tp& now);
    void   renderWallpaperLayers(PHLMONITOR monitor, const CBox& workspaceBox, float renderScale, const Time::steady_tp& now, float alpha = 1.F);
    void   updateBackdropBlurCache(PHLMONITOR monitor, int wallpaperMode, const Time::steady_tp& now);
    void   renderBackdropBlurCache(PHLMONITOR monitor);
    void   renderWorkspaceBackground(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now);
    void   renderWorkspaceLive(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now);
    bool   hasVisiblePrecomputedBlurWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale) const;
    void   renderWindowLive(PHLMONITOR monitor, PHLWINDOW window, const CBox& windowBox, float renderScale, const Time::steady_tp& now, const CBox* workspaceBox = nullptr,
                             bool dragged = false);
    void   renderDraggedWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale, const Time::steady_tp& now);
    void   renderPinnedFloatingWindows(PHLMONITOR monitor, float overviewScale, const Time::steady_tp& now);
    void   moveViewportWorkspace(bool up);
    void   trackpadSwipeLayout(const PHLWORKSPACE target, const double delta);
    void   trackpadSwipeWorkspace(const double delta);
    void   followWorkspaceScroll(const double renderedDelta);
    void   finishWorkspaceScrollFollow();
    double trackpadWorkspaceScrollOffset(PHLMONITOR monitor, float renderScale);
    bool   scrollStepAllowed(uint32_t timeMs);
    bool   selectOverviewWindow(PHLWINDOW window, size_t workspaceIdx, bool syncFocus = false);
    bool   selectWindowAtOverviewCursor(bool syncFocus = false);
    PHLWINDOW windowClosestToWorkspaceCenter(size_t workspaceIdx) const;
    void   rememberSelection(PHLWINDOW window);
    void   syncSelectionToViewport();
    void   syncFocusedSelection();
    size_t dragWorkspaceIndex(PHLWINDOW window) const;
    void   updateWorkspaceOverflow();
    CBox   workspaceOverviewVisibleBox(size_t workspaceIdx, const CBox& workspaceBox, float renderScale, PHLMONITOR monitor) const;
    float      workspaceOverviewOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const;
    float      workspaceOverviewLogicalOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const;
    float      workspaceOverviewAlpha(size_t workspaceIdx) const;
    PHLWINDOW windowAtOverviewPoint(const Vector2D& point, size_t* workspaceIdx = nullptr) const;
    PHLWINDOW windowAtOverviewCursor(size_t* workspaceIdx = nullptr);
    PHLWINDOW windowAtOverviewCursorOnWorkspace(size_t workspaceIdx, const PHLWINDOW& ignoredWindow = nullptr, CBox* windowBox = nullptr) const;
    CDropIndicator::SDropAnchor dropAnchorAtOverviewCursorOnWorkspace(size_t workspaceIdx, const PHLWINDOW& ignoredWindow = nullptr,
                                                                      CScrollOverview* dragContext = nullptr);
    PHLWORKSPACE workspaceAtOverviewPoint(const Vector2D& point, size_t* workspaceIdx = nullptr) const;
    PHLWORKSPACE workspaceAtOverviewDropPoint(const Vector2D& point, size_t* workspaceIdx = nullptr, const PHLWINDOW& draggedWindow = nullptr) const;
    PHLWORKSPACE workspaceAtOverviewCursor(size_t* workspaceIdx = nullptr) const;
    Vector2D  overviewPointToGlobal(size_t workspaceIdx, const Vector2D& pointLocal) const;
    CBox      draggedWindowBox(size_t workspaceIdx) const;
    CBox      draggedWindowBoxFor(PHLWINDOW window, size_t workspaceIdx, const Vector2D& pointLocal, const Vector2D& grabRatio) const;
    CBox      draggedWindowGlobalBox() const;
    void      refreshDragOriginalOverviewBoxes();
    void      clearDragPending();
    void      beginWindowDrag(PHLWINDOW window);
    void      updateWindowDrag();
    void      endWindowDrag();
    CBox      resizedWindowBox() const;
    void      beginWindowResize();
    void      updateWindowResize();
    void      endWindowResize();
    void      updateScrollingPan();
    void      beginScrollingPan(PHLWORKSPACE workspace);
    void      endScrollingPan();
    void      focusMostVisibleScrollingWindow(const PHLWORKSPACE& workspace);
    bool      moveScrollingColumnSelection(bool next);
    bool      moveScrollingStackSelection(bool next);
    void   forceSurfaceVisibility(SP<CWLSurfaceResource> surface);
    void   forceWindowSurfaceVisibility(PHLWINDOW window);
    void   forceWindowVisible(PHLWINDOW window);
    void   forceLayersAboveFullscreen();
    void   restoreForcedSurfaceVisibility();
    void   restoreForcedWindowVisibility();
    void   restoreForcedLayerVisibility();
    void   applyWorkspaceAnimationOverrides();
    void   restoreWorkspaceAnimationOverrides();
    void   forceWorkspaceAlphaVisible();
    void   forceWorkspaceWindowsDecoRecalc(const PHLWORKSPACE& workspace);
    void   emitFullscreenVisibilityState(PHLWINDOW window, bool hideFullscreen);
    void   applyInputConfigOverrides();
    void   restoreInputConfigOverrides();
    void   transferSharedStateOwnership();
    size_t activeWorkspaceIndex() const;
    bool   isSelectedWorkspace(const PHLWORKSPACE& workspace) const;
    void   sendOverviewFrameCallbacks(const Time::steady_tp& now);
    bool   isVisibleRealtimePreviewWindow(const PHLWINDOW& window) const;
    bool   hasRunningWorkspaceAnimation() const;
    bool   shouldAllowRealtimePreviewFrame() const;
    void   scheduleMinimumPreviewFrame();
    void   schedulePreviewFrameAfter(std::chrono::milliseconds delay);
    void   scheduleRealtimePreviewFrame();
    void   releaseInputListeners();
    void   activateSubmapIfConfigured();
    void   restoreSubmapIfActive();
    bool   dispatchSubmapMouseClick(uint32_t button);
    static int realtimePreviewTimerCallback(void* data);

    size_t viewportCurrentWorkspace = 0;
    bool   rebuildPending           = false;
    bool   overviewBlurDirty        = true;
    bool   backdropBlurDirty        = true;
    bool   overviewBlurStateValid   = false;
    float  lastOverviewBlurScale    = 1.F;
    int    lastBackdropWallpaperMode = -1;
    Vector2D lastOverviewBlurViewOffset = Vector2D{};
    SP<Render::IFramebuffer> backdropBlurFB;

    struct SWorkspaceImage {
        PHLWORKSPACE              pWorkspace;
        std::vector<PHLWINDOWREF> windows;
        float                     overflowLeft   = 0.F;
        float                     overflowRight  = 0.F;
        float                     overflowTop    = 0.F;
        float                     overflowBottom = 0.F;
    };

    struct SWorkspaceInsertTransition {
        bool                             active              = false;
        WORKSPACEID                      transitionWorkspaceID = WORKSPACE_INVALID;
        bool                             transitionFadeIn   = true;
        std::unordered_map<WORKSPACEID, float> oldRelativeOffsets;
        std::unordered_map<WORKSPACEID, float> newRelativeOffsets;
        float                            transitionOldRelativeOffset = 0.F;
    };

    Vector2D                         lastMousePosLocal = Vector2D{}; // monitor-local pixel space

    PHLWINDOWREF                     closeOnWindow;
    PHLWINDOWREF                     dragActiveWindow;
    PHLWORKSPACEREF                  dragOriginalWorkspace;
    PHLWORKSPACEREF                  scrollingPanWorkspace;
    PHLWINDOWREF                     scrollingPanInitialWindow;
    PHLWINDOWREF                     resizePendingWindow;
    PHLWINDOWREF                     resizeActiveWindow;

    Vector2D                         dragStartMouseLocal   = Vector2D{};
    Vector2D                         dragGrabOffsetLocal   = Vector2D{};
    Vector2D                         dragGrabRatio         = Vector2D{0.5, 0.5};
    Vector2D                         dragOriginalFloatSize = Vector2D{};
    Vector2D                         dragOriginalTapeTranslation = Vector2D{};
    Vector2D                         resizeStartMouseLocal = Vector2D{};
    Vector2D                         resizeLastMouseLocal  = Vector2D{};
    Vector2D                         scrollingPanLastMouseLocal = Vector2D{};
    CBox                             dragOriginalBox            = CBox{};
    CBox                             dragOriginalVisualBox      = CBox{};
    CBox                             dragOriginalOverviewBox    = CBox{};
    CBox                             dragOriginalOverviewHitbox = CBox{};
    CBox                             resizeOriginalBox          = CBox{};
    WORKSPACEID                      focusSyncedFromWorkspaceID = WORKSPACE_INVALID;
    size_t                           resizeWorkspaceIdx     = 0;
    Layout::eRectCorner              resizeCorner           = Layout::CORNER_NONE;
    bool                             dragPendingPrimary    = false;
    bool                             resizePointerDown     = false;
    bool                             scrollingPanPointerDown = false;
    bool                             submapMouseClickPending = false;
    bool                             dragStartedTiled      = false;
    bool                             emittingFullscreenVisibilityState = false;
    bool                             inputConfigOverridden = false;
    bool                             realtimePreviewTimerArmed = false;
    bool                             realtimePreviewFrameQueued = false;
    bool                             selectedWorkspaceFramePending = false;
    bool                             inputFramePending = false;
    bool                             sendingOverviewFrameCallbacks = false;
    bool                             usesSubmapKeybinds = false;
    bool                             submapActive = false;
    uint32_t                         submapMouseClickButton = 0;
    std::string                      previousSubmapName;
    int                              previousNoWarps = 0;
    int                              previousWarpOnChangeWorkspace = 0;
    int                              previousWarpOnToggleSpecial = 0;
    int                              previousWarpBackAfterNonMouseInput = 0;
    int                              previousFollowMouse = 0;

    std::vector<SP<SWorkspaceImage>> images;
    std::vector<PHLWINDOWREF>        pinnedFloatingWindows;
    std::unordered_map<WORKSPACEID, PHLWINDOWREF> rememberedSelection;
    SWorkspaceInsertTransition       workspaceInsertTransition;
    PHLWORKSPACEREF                  pendingRemovedWorkspace;
    ScrollOverview::Config::ELayout  layout = ScrollOverview::Config::ELayout::VERTICAL;

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

    struct SWorkspaceAnimationConfig {
        std::string                                      name;
        Hyprutils::Animation::SAnimationPropertyConfig   config;
    };
    std::vector<SWorkspaceAnimationConfig> savedWorkspaceAnimationConfigs;
    bool                                   workspaceAnimationsOverridden = false;
    bool                                   sharedStateOwner = false;

    PHLWORKSPACE                     startedOn;

    PHLANIMVAR<float>                scale;
    PHLANIMVAR<Vector2D>             viewOffset;
    PHLANIMVAR<float>                workspaceInsertProgress;
    PHLANIMVAR<float>                workspaceInsertFadeProgress;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> workspaceInsertFadeConfig;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> workspaceRemoveFadeConfig;
    Time::steady_tp                  lastRealtimePreviewFrame = {};
    Time::steady_tp                  realtimePreviewTimerDue = {};
    wl_event_source*                 realtimePreviewTimer = nullptr;

    bool                             closing = false;
    bool                             closeApplied = false; // close() has run its teardown; guards against double-invocation

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

    bool                             swipe = false;

    uint32_t                         lastScrollStepTimeMs = 0;      // event time of the last discrete scroll step, for scroll_event_delay throttling

    double                           trackpadScrollAccum          = 0.0;
    bool                             trackpadWorkspaceFollowing   = false;
    bool                             trackpadTapeFollowing        = false;
    bool                             trackpadGestureSettlePending = false;
    double                           trackpadGestureSettleOffset  = 0.0;

    friend class CScrollOverviewPassElement;
};
