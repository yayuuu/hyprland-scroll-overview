# scrolloverview visual / UX audit — 2026-07-31

Run against Hyprland 0.56.1, fork `navigate-gesture`, monitor eDP-1 2880x1800 @ scale 1.5,
config `scale = 0.35`, `workspace_gap = 60`, `wallpaper = 0`, `shadow.enabled = false`,
`layout = horizontal`.

Method: scenarios driven with `dotool` (held-button drags, real motion pacing) + `grim`
captures + `hyprctl -j` state diffs, on four throwaway workspaces (ids 20/30/40 with `class =
audit` terminals) so nothing depended on the live session. Screenshots in this directory are
horizontal strips through the card band (y 520..1280 physical).

Severity: **A** = looks broken / loses work, **B** = confusing, **C** = polish.

---

## F1 — An empty workspace card renders nothing at all ✓ FIXED (fork) — severity A

**This is the "the workspace disappeared, I have to close and reopen the overview" bug.**

A workspace with no windows paints *nothing*: no background, no border, no label. With
`wallpaper = 0` the card area shows the global wallpaper, and with `shadow.enabled = false`
there is not even a shadow to outline it. An empty workspace is therefore pixel-identical to
the gap between two cards, and to the dead space past the ends of the carousel.

Repro A — empty out the centred workspace while the overview is open:
1. open the overview on a workspace with one window
2. `CTRL+SUPER+SHIFT+→` (or drag the window to another card)
3. the source workspace stays alive because it is still active, but its card vanishes

Repro B — make a new workspace while the overview is open:
1. open the overview, press `SUPER+N` (`ws-index.sh new`)
2. the new workspace is created, focused and centred — and the middle of the screen is bare
   wallpaper. See `s7_newws_settled.png`: the only thing on screen is one card at the far
   left; everything else reads as "the overview broke".

Evidence: `s3_died_settled.png` (centred card is invisible), `s7_newws_settled.png`.

Cause: `CScrollOverview::renderWorkspaceBackground()` only ever emits a shadow (config-gated),
the per-workspace wallpaper (`wallpaper != 0`) and bottom layer-shell surfaces. Nothing draws
the card itself, so a card is only visible by virtue of the windows inside it.

Fixed: every card now paints a plate — a low-alpha fill plus a border, under the windows,
`plugin:scrolloverview:card_plate` (default on). Original note kept below.

Fix: always paint a card plate — a low-alpha fill plus a 1–2px border at
the workspace box, under the windows. That also fixes counting cards for positional
navigation, and makes an empty card distinguishable from an insert slot (which *does* draw an
outline — see `s10_scroll_2000ms.png`). Config-only workarounds that help but do not solve it:
`wallpaper = 1` or `2` (per-workspace wallpaper) or `shadow.enabled = true`.

---

## F2 — Drag auto-scroll ran backwards ✓ FIXED (fork) — severity A

Dragging a window into the *trailing* screen edge scrolled the carousel toward *earlier*
workspaces, at ~1500 px/s, until it hit the first workspace. In a live test the dragged window
crossed the whole carousel and was dropped onto an unrelated workspace 4 cards away, and the
snap-on-release then made *that* workspace active.

Cause: `viewOffset` is subtracted when placing cards, so a positive offset reveals *later*
workspaces; `updateDragAutoScroll()` had the sign the other way round.

Fixed: sign corrected, and the top speed reduced from 24 to 14 rendered px per 16 ms tick
(~0.8 of a card per second) so the carousel does not run away while you aim.

---

## F3 — Window previews are blank for the first ~300 ms after opening ✗ OPEN — severity B

At 150 ms after opening, cards are empty or half-drawn; by ~500 ms they are populated
(`s1_150ms.png` vs `s1_settled.png`). Clients that must repaint to produce a frame only get
throttled frame callbacks: `shouldAllowSurfaceFrame()` sends non-selected workspaces to
`scheduleRealtimePreviewFrame()`, i.e. `misc:render_unfocused_fps`. Combined with F1 the first
frames of an overview can look completely empty, which is probably a second contributor to
"I have to close and reopen it".

Possible fix: on open, allow one unthrottled frame callback per visible window before falling
back to the `render_unfocused_fps` budget.

---

## F4 — Nothing marks the active / selected card ✓ PARTLY FIXED (fork) — severity B

The card plate from F1 now draws the selected card with a brighter, thicker edge, which is the
first cue of any kind. Still no marker on the selected *window* inside a card.

Originally: there is no highlight, border, tint or dot on the active workspace or the selected window —
being horizontally centred is the only cue, and that cue is invisible while the carousel is
mid-animation or when the centred workspace is empty (F1). Keyboard selection (bare arrows,
`moveSelection()`) therefore has no visible effect on an empty card.

---

## F5 — Cards carry no workspace identity ✗ OPEN — severity C

No number, name or index is drawn on a card. Harmless with absolute ids and muscle memory, but
navigation here is now positional (`SUPER+N` = Nth existing workspace), so the user has to
count cards — and F1 makes the count wrong whenever an empty workspace sits in the row.

---

## F6 — A refused insert gives no feedback ✓ FIXED (fork) — severity B

Hyprland orders workspaces by numeric id, so an insert between two numerically adjacent
workspaces (7 and 8) has no free number and is refused: no slot opens, and on release the
window simply flies home. Nothing says why. The keyboard path (`ws-index.sh insert`) at least
sends a `notify-send`; the drag path is silent.

Fixed: a blocked slot is now drawn in the bare gap in a red tint, without moving the cards, so
the gesture is acknowledged. Also largely designed out: `ws-index.sh respace` renumbers the open
workspaces onto a step of 10 (run once on this machine: 4,5,7,8,9,77 → 10,20,30,40,50,60), and
new workspaces are allocated on the same step, so gaps normally have nine free numbers. Before
that respace, 3 of 4 gaps on this machine were refused — which is why drag-to-insert "worked for
the test rig but not for the user".

---

## Verified working (no bug)

- Switching workspaces with the overview open re-centres the carousel and keeps every card
  live (`s2_switch_settled`).
- A window opened while the overview is open appears in its card within ~1 frame
  (`s6_window_opened`); a window closed updates the same way (`s5_window_closed`).
- A *non-active* workspace emptied while the overview is open is destroyed and its card is
  removed without leaving a stale slot (`s8_other_ws_died`).
- Drag-to-insert: gap with a free id → new workspace at the midpoint; gap without → refused,
  overview stays open; past the last card → new workspace on the +10 step; before the first →
  new workspace below. All four confirmed by `hyprctl` state diffs.
- Cards spread apart by exactly half a pitch either side of the hovered slot, and the slot
  placeholder outline is clearly legible (`s10_scroll_2000ms.png`).
- No plugin errors in the Hyprland log across the whole run.

---

## F7 — Only ~2.6 of N workspaces can ever be on screen ✓ FIXED (fork) — severity A

**The second half of "the workspace disappeared".** At `scale = 0.35` a card is 1008 px and the
pitch is 1098 px, so a 2880 px screen holds 2.6 cards. With 7 workspaces, 4 were off-screen at
all times, with nothing on screen to say they existed, and switching slid more of them out.
Reopening the overview does not help because it re-centres on the same three.

Measured before the fix, active = 40:

    ws 10  x  -2358..-1350  OFF SCREEN     ws 42  x  2034..3042  on screen
    ws 20  x  -1260..  -252 OFF SCREEN     ws 52  x  3132..4140  OFF SCREEN
    ws 30  x   -162..  846  on screen      ws 60  x  4230..5238  OFF SCREEN
    ws 40  x    936.. 1944  on screen (active)

Fixed: the configured scale is now a maximum, not a constant. `CScrollOverview::targetScale()`
shrinks it so every workspace fits, down to `plugin:scrolloverview:min_scale` (0.18) past which
the carousel scrolls as before; `plugin:scrolloverview:adaptive_scale` turns it off. Re-applied
whenever a workspace appears or disappears, and animated, so cards glide to the new size. With 6
workspaces this puts 5 on screen at once instead of 2.6.

Note the count comes from the workspace registry, not `images` — the first cut read `images`,
which is still empty while the overview is being constructed, so the fit silently never applied
on open.
