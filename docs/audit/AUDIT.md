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

## F7 — Only ~2.6 of N workspaces can ever be on screen — adaptive built, then TURNED OFF by choice

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

---

## F8 — A workspace created by a drop renders blank ✓ FIXED (fork) — severity A

Drop a window into an insert slot: the new card appears in the right place and stays **empty**,
while `hyprctl` reports the window on that workspace, `mapped: true`, `at [7,7]`, correct size,
monitor eDP-1 — i.e. the state is perfect and only the render is wrong.

Cause: **a workspace that has never been active does not render its windows.** Activating the
new workspace once and reopening the overview showed the window immediately. `endWindowDrag()`
calls `RESTOREACTIVEWORKSPACE()`, so a workspace the drop had just created was never activated.

Fixed: for a drop that created the workspace, follow the window into it instead of restoring the
previous active workspace — which is also where the user is looking, and matches what the
keyboard `ws-index.sh insert` path (`follow = true`) already did. That path was reported as
working, which was the clue.

---

## F9 — An emptied workspace does not collapse ✓ FIXED (fork) — severity A

Move the only window off a workspace while the overview is open and its empty card stays in the
row. Outside the overview that workspace would be destroyed at once.

Cause: `SWorkspaceImage` holds a **strong** `PHLWORKSPACE`, and the render path forces
`m_visible = true`, so Hyprland cannot reap it. Proof: the workspace vanished the instant the
overview was closed.

Fixed: `reapEmptiedWorkspace()` drops it from the carousel, which releases the last reference.
Three details that each cost a test round:

- When the emptied workspace is the **active** one — the common case, since you drag off the card
  you are looking at — focus follows the window out first, otherwise the card you just emptied is
  the one left in front of you.
- The sweep runs **one frame after** `window.moveToWorkspace`, not in the handler: the event fires
  before the window's workspace pointer is updated, so the source still reports a window.
- It runs **only after a move**, never on a plain frame: an empty active workspace is legitimate
  right after opening the overview on one, or after `SUPER+N`. Both verified to survive.

---

## F10 — The insert slot stole drops aimed at a card ✓ FIXED (fork) — severity B

Regression from the insert work. Dragging toward the centre of a neighbouring card created a new
workspace instead: travelling through the gap opened the slot, the spread shifted that card half
a pitch (304 px) out from under the cursor, and the cursor was then inside the open slot.

Fixed: `insertSlotAtOverviewPoint()` hit-tests the **unspread** layout
(`workspaceOverviewOffset(..., withInsertSpread = false)`). The spread is purely visual, so the
geometry the pointer is tested against cannot move as a result of the pointer's own position.

---

## F11 — dev.sh reported success while the OLD build stayed live ✓ FIXED — process, not code

`dev.sh test` unloads and loads the plugin, then printed the version string — which is
`<git-hash>-dirty` and therefore **identical across dirty builds**. The only proof a reload
happened is the plugin **handle** changing. It was not: `hyprctl plugin list` showed the same
handle before and after, so several rounds of "the cascade does not work" were measured against a
plugin that did not contain the cascade.

Root cause: `sleep 0.3` between unload and load. `sleep` is unavailable in the environment the
script was being driven from, and the script continued past it in a state where the load became a
no-op. Fixed by dropping the sleeps and by comparing the handle before/after, failing loudly:

    ERROR: handle unchanged (5...) -- the OLD build is still live, your changes are NOT running

Lesson for this fork: a version string derived from the git hash cannot detect a reload of
uncommitted work. Check the handle.

---

## F12 — Inserting into a gap runs out of numbers ✓ ADDRESSED (spacing), cascade REVERTED

Hyprland orders workspaces by id, every insert takes the midpoint of a gap, so each insert into the
same gap halves what is left. With a step of 10 that gave about three inserts before the gap was
full and the insert refused — which is not dynamic ordering, it is a countdown, and it is what the
user hit.

Attempted fix, **reverted**: shift the following workspaces up by one to make room. Moving the
windows is straightforward, but the vacated workspace OBJECT keeps its id, and while it is active it
cannot be reaped, so the number never frees. Working around that (move focus off it, drop it from
the carousel, rebuild synchronously, then create the wanted id) still produced the wrong outcome:
the dragged window landed on the shifted id with no workspace inserted. Removed rather than left in
— code that runs and gets the wrong answer is worse than no code.

Shipped instead: allocate on a step of **1000** (`ws-index.sh` and the plugin's end-inserts), which
gives ~10 nested inserts per gap by bisection, and `ws-index.sh respace` re-flattens the row onto
1000, 2000, 3000 … whenever it gets tight. Verified: repeated drag-inserts into one gap
(5000|6000 → 5500 → 5750), and the whole row respaced from 1, 10, 25, 60 to 1000, 2000, 3000, 4000
with window order preserved.

---

## F13 — Insert worked on one side of a card but not the other ✓ FIXED (fork) — severity A

Reported as "sometimes the left side drag inserts but the right does not, sometimes the opposite".

`endWindowDrag()` resolved the drop target in the wrong order: `workspaceAtOverviewDropPoint()`
first, and the insert slot only if that came back empty. That card hit-test runs on the **spread**
card positions, and the spread pushes cards away from the open slot in *opposite directions on
either side*, so a card could claim a point the user could plainly see was inside the open slot.
The insert was then skipped and the drag silently became a plain move onto that card — on one side
only, and which side depended on where the slot had opened. Hence "sometimes the opposite".

Fixed: an open, non-blocked slot now takes priority over a card hit. What is drawn under the cursor
is what the drop uses. Verified: left, right, then left again, all inserting (8500 / 9500 / 8750).

---

## F14 — Adaptive scale re-fitted mid-drag ✓ FIXED (fork) — severity B

Raised by the user: "dynamic workspace overview scaling might bite us cause it expands". Correct —
completing an insert adds a workspace, which re-fits the scale (F7), which resizes and reflows every
card while the pointer is still down. Same failure family as the spread moving cards under the
cursor.

Fixed: `onWorkspaceChange()` skips the re-fit while `dragActiveWindow` is set, and
`clearDragPending()` re-fits once, after the gesture ends.

---

## F15 — The first leading insert blocked all later ones ✓ FIXED (fork) — severity A

`insertSlotWorkspaceID(0)` picked `first_id - INSERT_ID_STEP`, clamped to at least 1. With a row
starting at 1000 and a step of 1000 that clamps to exactly **id 1** — so the very first insert
before the first card landed on 1, and from then on the row started at 1 with nothing below it, so
every later leading insert was blocked and drew the red cue. A one-way door, self-inflicted, and it
is what the user hit twice (ids 1 and 250 were not test leftovers after all: 1 came from a leading
insert).

Fixed: the leading slot bisects downward like every interior gap (first_id / 2, then search down and
up), so 1000 -> 500 -> 250 -> ... and id 1 is only reached after ~10 leading inserts.

---

## F7 revisited — adaptive sizing is off by default

Measured on this machine (2880 px wide, gap 60, monitor scale 1.5), fit-all scale by workspace
count: 3 -> 0.29, 4 -> 0.21, 5 -> 0.16, 7 -> 0.11. With any sane legibility floor, **fit-all only
works up to about four workspaces**; past that the floor takes over and the carousel scrolls anyway.
So the cost -- cards resizing whenever a workspace appears or disappears, smaller drop targets,
unreadable previews -- bought very little.

Decision: `adaptive_scale` defaults to **false**. Cards stay at the configured `scale` (0.35 here:
1008 px wide, ~2.6 on screen, previews readable), and the carousel scrolls, which is the position
niri takes with its fixed `zoom`. `adaptive_scale = true` plus `min_scale` remains available.

Consequence, still open: with ~2.6 cards visible, nothing on screen says how many workspaces exist
either side. The pill bar was assumed to cover this -- it does not; it is collapsed to a small pill
and is not a workspace inventory while the overview is up. An edge affordance (a peek sliver, or a
"3 more ->" count at each edge) would close that gap cheaply and without touching layout.

---

## F16 — The drop does not animate; it teleports — DIAGNOSED, fix not built

Reported as "from release to settle the whole frame is skipped, feels laggy".

Three separate causes, in the order they were found:

1. The drag preview is drawn from a **synthetic box** (`draggedWindowGlobalBox()` = cursor position +
   card-scaled size) with no relation to the window's own animated position. At release the preview
   stops being drawn while the real window is already at its destination, so nothing ever moves
   between the two. Fixed: the drop now seeds the window's position from the drag box first
   (`SEEDHANDOFF`), and the redundant `warpPositionSize()` in the tiled same-workspace path is gone.

2. `windowsMove` was reported by `hyprctl animations` as **speed 0.0** — the config sets `windows`
   at 4.2 but never the child leaf, and it did not inherit. Set explicitly in
   `~/.config/hypr/modules/animations.lua`; it now reads 4.2 / pillMorph.

3. Even so, measured frame-to-frame RMSE after release still shows one jump and then a settled
   image by ~120 ms (a 420 ms animation would still be in flight). The remaining cause is
   compositor-side: **Hyprland warps a window when it changes workspace**, deliberately, so windows
   do not fly across the screen on a workspace switch. Card rendering follows
   `GEOMETRIC_CURRENT`, so it would show an animation if one were running — there is none to show.

What an actual fix requires: the plugin animates **its own preview**. Keep drawing the dragged
window after release and animate its box from the hand position into the destination card box over
~150-250 ms, then stop drawing it and let the real (already warped) window take over. That needs one
animated variable plus keeping the drag preview alive past button release. Not built.

Measurement technique for next time: `grim` frames at 40/80/120/200/600 ms after release, compared
with `magick compare -metric RMSE`. A running animation shows non-trivial differences across the
first few; a warp shows one jump then zeros.

---

## Root-cause work, 2026-08-01: invariant enforced, drop animation still unsolved

### Done: pointer-facing geometry is settled geometry (root fix for the F13 class)

F13 (insert working on one side of a card only) was patched by giving the open slot priority over a
card hit. The root cause was that two hit-tests computed card geometry with *different* parameters —
one including the insert spread, one not — and the spread is driven by the pointer itself.

Now enforced as a mechanism rather than a per-call-site flag: `SHitTestScope` sets
`hitTestingGeometry` for the duration of a hit test, and `insertSlotSpreadOffset()` returns 0 while
it is set. Applied at all nine pointer-facing entry points (`insertSlotAtOverviewPoint`,
`workspaceAtOverviewPoint`, `workspaceAtOverviewDropPoint`, `windowAtOverviewPoint`,
`windowAtOverviewCursorOnWorkspace`, `dropAnchorAtOverviewCursorOnWorkspace`, `draggedWindowBox`,
`draggedWindowBoxFor`, `draggedWindowGlobalBox`). Threading a bool through ~30 call sites is exactly
how the two hit-tests drifted apart in the first place; a scope cannot be half-applied.

**Invariant, stated for the future: geometry the pointer is tested against may not move as a result
of where the pointer is.** That covers the spread, the adaptive-scale re-fit mid-drag, and anything
of the same shape added later.

### Not done: the drop transition. Four attempts, all measured, all negative

1. Seed the window's position from the drag box *before* the move — erased, because Hyprland warps a
   window when it changes workspace.
2. Set `windowsMove` explicitly (it was reported as speed 0.0 and was not inheriting from `windows`) —
   necessary, not sufficient.
3. Drive the window's own animated variables *after* the move (`m_realPosition`/`m_realSize`:
   `setValueAndWarp()` to the hand box, then restore the goal). Measured: motion exists but is
   invisible, because the card maps the window's global box into card space, so the travel happens
   *inside* the destination card — a few dozen px.
4. An overview-owned animation interpolating the preview from the hand box to the window's box in its
   new card, with its own animation config so the duration was ours. Set to 1.5s to make sampling
   trivial: everything was settled by ~300ms, i.e. it never drew. Removed rather than left in.

Measurement method that made these conclusive (whole-screen RMSE is too blunt — a small moving window
scores ~25 and duplicate captures score 0): compare each frame to the settled frame and take the
bounding box of the changed region.

    magick compare -metric AE frame.png settled.png -compose src miff:- \
      | magick - -trim -format "%wx%h+%X+%Y" info:

    t=100ms  1945x632+0+584   <- whole card band still differs
    t=300ms   183x2+442+1169  <- only a text cursor: already settled

**Blocker for attempt 5: no observability.** Plugin log output never reaches the Hyprland log in this
session (`grep -c scrolloverview` on the log is 0, including the plugin's own load lines), so attempt
4 could not be told apart from "the function is never called". Fix that first — a debug Hyprland log,
or a dispatcher that reports plugin state on demand — then the drop animation is a short job.

---

## F17 — "only the first and last frame are visible" ✓ FIXED (fork) — severity A, and it was the root

The user's description — *"the window just snaps into frame, only initial and final frame is visible,
the whole middle is missing"* — is not a broken animation. It is animations running correctly while
their **intermediate frames are never presented**.

The plugin gates presentation in two places, and both listed only two animated variables:

    // shouldAllowRealtimePreviewSchedule()  -- hooks CMonitor::scheduleFrame
    if (scale->isBeingAnimated() || viewOffset->isBeingAnimated()) return true;

    // shouldSuppressRenderDamage()          -- hooks CMonitor::addDamage
    if (scale->isBeingAnimated() || viewOffset->isBeingAnimated()) return false;

So the card insert/remove transition, the insert-slot spread, and any animation added later advanced
in state with their frames dropped. Measured signature: exactly one intermediate frame is presented
(the first `damage()` from the update callback gets through) and then the pipeline stalls until the
animation ends — consecutive captures identical, yet all differing from the settled frame.

**This is also why four attempts at the drop transition looked like they never drew.** They ran. They
were not presented.

Fixed with one predicate used by both gates:

    bool CScrollOverview::hasRunningOverviewAnimation() const {
        return (scale && scale->isBeingAnimated())
            || (insertSlotSpread && insertSlotSpread->isBeingAnimated())
            || (dropProgress && dropProgress->isBeingAnimated())
            || hasRunningWorkspaceAnimation();
    }

**Rule for this fork: an animated variable that is not in `hasRunningOverviewAnimation()` will not be
seen.** Add new ones there, in the same commit that creates them.

With presentation fixed, the overview-owned drop animation (removed earlier as unproven) was restored
and now measures as continuous motion at the default 250 ms
(`plugin:scrolloverview:drop_animation_speed`), frame to frame:

    q0 -> q1  1949x1215      q2 -> q3  929x1057
    q1 -> q2   927x1060      q3 -> q4  935x1059

The card insert/remove transition and the slot spread were suppressed by the same gates and are now
presented too.
