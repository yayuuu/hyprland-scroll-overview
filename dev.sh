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
}

case "${1:-test}" in
  build) build ;;

  test|load|reload)
    build || { echo "build failed -- not loading"; exit 1; }
    unload_both; sleep 0.3
    hyprctl plugin load "$DEV_SO" || { echo "load failed"; exit 1; }
    hyprctl reload >/dev/null 2>&1
    echo "FORK loaded. 4-finger up opens the overview, 4-finger horizontal navigates it."
    hyprctl plugin list | grep -A2 scrolloverview
    ;;

  restore)
    unload_both; sleep 0.3
    hyprctl plugin load "$STOCK_SO" || { echo "load failed"; exit 1; }
    hyprctl reload >/dev/null 2>&1
    echo "hyprpm build restored."
    ;;

  *) echo "usage: dev.sh {build|test|restore}"; exit 1 ;;
esac
