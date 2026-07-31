#!/usr/bin/env bash
# scroll-overview FORK dev helper (mirrors ~/Projects/hyprtasking/dev.sh).
#
#   ./dev.sh build     just compile the fork
#   ./dev.sh test      build + load the FORK build (this session only)
#   ./dev.sh restore   unload the fork and go back to the hyprpm build
#
# The fork adds `action = "navigate"` to scrolloverview.gesture, used by
# ~/.config/hypr/modules/plugins.lua. Stock scrolloverview raises on that action, but the
# call is last inside a pcall'd block, so restore does not produce a config error -- the
# 4-finger horizontal swipe just goes dead until modules/input.lua's native gesture is
# put back.
set -uo pipefail

DIR="$HOME/Projects/scroll-overview"
DEV_SO="$DIR/scrolloverview.so"
STOCK_SO="/var/cache/hyprpm/$USER/hyprland-scroll-overview/scrolloverview.so"

build() { make -C "$DIR"; }

unload_both() {
  hyprctl plugin unload "$STOCK_SO" >/dev/null 2>&1 || true
  hyprctl plugin unload "$DEV_SO"   >/dev/null 2>&1 || true
  # A beat between unload and load, or the load can be processed first and become a no-op, leaving
  # the old build live. `sleep` is not available in every environment this is driven from (it is
  # blocked in the agent harness), and its absence is what made that failure silent -- `read -t` is
  # a shell builtin and always works.
  read -t 0.4 -r _ < /dev/null 2>/dev/null || true
}

case "${1:-test}" in
  build) build ;;

  test|load|reload)
    build || { echo "build failed -- not loading"; exit 1; }
    # The version string is git-hash+dirty, so it does NOT change between dirty builds: the only
    # proof a reload happened is the plugin HANDLE changing. Without this check dev.sh happily
    # reported success while the previous build stayed live, and hours went into "fixing" code that
    # was never running.
    before=$(hyprctl plugin list | awk '/scrolloverview/{f=1} f&&/Handle:/{print $2; exit}')
    unload_both
    hyprctl plugin load "$DEV_SO" || { echo "load failed"; exit 1; }
    hyprctl reload >/dev/null 2>&1
    after=$(hyprctl plugin list | awk '/scrolloverview/{f=1} f&&/Handle:/{print $2; exit}')
    if [ -z "$after" ]; then
      echo "ERROR: plugin is not loaded after load -- check the Hyprland log"; exit 1
    fi
    if [ -n "$before" ] && [ "$before" = "$after" ]; then
      echo "ERROR: handle unchanged ($after) -- the OLD build is still live, your changes are NOT running"; exit 1
    fi
    echo "FORK loaded (handle $after). 4-finger up opens the overview, 4-finger horizontal navigates it."
    ;;

  restore)
    unload_both
    hyprctl plugin load "$STOCK_SO" || { echo "load failed"; exit 1; }
    hyprctl reload >/dev/null 2>&1
    echo "hyprpm build restored."
    ;;

  *) echo "usage: dev.sh {build|test|restore}"; exit 1 ;;
esac
