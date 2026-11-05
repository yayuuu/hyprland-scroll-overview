# Wallpaper layers

The overview draws the wallpaper behind the workspace cards. The plugin finds it by looking at the layer-shell surfaces on each monitor. A surface counts as the wallpaper when it is mapped, covers the whole monitor, and has no exclusive zone. The `wallpaper_layer` option controls which layers are searched: `auto` (default), `background`, or `bottom`.

Other `BACKGROUND` and `BOTTOM` surfaces (widgets) are drawn on each card in every wallpaper mode.

To see what the plugin detects, run with `SCROLLOVERVIEW_DEBUG_WALLPAPER=1` and read the compositor's stderr. Each overview open logs the wallpaper and widget surfaces for the monitor.

## Shell layer usage

Layers were checked in a running session with `hyprctl layers`, on a headless monitor at 1920x1080.

| Shell | Surface (namespace) | Layer | Exclusive zone | Plugin result |
|---|---|---|---|---|
| swaybg | `wallpaper` | BACKGROUND | -1 (full monitor) | Wallpaper with `auto` and `background`. Widget with `bottom`. |
| hyprpaper 0.8.5 | `hyprpaper` | BACKGROUND | -1 (full monitor) | Wallpaper with `auto` and `background`. Widget with `bottom`. |
| noctalia 5.4.1 | `noctalia-wallpaper` | BACKGROUND | -1 (full monitor) | Wallpaper with `auto` and `background`. Widget with `bottom`. |
| noctalia 5.4.1 | `noctalia-bar-default` | TOP | 34 | Excluded. Not a wallpaper. TOP surfaces are never drawn as widgets. |
| quickshell (full-monitor surface) | `quickshell-wallpaper` | BOTTOM | -1 (full monitor) | Wallpaper with `auto` and `bottom`. Widget with `background`. |
| dux-shell 2.0.0 (`dms run`) | `quickshell` | BACKGROUND | -1 (full monitor) | Wallpaper with `auto` and `background`. Widget with `bottom`. |
| dux-shell 2.0.0 (`dms run`) | `dms:bar` | TOP | 44 | Excluded. Not a wallpaper. TOP surfaces are never drawn as widgets. |
| waybar | `waybar` | TOP | 30 | Excluded. Not a wallpaper. TOP surfaces are never drawn as widgets. |

Notes:

- Each monitor is checked separately. With two monitors, each monitor uses its own wallpaper surface.
- When several full-monitor surfaces are on the same monitor, all of them are listed as wallpapers. With hyprpaper and noctalia running together, the frame showed hyprpaper's image, the surface on top.
- If no surface matches, the plugin draws no wallpaper, and Hyprland's own background shows through.
