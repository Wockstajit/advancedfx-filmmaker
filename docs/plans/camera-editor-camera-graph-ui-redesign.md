# Camera Editor — Camera tab & Graph tab visual redesign

**Status:** implemented 2026-07-01 (first pass — all 9 design-direction items except #9, the attach-point label bug, which was handed off as a separate task). Files touched: `CameraEditorJs.h` (new `S` tokens + render-side state treatments), `CameraEditorWidgetsJs.h` (`btnPrimary`/`btnDanger`/`tabify`/`styleTab`/`makeSlider` + elevated `section()`), `CameraEditorFollowInspectorJs.h` (axis-colored layered sliders, checkbox-indicator toggles, Place=primary/Clear=danger), `CameraEditorPathInspectorJs.h` (layered lens sliders, +Add=primary/Clear All=danger, empty-state card), `GraphEditorJs.h` (channel-row breathing room, bigger swatches, danger Delete Key). Regular Timeline untouched. Live in-game before/after verification still pending.
**Scope:** the Camera Editor's **Camera** bottom-tab (the right-hand inspector when `PATH CAMERA` / `FOLLOW CAMERA` is active) and the **Graph** bottom-tab (the graph editor dock + its part of the inspector). Purely visual/layout polish — no new features, no command/behavior changes.
**Explicitly OUT of scope:** the **Regular Timeline** tab (native CS2 demo bar, hosted/docked by `CameraTimelineJs.h`). That was just fixed (native EndPlayback/gear hidden permanently + round/kill-marker rescale on dock — see [[camera-editor-mode-feature]] memory) and must not be touched by this work. Don't change `nativeDock`/`nativeUndock`/`rescaleMarkers`/`patchNativeBar` or anything under the "Regular Timeline" bottom-panel mode.

## Why this plan exists

The user reviewed the current Camera Editor UI (screenshots taken directly from the live game, described in detail below since the actual PNGs weren't saved to disk as part of writing this plan — see "About the reference screenshots") and called it "weak and trash" — flat, generic, no visual hierarchy. This plan is the reference for a follow-up session to redesign it to look like a premium tool, working within what this embedded Panorama HUD context actually allows (see Constraints).

## About the reference screenshots

Three screenshots were shared in the conversation that produced this plan (2026-07-01 session), captured directly from the live in-game Camera Editor:

1. **Camera tab, FOLLOW CAMERA sub-mode, "Lock-on" mode, empty target state.**
2. **Camera tab, PATH CAMERA sub-mode, no cameras placed yet.**
3. **Graph tab, FOLLOW CAMERA sub-mode, "Attach" mode with a target selected, plus the Graph Editor dock below (channel list + plot).**

These images were not extracted to files by this planning session (no tool available to pull a user-attached image out of the conversation to disk). **Before starting implementation, re-capture equivalent screenshots** with `automation/capture/capture-main-monitor.ps1` while the editor is open in each of these three states (see `automation/verify/verify-editor-bottom-tabs.ps1` for the netcon sequence to reach each tab), and drop them in `docs/plans/reference/` for side-by-side comparison while iterating. The detailed descriptions below are a reasonably complete substitute in the meantime.

### Screenshot 1 — Camera tab / Follow Camera / Lock-on, empty

Top to bottom, right-hand inspector (430px wide, dark navy `#0c0e12` background):
- Header row: `CAMERA EDITOR` (bold, letter-spaced) + `✕ Exit` button, right-aligned.
- Full-width gold pill button: `MOUSE: UI [G]`.
- Two half-width tabs: `PATH CAMERA` / `FOLLOW CAMERA` (FOLLOW active — gold border + gold text, flat fill).
- Full-width row: eye icon + `Game UI: Show All`.
- Section card "FOLLOW / ATTACH CAMERA" (subtle lighter-navy `#1a1f26` background, small dim all-caps label) with subtext "ONE CAMERA - PLACING AGAIN REPLACES IT".
- Row of 4 buttons: `Place` (gold/active), `Reposition`, `Clear` (red text), `Live` — all same flat gray pill shape, differing only by text color.
- Hint text: "Place a camera, then select a target."
- Full-width gold pill: `Camera Mode: Lock-on` with a swap icon.
- Hint text under it.
- `Type` row: label + a custom dropdown field showing "Player".
- `Select Nearest` button + adjacent label "Closest: Ryder - CT".
- `Target` row: label + dropdown "Select target [10]".
- Section "FRAMING (OFFSET / ROTATION / FOV)" + a `Reset` button on the same header row.
- A 2-column grid of labeled sliders: Off X/Off Y, Off Z/Pitch, Yaw/Roll, then FOV alone (half-width, right side padded empty). Each slider cell is `label | thin native HorizontalSlider | numeric value` — plain, no track color, no fill-to-handle progress indication, generic thin gray line.
- Row of 5 preset buttons: `Center / Above / Low / Shoulder / Side` — same flat gray pill style as everything else.
- Collapsible section header `ADVANCED TRACKING >` + `Reset`.
- More slider rows: Look/Pos, Predict/Deadzone, Turn/s.
- A 2×2 grid of toggle buttons styled as literal bracket-checkboxes: `[x] Auto-disable on death`, `[ ] Retarget dropped weapon`, `[ ] Retarget dropped C4`, `[x] Hold last known position`. The `[x]` ones get a gold-tinted background, the `[ ]` ones stay flat gray. This reads like debug/placeholder UI, not a shipped toggle control.
- Full-width, taller gold CTA button: `+ Customize Player`.
- Below the inspector: the persistent bottom tab bar (`REGULAR TIMELINE / CAMERA / GRAPH` + gear ⚙, out of scope) and, under it, the (also out of scope) Regular Timeline strip.

### Screenshot 2 — Camera tab / Path Camera, empty (no markers placed)

Same header/tabs/Game-UI row as above, PATH CAMERA active instead. Then:
- Section "SELECTED CAMERA": `◀` / centered `no cameras` / `▶`.
- Row: `+ Add` (gold) / `Clear All` (red outline).
- Section "TRANSFORM": `POS` x/y/z readout, `ANG` p/y/r readout, hint "live free cam · press K to place a camera".
- Section "LENS": FOV slider (90°), Roll slider (0°), hint "Select a camera to edit its lens."
- Section "PATH": three buttons `Curve: Linear / Speed: Manual / Timing: Live`.
- Section "SHORTCUTS": "Space ▶/⏸ · -/+ ±15s" + "Scrub + keyframes on the timeline below."
- **A large dead-empty gap** (roughly 40% of the remaining panel height) between Shortcuts and the bottom Customize Player button — nothing fills it. This is the single worst offender for "looks sparse/unfinished."
- `+ Customize Player` CTA at the very bottom.

### Screenshot 3 — Graph tab / Follow Camera / Attach mode

Inspector top matches Screenshot 1 up through "FOLLOW / ATTACH CAMERA", but now:
- Buttons row: `Reposition`, `Live` (gold/active) — only 2 buttons this time (Attach mode has fewer controls than Lock-on).
- Hint: "Select a target to ride." + a RED warning line: "Select a target before repositioning attach." (this red inline-warning pattern is good and worth reusing elsewhere).
- Gold pill: `Camera Mode: Attach` + swap icon.
- `Type`: dropdown "Player Bone".
- `Select Nearest` + "closest: Ryder - CT".
- `Target`: dropdown "Select target [10]".
- `Attach pt`: dropdown showing **"head (invalid) - invalid - invalid"** — this looks like a genuine data/formatting bug in the attach-point dropdown label, not a design issue, but flag it for whoever picks this up; it's ugly and looks broken regardless of restyle. Not this plan's job to fix the logic, but worth a one-line note/ticket.
- Framing sliders (Off X = 72.0, gold-highlighted value since non-zero; others 0).
- Center/Above/Low/Shoulder/Side row.
- `+ Customize Player` CTA.
- Bottom tab bar: `REGULAR TIMELINE / CAMERA / GRAPH` (GRAPH active, gold).
- Below that, the **Graph Editor dock** (fills the whole bottom, ~556px tall, replaces the timeline):
  - Header: `GRAPH EDITOR`, `tick 53603 · 0 selected · live`, hint "shift: lock/straighten · box-select: ctrl+A = all · right-click = ease".
  - Toolbar row: `Smooth` (gold/active) / `Linear` / `↺ Undo` / `↻ Redo` / `✕ Delete Key`, then speed buttons `0.1x .. 4x` right-aligned.
  - Left column "CHANNELS": 7 rows (Pos X/Y/Z, Pitch, Yaw, Roll, FOV), each a small color swatch + name + eye icon (show/hide) + a bare `−` button (meaning unclear from the screenshot alone — likely remove/solo). Cramped, no hover/selected state visible, swatches are small flat squares.
  - Right: the plot area — grid lines, axis labels (`-1, 0, 1, 2` horizontal; `-256..256` vertical), a single gold vertical playhead line at x=0. No visible curve because 0 keys exist in this capture, so can't judge curve-line/keyframe-diamond styling from this screenshot alone.
  - Footer hint: "click name = solo · @ = show/hide" / "drag value = scrub · click value = type".

## Diagnosis — why this reads as "weak"

1. **No elevation/depth hierarchy.** Every section is the same flat `S.sectionBg` (`#1a1f26`) card with a 1px-equivalent implicit border and no shadow. Nothing recedes or comes forward; the eye has nowhere to land.
2. **One accent color doing too many jobs.** Gold (`S.accent #f0b323`) marks: the active PATH/FOLLOW tab, the active bottom tab, `Camera Mode` pills, the `Place`/`Live` active-state buttons, non-zero slider values, AND the `Customize Player` CTA. When everything is gold, nothing reads as *the* primary action.
3. **Buttons have no hierarchy.** `Place / Reposition / Clear / Live`, `Center / Above / Low / Shoulder / Side`, `Curve: Linear / Speed: Manual / Timing: Live` — every button in every row is visually identical (same pill, same padding, same font weight) regardless of how important or destructive it is. `Clear` (destructive) looks exactly as weighty as `Live` (a mode toggle).
4. **Sliders are the native `HorizontalSlider` with no styling investment** — thin flat line, no track fill/progress indication from min up to the handle, no color coding, generic tiny handle. This is the single most-touched control in the whole inspector and it currently looks like a placeholder.
5. **The `[x]` / `[ ]` bracket-checkbox toggles look like debug output**, not a finished control, and clash with the pill-button language used everywhere else.
6. **Dead space.** Screenshot 2 (Path Camera, empty) has a huge unstyled gap. Empty/placeholder states need their own intentional layout (e.g., a centered "Place your first camera" empty-state graphic/CTA), not just fewer rows leaving a hole.
7. **Typography has one weight and one rhythm.** Section headers, body labels, and hint text all use the same `Stratum2` family at 10–14px with only size/color to differentiate — no real type scale (e.g., a distinct hint/caption style that's visually quieter than it currently is, a distinct section-title treatment that isn't just "small caps + letter-spacing").
8. **Graph channel list is cramped** — swatch + label + eye + minus packed into a narrow column with no breathing room, no visible hover/selected affordance, and an unlabeled `−` button whose function has to be inferred.
9. **Icon usage is inconsistent** — a handful of controls get an icon (eye, gear, arrows, swap ⇄) but most buttons are text-only, so the icons that do exist don't reinforce a system, they just appear ad hoc.

## Constraints (real, not aesthetic preference — read before proposing anything)

This is a Source 2 Panorama HUD panel tree built and styled **entirely from inline JS** at runtime inside `AfxHookSource2.dll` — there is no separate `.vcss` stylesheet compiled for this UI, no build step for new assets, and no web-style CSS engine beyond what Panorama's native panel styles support. Concretely:

- **No mouse-move event.** Per [[panorama-ui-input-constraint]]: any drag interaction (sliders, resizing) must be built on a native `Slider`/`HorizontalSlider` panel — you cannot hand-roll a custom-drawn draggable control. Restyling sliders means overriding the *visual* layer around/behind a real `Slider` element (custom track/fill/handle panels layered with it, or styling the native `.HorizontalSlider` class via inline styles/classes), not replacing the drag mechanism.
- **`style.X = ''` throws and aborts the whole script.** Per [[panorama-style-empty-string]] — always clear with `rgba(0,0,0,0)` etc., never an empty string. This has bitten this codebase before; a redesign touching dozens of style lines is exactly where it'll bite again if this rule is forgotten.
- **MSVC ~16KB string-literal cap per adjacent raw-string chunk.** Any of these `*Js.h` files that grow past ~16KB in one `R"EDJS( ... )EDJS"` block hits `error C2026`. `CameraTimelineJs.h` just hit this during the native-bar fix and had to be split. A redesign that adds meaningfully more styling code to `CameraEditorFollowInspectorJs.h` / `CameraEditorPathInspectorJs.h` / `GraphEditorJs.h` should expect to need a new split point — check file size after each meaningful addition (`wc -l`) rather than waiting for the compiler error.
- **Fonts are whatever `Stratum2, "Arial Unicode MS"` resolves to in-game** (the existing `S.font` token) — no custom web fonts, no `@font-face`.
- **Colors/spacing should stay data, not be re-invented per file.** The existing `S` token object (defined once in `CameraEditorJs.h`, shared via closure into every `#include`d file) is the right place to *extend* the palette (e.g., add `S.accentDim`, `S.danger` is already there but unused in the button row, `S.trackFill`, `S.cardElevated`, a proper type-scale set of font sizes) rather than hardcoding new hex values inline all over `CameraEditorFollowInspectorJs.h`/`CameraEditorPathInspectorJs.h`/`GraphEditorJs.h`.
- **Icons**: no arbitrary SVG/image import pipeline is wired up for this UI (unlike native CS2 panels, which pull `s2r://panorama/images/...vsvg` — check whether pulling from that same pool works from an injected script; if not, stick to the Unicode-glyph approach already used for ⚙ ✕ ▾ ▴ ⇄).
- **The right-hand inspector is a fixed 430px (`INSPECTOR_W`)** column and the bottom Graph dock is a fixed 556px (`GRAPH_DOCK_H`, keep in sync with `CameraEditorJs.h`'s copy of the same constant) — redesign within those envelopes, don't grow them without checking the preview-frame math in `CameraEditorJs.h`'s `render()` that depends on them.
- **Whatever changes must keep working through `mirv_filmmaker editor curveeditor timeline|graph|native`** mode switching and the Path/Follow tab switch without visual glitches on transition (these are exercised live in `automation/verify/verify-editor-bottom-tabs.ps1` and `verify-editor-ui-follow-timeline.ps1` — re-run those, or extend them, as part of verifying this work).

## Design direction (concrete, works within the constraints above)

1. **Establish a real elevation scale.** Two or three background tiers (e.g., `S.bg` outer < `S.bgSoft` cards < a new lighter `S.bgRaised` for "active/selected" cards), plus a subtle `boxShadow` on top-level section cards (Panorama supports `box-shadow`, already used elsewhere in this codebase e.g. the settings card `#000000ee 0px 0px 26px 5px`). Sections should visibly sit "on top of" the inspector background, not just be a 6% lighter fill.
2. **Reserve gold for exactly one job: the current primary/confirm action.** Everything else (active-but-not-primary state, like an active tab) should get a *secondary* accent treatment — e.g., a thin gold left-border or bottom-underline instead of a full gold fill, so a full gold pill unambiguously means "this is the thing to click." Concretely: keep `Live`/`Place` gold (they ARE the primary action in their row), but change the PATH/FOLLOW and CAMERA/GRAPH/REGULAR-TIMELINE tab "active" treatment to an underline+text-color style instead of a filled pill, freeing gold to mean something.
3. **Introduce a button hierarchy:** primary (gold fill, one per row max), secondary (outlined or `S.btnBg` fill, current default), destructive (`S.danger` red, filled or outlined — not just red *text* on a gray pill like `Clear` currently is), and toggle/pressed (the existing `S.btnOn` treatment, fine as-is, just apply it consistently instead of ad hoc).
4. **Rebuild the slider visual**, not the mechanism: keep the real `Slider`/`HorizontalSlider` for drag, but layer a custom track (rounded, `S.bgSoft`) + a filled progress bar from the track's start to the handle position (color-coded per axis is a nice touch and cheap: e.g., a muted red/green/blue tint for X/Y/Z-style rows, matching the Graph channel swatches for consistency between the Framing sliders and the Graph channels) + a bigger, more deliberate handle. `sliderCell`/`sliderGrid` in `CameraEditorFollowInspectorJs.h` and the FOV/Roll sliders in `CameraEditorPathInspectorJs.h` are the two places this pattern needs to land; factor it into one shared helper (candidate: move it into `CameraEditorWidgetsJs.h` next to `mk/lbl/btn/section/row` since both inspector files need it, and the Graph channel color swatches could reuse the same axis-color constants).
5. **Replace the `[x]`/`[ ]` bracket toggles** with the same pill-button `btn()` treatment as everything else, using the toggle/pressed state (`S.btnOn` fill + accent text) instead of bracket glyphs, and add a small leading checkbox-style indicator (a tiny filled/unfilled square or a checkmark glyph) if a stronger on/off affordance is wanted beyond the fill color.
6. **Design an explicit empty state for Path Camera with 0 markers** instead of leaving dead space: a centered icon/glyph + "No cameras placed yet" + a restatement of the `K` hotkey and the `+ Add` action, filling the gap Screenshot 2 shows instead of leaving it blank. Same idea applies to the Graph plot when 0 keys exist (Screenshot 3) — currently just an empty grid.
7. **Tighten the Graph channel list**: give each row more vertical breathing room, add a visible hover/selected background, replace the bare `−` with a labeled/tooltip-carrying icon (or confirm what it does and make it read as "remove" vs "solo" unambiguously), and make the color swatch bigger/rounder so it visually ties to the (also-recolored) curve lines in the plot.
8. **Establish a real 3-tier type scale** in `S` (e.g., `S.fontSection` ~11px bold + wide letter-spacing for section titles as now, `S.fontLabel` ~13px for control labels, `S.fontHint` ~11px regular, dimmer color, tighter letter-spacing, NOT the same visual weight as section titles) and apply it consistently instead of picking sizes per call site.
9. **Fix (or explicitly hand off) the "head (invalid) - invalid - invalid" attach-point label bug** seen in Screenshot 3 — it undermines any amount of visual polish around it. This is a data/logic bug in `CameraEditorFollowInspectorJs.h`'s attach-point dropdown population, not a styling task; flag it to the user rather than silently fixing it as part of a "visual only" pass, unless they confirm it's in scope.

## Files likely touched

- `AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h` — the shared `S` style-token object (extend it here; this is the single source of truth for colors/fonts used by every `#include`d file below).
- `AfxHookSource2/Filmmaker/Panorama/CameraEditorWidgetsJs.h` — shared builders (`mk/lbl/btn/section/row`, `customDrop`); button-hierarchy variants and the new shared slider-track helper belong here.
- `AfxHookSource2/Filmmaker/Panorama/CameraEditorFollowInspectorJs.h` — Follow Camera tab body (Screenshots 1 & 3's inspector).
- `AfxHookSource2/Filmmaker/Panorama/CameraEditorPathInspectorJs.h` — Path Camera tab body (Screenshot 2's inspector).
- `AfxHookSource2/Filmmaker/Panorama/GraphEditorJs.h` — the Graph Editor dock (toolbar, channel list, plot) seen at the bottom of Screenshot 3.
- **Do not touch:** `AfxHookSource2/Filmmaker/Panorama/CameraTimelineJs.h` (Regular Timeline / native bar — just fixed, out of scope) beyond maybe reading it for the axis-color-constant idea if one already exists there.

## Suggested order of work

1. Extend `S` in `CameraEditorJs.h` with the new tokens (elevation tiers, danger/secondary button variants, type scale, axis colors) — additive only, don't rename/remove existing keys other call sites depend on.
2. Build the shared slider-track + button-hierarchy helpers in `CameraEditorWidgetsJs.h`.
3. Re-skin `CameraEditorFollowInspectorJs.h` (both Lock-on and Attach mode — Screenshots 1 & 3) using the new helpers; watch the ~16KB split limit.
4. Re-skin `CameraEditorPathInspectorJs.h` (Screenshot 2), including the empty-state design.
5. Re-skin `GraphEditorJs.h`'s toolbar + channel list; leave curve/keyframe rendering logic alone, just the chrome around it (unless a specific curve-line/diamond restyle is separately requested).
6. Rebuild (`cmake --build --preset x64-release --target AfxHookSource2`), relaunch via `automation/launch/launch-cs2-netcon.ps1`, and re-capture the same three states for a before/after comparison.
7. Run `automation/verify/verify-editor-bottom-tabs.ps1` and `verify-editor-ui-follow-timeline.ps1` to confirm nothing in the underlying tab-switch/mode-switch logic broke.
8. Confirm Regular Timeline mode is pixel-identical to before this work (diff a screenshot of it pre/post) — it must be untouched.
