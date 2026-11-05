`hl.plugin.scrolloverview.configure(options)` accepts a table of the ScrollOverview
properties listed in [Basic configuration](Basic-configuration.md). Without `output`, it is shorthand for
`hl.config({ plugin = { scrolloverview = options } })` and keeps the same global
configuration behavior. You can use either form for global settings.

With `output`, it stores overrides for that monitor. This is useful when two
landscape monitors should use different overview layouts:

```lua
-- Global settings inherited by all monitors.
hl.plugin.scrolloverview.configure({
    layout = "auto",
    scale = 0.5,
    workspace_gap = 100,
    shadow = { enabled = true, range = 50 },
})

-- Only these fields differ on DP-1.
hl.plugin.scrolloverview.configure({
    output = "DP-1",
    layout = "vertical",
    input = { scrolling_mode = 2 },
})

-- HDMI-A-2 still inherits workspace_gap and shadow.range from global settings.
hl.plugin.scrolloverview.configure({
    output = "HDMI-A-2",
    layout = "horizontal",
    scale = 0.4,
    shadow = { enabled = false },
})

hl.bind("SUPER + g", function()
    overview.overview("toggle all")
end)
```

- `output` must be a non-empty string matching the exact connector name from
  `hyprctl monitors`, such as `DP-1` or `HDMI-A-2`. It does not accept `all`,
  wildcards, or monitor-description selectors. A monitor may be disconnected when
  its settings are configured; its overrides apply when it is connected.
- Every overview property except `submap_name` can be overridden, including
  individual `input` and `shadow` fields. Hyprland options outside ScrollOverview
  remain global.
- Missing fields inherit the current global value.
- Repeated calls for the same output merge fields - the last supplied value for a
  field wins. An empty subtable leaves previous overrides unchanged.
- On config reload, monitor overrides are cleared and rebuilt from the config.
  Remove an override and reload to return that field to its global setting.
- Monitor-specific values use Hyprland's value parsers and the option ranges
  listed below. Unknown fields or invalid values reject the entire monitor-specific
  call without applying any of its changes.
- These settings are used for keybinds, gestures, and overviews opened during
  window dragging. `cross_monitor_drag` is read from the monitor where the drag
  starts; it does not block dropping into an overview that is already open.
- Layout is chosen when an overview is created. Close and reopen existing
  overviews after changing configuration to apply the complete new setup.
  `auto` retains its usual behavior: horizontal on portrait monitors, vertical on
  landscape monitors.

`output` is a selector for `configure()` only. Putting it inside
`hl.config({ plugin = { scrolloverview = ... } })` does not create a monitor rule.