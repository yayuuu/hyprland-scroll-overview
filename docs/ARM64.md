# ARM64 fork for Hyprland 0.56.2

This branch retains ScrollOverview's rendering and interactions on ARM64.
It uses Dobby for ARM function hooks while keeping Hyprland's native x86_64
backend. No Hyprland replacement or patch is needed. Upstream attribution and
BSD-3-Clause license remain unchanged; Dobby is an Apache-2.0 build dependency.

Validated on an Apple M1 Pro with Hyprland commit
`efb50993780079460b0cbed1363e2166a2de1d9f` (0.56.2). Other versions/architectures
have not been validated. Native x86 code is retained, but this fork's changes
were exercised on ARM. Keep the compositor's ABI check; rebuild matching headers
after updates.

## Build

Install Hyprland's normal plugin build dependencies plus CMake and Git. Prepare
matching headers with `hyprpm update`, then run:

```bash
# Substitute your prepared header-cache path if needed.
export PKG_CONFIG_PATH="/var/cache/hyprpm/$USER/headersRoot/share/pkgconfig"
make
```

On ARM, `make` builds pinned Dobby revision
`2f49a2ab60ca65cb8add43f60bda20e78bd6ab86`, runs hook/gesture lifecycle tests, then
builds `scrolloverview.so`. `DOBBY_SOURCE` can select an existing pristine pinned
checkout. It never installs or loads the plugin. For installation, the normal
hyprpm manifest invokes the same Makefile; direct user-owned plugin loading is
also possible. Do not keep two ScrollOverview builds enabled simultaneously.

The tested Dobby revision is older because its current latest source did not
build on Linux. Relocation allocations are retained across hook operations;
sustained-session memory use and additional hardware still need evaluation.
This is a working prototype, not a claim of production stability.

## Native gestures

Register these through the optional Lua interface after removing conflicting
four-finger vertical registrations. Keep horizontal gestures separately:

```lua
hl.plugin.scrolloverview.native_gesture({ fingers = 4, direction = "up", action = "overview_all" })
hl.plugin.scrolloverview.native_gesture({ fingers = 4, direction = "down", action = "overview_close_all" })
```

Up toggles across monitors with swipe-driven scaling. Down closes existing
overviews only. Short/reversed swipes restore the starting state; reaching 35%
of `gesture_distance` completes on release. Cancelled events restore the starting
state. Existing `overview` gestures and keyboard dispatchers remain available.

Three-finger workspace swipes belong to Hyprland's own `workspace` gesture,
not this plugin. For natural vertical movement, use a signed `vertical` gesture
with `workspace_swipe_invert=true` and vertical workspace animations. Disable
workspace creation and `workspace_swipe_use_r` for existing monitor-local slots.

## Tests and development

Standalone tests cover repeated hook install/original-call/removal, floating
point and PC-relative relocation, duplicate ownership, cleanup and gesture
reversal/cancellation. Nested compositor tests exercised real windows, gesture
updates and plugin reload/unload. Physical gestures and long-session stability
need testing on each receiving system.

`-DSCROLLOVERVIEW_TESTING=ON` adds a synthetic Lua gesture probe for disposable
compositor tests only. Production builds omit it. CMake's explicit ARM build
accepts `DOBBY_SOURCE` and `DOBBY_LIBRARY`; Meson is not the ARM build route yet.
