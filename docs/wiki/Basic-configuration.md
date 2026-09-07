The configuration below is a good starting point for most users. It provides a balanced overview layout with workspace spacing, both wallpaper layers, blur, shadows, and a `SUPER + g` keybind that toggles the overview on all monitors. Adjust the individual values to match your workflow and visual preferences.

## Configuration

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

## Properties

| property | type | description | default |
| --- | --- | --- | --- |
| gesture_distance | number | how far is the max for the gesture | `200` |
| scale | float | overview scale, [0.1–0.9] | `0.5` |
| workspace_gap | number | gap between visible workspaces in the overview, in pixels | `0` |
| layout | string | overview layout: `vertical`, `horizontal`, or `auto`; auto uses horizontal on portrait monitors and vertical on landscape monitors | `vertical` |
| cross_monitor_drag | bool | enable cross-monitor overview dragging and Hyprland move-drag adoption; does not affect `open all`, `toggle all`, or manually opened overviews | `false` |
| wallpaper | int | wallpaper mode: `0` global only, `1` per-workspace only, `2` both | `0` |
| blur | bool | blur the main overview wallpaper without blurring workspace wallpapers | `false` |
| input | structure | input configuration subcategory; accepts a structure containing the properties listed below | see below |
| shadow | structure | shadow configuration subcategory; accepts a structure containing the properties listed below | see below |

### Subcategory `input`

ScrollOverview respects Hyprland's global `input.left_handed` and `input.touchpad.scroll_factor` settings. The `left_handed` override and `touchpad_scroll_factor` multiplier below let you adjust their behavior inside the overview only, without changing the global input configuration.

| property | type | description | default |
| --- | --- | --- | --- |
| left_handed | int | swap left and right mouse button actions in overview: `0` disabled, `1` enabled | `input.left_handed` |
| scroll_event_delay | number | in ms, delay between scroll events (to prevent multiple activation) | `200` |
| touchpad_scroll_factor | float | overview touchpad workspace scroll distance multiplier | `1` |
| scrolling_mode | int | mouse wheel behavior: `0` layout-aware default, `1` inverted, `2` vertical scroll changes workspace and horizontal scroll changes columns, `3` vertical scroll changes columns and horizontal scroll changes workspace | `0` |
| drag_mode | int | mouse drag behavior: `0` main button drags windows and middle button pans scrolling workspaces, `1` main button pans scrolling workspaces and middle button drags windows | `0` |
| drag_threshold | int | movement threshold in pixels before a mouse press becomes drag/pan/resize instead of click; `0` disables the threshold | `10` |

### Subcategory `shadow`

Controls the shadow around each workspace card. `enabled` defaults to `false`; all other unset values fall back to `decoration.shadow.*`.

| property | type | description | default |
| --- | --- | --- | --- |
| enabled | bool | draw a shadow around each workspace card | `false` |
| range | int | shadow range in layout px | `decoration.shadow.range` |
| render_power | int | shadow falloff power | `decoration.shadow.render_power` |
| color | color or gradient | shadow color or gradient | `decoration.shadow.color` |

## Gestures

The overview can be opened and closed with a trackpad swipe gesture.

Call `hl.plugin.scrolloverview.gesture({ ... })` from your Lua configuration.

```lua
-- hyprland.lua
hl.plugin.scrolloverview.gesture({ fingers = 3, direction = "vertical" })
hl.plugin.scrolloverview.gesture({ fingers = 4, direction = "vertical", mod = "SUPER", scale = 1.5 })
hl.plugin.scrolloverview.gesture({ fingers = 4, direction = "vertical", disable_inhibit = true })
hl.plugin.scrolloverview.gesture({ fingers = 3, direction = "vertical", action = "unset" })
```

| field | type | description | default |
| --- | --- | --- | --- |
| fingers | number | finger count (2–9) | required |
| direction | string | swipe direction (`up`, `down`, `left`, `right`, …) | required |
| action | string | `overview` to register, `unset` to remove | `overview` |
| mod | string | modifier mask held during the gesture (e.g. `SUPER`) | none |
| scale | number | gesture delta scale, [0.1–10] | `1.0` |
| disable_inhibit | bool | fire the gesture even when an app inhibits gestures | `false` |

[Back to Configuration](Configuration.md)
