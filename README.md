# ScrollOverview

ScrollOverview is an overview plugin like niri.

https://github.com/user-attachments/assets/7ab51651-7901-44d4-b906-357f4c2869c1

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

## Configuration

### Hyprlang

```ini
# .config/hypr/hyprland.conf
plugin {
    scrolloverview {
        gesture_distance = 300 # how far is the "max" for the gesture
        scale = 0.5 # preferred overview scale
        workspace_gap = 100
        layout = vertical # vertical or horizontal
        wallpaper = 0 # 0: global only, 1: per-workspace only, 2: both
        blur = false # blur only the main overview wallpaper

        shadow {
            enabled = false
            range = 50
            render_power = 3
            color = rgba(1a1a1aee)
        }
    }
}
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
            layout = "vertical", -- vertical or horizontal
            wallpaper = 0, -- 0: global only, 1: per-workspace only, 2: both
            blur = false, -- blur only the main overview wallpaper

            shadow = {
                enabled = false,
                range = 50,
                render_power = 3,
                color = 0xee1a1a1a,
            },
        },
    },
})
```

In Lua, `shadow.color` must be an integer color value or a gradient (refer to the Hyprland's docs for the specific format). The Hyprlang-only
`rgba(...)` syntax is not accepted there.

### Properties

| property         | type   | description                                                            | default |
| ---------------- | ------ | ---------------------------------------------------------------------- | ------- |
| gesture_distance | number | how far is the max for the gesture                                     | `200`   |
| scale            | float  | overview scale, [0.1 - 0.9]                                            | `0.5`   |
| workspace_gap    | number | gap between visible workspaces in the overview, in pixels              | `0`     |
| layout           | string | overview layout: `vertical` or `horizontal`                           | `vertical` |
| wallpaper        | int    | wallpaper mode: `0` global only, `1` per-workspace only, `2` both      | `0`     |
| blur             | bool   | blur the main overview wallpaper without blurring workspace wallpapers | `false` |

#### Subcategory `input`

| property | type | description | default |
| --- | --- | --- | --- |
| left_handed | int | swap left and right mouse button actions in overview: `0` disabled, `1` enabled | `input:left_handed` |
| scroll_event_delay | number | in ms, delay between scroll events (to prevent multiple activation) | `200` |
| touchpad_scroll_factor | float | overview touchpad workspace scroll distance multiplier | `1` |
| scrolling_mode | int | mouse wheel behavior: `0` layout-aware default, `1` inverted, `2` vertical scroll changes workspace and horizontal scroll changes columns, `3` vertical scroll changes columns and horizontal scroll changes workspace | `0` |
| drag_mode | int | mouse drag behavior: `0` main button drags windows and middle button pans scrolling workspaces, `1` main button pans scrolling workspaces and middle button drags windows | `0` |
| drag_threshold | int | movement threshold in pixels before a mouse press becomes drag/pan/resize instead of click; `0` disables the threshold | `10` |

#### Subcategory `shadow`

Controls the shadow around each workspace card. `enabled` defaults to `false`; all other unset values fall back to `decoration:shadow:*`.
| property | type | description | default |
| --- | --- | --- | --- |
| enabled | bool | draw a shadow around each workspace card | false |
| range | int | shadow range in layout px | `decoration:shadow:range` |
| render_power | int | shadow falloff power | `decoration:shadow:render_power` |
| color | color or gradient | shadow color or gradient | `decoration:shadow:color` |

### Binding

#### Hyprlang

```bash
# hyprland.conf
bind = MODIFIER, KEY, scrolloverview:overview, OPTION
```

Example:

```bash
# This will toggle ScrollOverview when SUPER+g is pressed
bind = SUPER, g, scrolloverview:overview, toggle
```

#### Lua

```lua
-- hyprland.lua
hl.bind("SUPER + g", function()
    hl.plugin.scrolloverview.overview("toggle all")
end)
```

`hl.plugin.scrolloverview.overview("toggle")` returns a dispatcher function
accepted by `hl.dispatch()` and binds. When called from inside a Lua keybind
callback, it dispatches immediately.

#### Submap

Defining a `scrolloverview` submap replaces the built-in keyboard navigation in
the overview. While the submap is active, normal Hyprland keybinds outside that
submap are not handled unless they use Hyprland's `submap_universal` flag. This method allows greater configuration of the keybinds, for example it is possible to close the window under the mouse cursor with a click.

```ini
# hyprland.conf
submap = scrolloverview
    bind = , left,   scrolloverview:navigate, left
    bind = , right,  scrolloverview:navigate, right
    bind = , up,     scrolloverview:navigate, up
    bind = , down,   scrolloverview:navigate, down
    bind = , return, scrolloverview:overview, select
    bind = , escape, scrolloverview:overview, off
    bind = , mouse:272, scrolloverview:overview, select # selects the workspace under the cursor, multiple actions are not possible with Hyprlang
    bind = , mouse:274, scrolloverview:window, close
submap = reset

# Example Hyprland bind that keeps working inside the submap:
bind = ALT, 1, workspace, 1, submap_universal
bind = ALT, 2, workspace, 2, submap_universal
bind = ALT, 3, workspace, 3, submap_universal
```

```lua
-- hyprland.lua
hl.define_submap("scrolloverview", function()
    hl.bind("left",   hl.plugin.scrolloverview.navigate("left"))
    hl.bind("right",  hl.plugin.scrolloverview.navigate("right"))
    hl.bind("up",     hl.plugin.scrolloverview.navigate("up"))
    hl.bind("down",   hl.plugin.scrolloverview.navigate("down"))
    hl.bind("return", hl.plugin.scrolloverview.overview("select"))
    hl.bind("escape", hl.plugin.scrolloverview.overview("off"))
    hl.bind("mouse:272", function()
        -- Select the clicked window, or just the workspace if no window was clicked, then close the overview. This is the default behaviour if submap is not defined.
        hl.plugin.scrolloverview.overview("select")
        hl.plugin.scrolloverview.window("select")
        hl.plugin.scrolloverview.overview("off")
    end, { mouse = true })
    hl.bind("mouse:274", hl.plugin.scrolloverview.window("close"), { mouse = true })
end)

-- Example Hyprland bind that keeps working inside the submap:
for i = 1, 10 do
    local key = i % 10
    hl.bind("ALT + " .. key, hl.dsp.focus({ workspace = i }), { submap_universal = true })
end
```

### Gesture

The overview can be opened/closed with a trackpad swipe gesture. Configure it as follows.

#### Hyprlang

```ini
# hyprland.conf
scrolloverview-gesture = 3, up, overview        # 3-finger up swipe
scrolloverview-gesture = 4, up, overview, mod:SUPER, scale:1.5
scrolloverview-gesture = 4, up, overview, disable_inhibit   # fire even when an app inhibits gestures
scrolloverview-gesture = 3, up, unset           # remove a previously set gesture
scrolloverview-gesture = 4, horizontal, navigate # continuous carousel navigation
```

| name                   | description                                                                          | arguments       |
| ---------------------- | ------------------------------------------------------------------------------------ | --------------- |
| scrolloverview-gesture | same as gesture, but for ScrollOverview gestures. Supports: `overview`, `navigate`.  | Same as gesture |

#### Lua

Call `hl.plugin.scrolloverview.gesture{ ... }` from your Lua config.

```lua
-- hyprland.lua
hl.plugin.scrolloverview.gesture({ fingers = 3, direction = "vertical" })
hl.plugin.scrolloverview.gesture({ fingers = 4, direction = "vertical", mod = "SUPER", scale = 1.5 })
hl.plugin.scrolloverview.gesture({ fingers = 4, direction = "vertical", disable_inhibit = true })
hl.plugin.scrolloverview.gesture({ fingers = 3, direction = "vertical", action = "unset" })
hl.plugin.scrolloverview.gesture({ fingers = 4, direction = "horizontal", action = "navigate" })
```

| field     | type   | description                                              | default    |
| --------- | ------ | -------------------------------------------------------- | ---------- |
| fingers   | number | finger count (2–9)                                       | required   |
| direction | string | swipe direction (`up`, `down`, `left`, `right`, …)       | required   |
| action    | string | `overview` to open/close, `navigate` to scroll the carousel, `unset` to remove | `overview` |
| mod       | string | modifier mask held during the gesture (e.g. `SUPER`)     | none       |
| scale     | number | gesture delta scale, `[0.1 – 10]`                        | `1.0`      |
| disable_inhibit | bool | fire the gesture even when an app inhibits gestures   | `false`    |

#### The `navigate` action

`action = "navigate"` moves the carousel continuously while the overview is open: the cards
track the finger 1:1 and snap to the nearest one on release, instead of stepping a whole
workspace at a time.

Hyprland's gesture manager refuses two gestures on the same finger count and axis, so a
`navigate` gesture cannot coexist with a native `gesture = 4, horizontal, workspace`. It does
not need to: when no overview is open, `navigate` forwards to a native workspace swipe, so one
binding covers both cases. Register it *instead of* the native gesture:

```lua
-- NOT hl.gesture({ fingers = 4, direction = "horizontal", action = "workspace" })
hl.plugin.scrolloverview.gesture({ fingers = 4, direction = "horizontal", action = "navigate" })
```

Two config values tune it:

| property                | type  | description                                                        | default |
| ----------------------- | ----- | ------------------------------------------------------------------ | ------- |
| input:navigate_factor   | float | carousel travel per px of finger travel                            | `1.0`   |
| input:navigate_invert   | bool  | move the cards against the fingers instead of with them            | `false` |

### Other Lua Examples

Set layout per monitor before opening the overview:

```lua
hl.bind(mainMod .. " + Tab", function()
    local monitor = hl.get_active_monitor()
    local layout = monitor and monitor.name == "DP-1" and "vertical" or "horizontal"
    hl.config({ plugin = { scrolloverview = { layout = layout } } })
    hl.plugin.scrolloverview.overview("toggle")
end)
```

### Dispatchers

Dispatchers can be used from Hyprland binds as `scrolloverview:<dispatcher>` or
from Lua as `hl.plugin.scrolloverview.<dispatcher>(...)`.

#### `scrolloverview:overview`

Controls the overview.

| option | description |
| --- | --- |
| `toggle [monitor\|all]` | toggle the active monitor, a named monitor, or every monitor |
| `select` | select the workspace under the cursor |
| `close [monitor\|all]` | close every overview by default, or only the named monitor |
| `off [monitor\|all]` | same as `close` |
| `disable` | same as `off` |
| `open [monitor\|all]` | open on the active monitor, a named monitor, or every monitor |
| `on [monitor\|all]` | same as `open` |
| `enable` | same as `on` |

For example, `toggle DP-1` targets only `DP-1`, while `toggle all` opens all
missing overviews or closes them when they are all already open. A bare
`toggle` still targets only the active monitor, and a bare `close` closes all
open overviews.

#### `scrolloverview:navigate`

Moves the overview selection/focus between windows in the current workspace. If
there are no more windows in that direction, it selects the next workspace when
the direction matches the configured overview layout.

| option | description |
| --- | --- |
| `left` | move selection left |
| `right` | move selection right |
| `up` | move selection up |
| `down` | move selection down |

#### `scrolloverview:window`

Acts on an overview window. Mouse binds use the window under the cursor;
keyboard binds use the currently selected window.

| option | description |
| --- | --- |
| `select` | focus/select the window under the mouse cursor |
| `close` | close the window under the mouse cursor |

### Supported plugins

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
