#pragma once

#define WLR_USE_UNSTABLE

#include "globals.hpp"
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <unordered_map>
#include <vector>

#include "IOverview.hpp"

class CMonitor;

class CScrollOverview : public IOverview {
  public:
    CScrollOverview(PHLWORKSPACE startedOn_, bool swipe = false);
    virtual ~CScrollOverview();

    virtual void render();
    virtual void damage();
    virtual void onDamageReported();
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
    void   redrawWorkspace(PHLWORKSPACE w, bool forcelowres = false);
    void   redrawAll(bool forcelowres = false);
    void   onWorkspaceChange();
    void   renderWorkspaceLive(PHLWORKSPACE workspace, const Time::steady_tp& now);
    void   renderWindowLive(PHLWINDOW window, float workspaceYOffset, const Time::steady_tp& now);
    void   moveViewportWorkspace(bool up);
    void   moveWindowSelection(const std::string& direction);
    void   rememberSelection(PHLWINDOW window);
    void   syncSelectionToViewport();
    void   syncFocusedSelection();
    void   forceSurfaceVisibility(SP<CWLSurfaceResource> surface);
    void   forceWindowSurfaceVisibility(PHLWINDOW window);
    void   forceWindowVisible(PHLWINDOW window);
    void   forceLayersAboveFullscreen();
    void   restoreForcedSurfaceVisibility();
    void   restoreForcedWindowVisibility();
    void   restoreForcedLayerVisibility();
    size_t activeWorkspaceIndex() const;

    bool   damageDirty              = false;
    size_t viewportCurrentWorkspace = 0;
    bool   rebuildPending           = false;
    bool   workspaceSyncPending     = false;

    struct SWindowImage {
        PHLWINDOWREF            pWindow;
        CFramebuffer            fb;
        UP<CHyprSignalListener> windowCommit;
        Vector2D                lastWindowPosition, lastWindowSize;
    };

    void redrawWindowImage(SP<SWindowImage>);

    struct SWorkspaceImage {
        PHLWORKSPACE                  pWorkspace;
        CBox                          box;
        std::vector<SP<SWindowImage>> windowImages;
    };

    CFramebuffer                     backgroundFb;
    CFramebuffer                     floatingFb;

    Vector2D                         lastMousePosLocal = Vector2D{};

    PHLWINDOWREF                     closeOnWindow;

    std::vector<SP<SWorkspaceImage>> images;
    SP<SWorkspaceImage>              imageForWorkspace(PHLWORKSPACE w);
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
    CHyprSignalListener             keyboardKeyHook;
    CHyprSignalListener             workspaceCreatedHook;
    CHyprSignalListener             workspaceRemovedHook;
    CHyprSignalListener             workspaceActiveHook;

    bool                             swipe             = false;
    bool                             swipeWasCommenced = false;

    friend class CScrollOverviewPassElement;
};

inline std::unique_ptr<CScrollOverview> g_pScrollOverview;
