# ScrollOverview

This fork's `arm64-native-gestures` branch adds a tested ARM64 hook backend and
continuous vertical zoom gestures for Hyprland 0.56.2. See [ARM64 build and usage](docs/ARM64.md).
The original upstream plugin and attribution are retained below.

ScrollOverview is an overview plugin like niri.

https://github.com/user-attachments/assets/e5eb1ad2-79bc-492a-82cd-02cd8b960d3e

## Installation

### Using Hyprpm (recommended)

1. Add the plugin repository:
   ```bash
   hyprpm add https://github.com/yayuuu/hyprland-scroll-overview.git
   ```
   If you use a Git build of Hyprland, add the plugin from the `new-release` branch instead:
   ```bash
   hyprpm add https://github.com/yayuuu/hyprland-scroll-overview origin/new-release
   ```
2. Build and fetch dependencies:
   ```bash
   hyprpm update
   ```
3. Enable the plugin:
   ```bash
   hyprpm enable scrolloverview
   ```
4. Configure and Enjoy.

For more installation methods, including building from source and loading the plugin automatically, see the [Installation guide](https://github.com/yayuuu/hyprland-scroll-overview/wiki/Installation).

## Configuration

### Hyprlang

```ini
# .config/hypr/hyprland.conf
plugin {
    scrolloverview {
        gesture_distance = 300 # how far is the "max" for the gesture
        scale = 0.5 # preferred overview scale
        workspace_gap = 100
        layout = vertical # vertical, horizontal, or auto (per-monitor orientation)
        wallpaper = 2 # 0: global only, 1: per-workspace only, 2: both
        blur = true # blur only the main overview wallpaper

        shadow {
            enabled = true
            range = 50
        }
    }
}

# Toggle ScrollOverview with SUPER+g
bind = SUPER, g, scrolloverview:overview, "toggle all"
```

### Lua

```lua
-- .config/hypr/hyprland.lua
hl.config({
    plugin = {
        scrolloverview = {
            gesture_distance = 300, -- how far is the "max" for the gesture
            scale = 0.5, -- preferred overview scale
            workspace_gap = 100,
            layout = "vertical", -- vertical, horizontal, or auto (per-monitor orientation)
            wallpaper = 2, -- 0: global only, 1: per-workspace only, 2: both
            blur = true, -- blur only the main overview wallpaper

            shadow = {
                enabled = true,
                range = 50,
            },
        },
    },
})

-- Toggle ScrollOverview with SUPER+g
hl.bind("SUPER + g", function()
    hl.plugin.scrolloverview.overview("toggle all")
end)
```

Set `cross_monitor_drag = true` to drag windows between monitors through ScrollOverview and adopt Hyprland move drags when another overview is open. It defaults to `false`. Native adoption uses Hyprland internals; if unavailable, overview-origin cross-monitor dragging still works.

This setting does not affect overview-origin drags with `toggle all` because every overview is already open. Existing multi-monitor commands remain available when it is disabled.

## Documentation

The complete documentation is available in the [ScrollOverview wiki](https://github.com/yayuuu/hyprland-scroll-overview/wiki). It covers all configuration options, keybinds, submaps, gestures, dispatchers, and advanced Lua examples.

## Supported plugins

- `hyprbars`

<br>
<br>

## Star History

<a href="https://www.star-history.com/?repos=yayuuu%2Fhyprland-scroll-overview&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=yayuuu/hyprland-scroll-overview&type=date&theme=dark&legend=top-left&sealed_token=dnoTQqdWontVtW1U-Qv5yJAQVYpafoKJ0ytKAwVNsCxYO8lor7ZbsD4oVIRHBW_9LjjRPMV-wHAXTw70cn4SbI_2OIrdFEMbOyjv9GVHi9EOBFIgwzKwvMTGWBhkYg5q1f9oV6mQqQZFgec__fA790Nj_OS5WOyVjeHFwceIJRFBlgSvBWyGMn3WUqym" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=yayuuu/hyprland-scroll-overview&type=date&legend=top-left&sealed_token=dnoTQqdWontVtW1U-Qv5yJAQVYpafoKJ0ytKAwVNsCxYO8lor7ZbsD4oVIRHBW_9LjjRPMV-wHAXTw70cn4SbI_2OIrdFEMbOyjv9GVHi9EOBFIgwzKwvMTGWBhkYg5q1f9oV6mQqQZFgec__fA790Nj_OS5WOyVjeHFwceIJRFBlgSvBWyGMn3WUqym" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=yayuuu/hyprland-scroll-overview&type=date&legend=top-left&sealed_token=dnoTQqdWontVtW1U-Qv5yJAQVYpafoKJ0ytKAwVNsCxYO8lor7ZbsD4oVIRHBW_9LjjRPMV-wHAXTw70cn4SbI_2OIrdFEMbOyjv9GVHi9EOBFIgwzKwvMTGWBhkYg5q1f9oV6mQqQZFgec__fA790Nj_OS5WOyVjeHFwceIJRFBlgSvBWyGMn3WUqym" />
 </picture>
</a>

## Workspace ID reordering on Hyprland 0.56

Open overviews watch workspace ID changes, re-sort their cards and animate them
with the existing workspace movement animation. Repeated swaps start from the
current displayed positions. Selection stays with the workspace/window object.

ID changes in one Lua callback are coalesced at Wayland idle, so temporary swap
IDs are never presented to consumers. For Quickshell versions that do not handle
`changeworkspaceid`, the plugin sends one `configreloaded` IPC notification to
refresh their existing monitor/workspace/toplevel models. This does not reload
Hyprland's config or change the bar widget. It also works with the overview closed.

## Desktop workspace-swap feedback

The existing plugin also provides short render-only swap effects, without
opening an interactive overview or changing keyboard/mouse focus. There is
one plugin to load. Full overview and feedback share `layout` and
`workspace_gap`; the foreground/current card is rendered last in both views.
Neighboring cards pass behind it, with off-screen cards culled by geometry.

Lua configuration example (inside `hl.config`):

```lua
plugin = { scrolloverview = {
  workspace_gap = 100,
  swap_effect = "nudge",
  swap_effect_up = "",
  swap_effect_down = "",
  swap_nudge = 0.06,
  swap_zoom = 0.05,
  swap_duration = 280,
} }
```

`swap_effect` selects `nudge`, `zoom`, or `none`. Direction overrides default
to the empty string, which inherits the default effect. Clear both overrides
to use a single mode in both directions. Nudge is a fraction of the monitor's
layout axis (0–0.25); zoom is the fractional scale reduction (0–0.4). Duration
is in milliseconds (80–1000). Amount zero disables that effect.

The zoom returns smoothly to scale 1, and the nudge returns to zero. A peek is
part of the shared scene, not another effect or plugin. Its visible extent
depends on the configured gap and amount. All intersecting neighboring cards
are eligible to render, including further neighbors when the view reveals them.
The top/overlay shell layers remain at their original scale and position.

The feedback scene installs no input listeners or submap and restores its
render-only visibility state when finished. Repeated swaps restart from the
current visual state; opening a real overview replaces it. Normal workspace
switches, workspace swipes and locking interrupt it. Animations-disabled,
scratchpad, non-swap ID changes and swaps on other monitors do not start it.

Validation on Hyprland 0.56.2 / ARM64: native build and isolated runs checked
both modes, unchanged active workspace/window and input options, cleanup,
repeated swaps, normal-workspace interruption, and full-overview takeover.
Colored-window captures confirmed the foreground ordering in both swap
directions.

Swap feedback uses cosine ease-in-out: the foreground nudge/zoom has zero
velocity at departure, its turning point, and return; passing background cards
ease into and out of their travel. Both halves keep the configured duration
and maximum displacement.

The foreground now uses 30% of its time to move out, holds at maximum
displacement for 30%, and returns over the final 40%. Each moving segment
uses cosine ease-in-out with zero velocity at its ends. Background cards
continue passing underneath during the hold. At 280 ms this is 84 ms out,
84 ms held, and 112 ms back.

The latest feedback envelope replaces the timed hold with one continuous
rounded crest. It slows almost to rest around the maximum without a flat
plateau, then falls back over a longer portion of the animation. Quintic
easing gives zero velocity and acceleration at departure and finish. The
configured displacement and total duration are unchanged.

The foreground nudge now moves opposite to the logical workspace reorder,
as though dragged by the background workspace passing beneath it: an upward
swap nudges the foreground down, and a downward swap nudges it up. Amount,
rounded-crest easing, duration and background travel are unchanged.
