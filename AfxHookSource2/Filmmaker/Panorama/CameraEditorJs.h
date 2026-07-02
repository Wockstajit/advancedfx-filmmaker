#pragma once

// Panorama JS for CAMERA EDITOR MODE: a docked editor workspace built ONCE into the
// in-game HUD (CSGOHud) context by CameraEditorHud.cpp.
//
// The chrome frames the live game as a "preview" in the top-left and fills the rest of
// the screen with editor panels:
//   * a RIGHT inspector column (selected-camera/property controls), and
//   * a BOTTOM backdrop behind the existing CameraTimeline panel (which renders in its
//     own root at a higher z-index, so it sits on top of this backdrop).
// The preview frame emerges automatically from the inspector's left accent border and
// the backdrop's top accent border (the backdrop border is clipped by the inspector
// where they meet, giving an L-shaped frame around the top-left preview rect).
//
// The gameplay HUD hide lives in CameraTimelineJs (driven by its "hosted" state flag),
// so a single script owns native-HUD visibility; this script only draws chrome.
//
// All controls issue the EXISTING "mirv_filmmaker camtl ... / marker ..." console
// commands -- the same back-end as the hotkeys and the timeline. Per the Panorama input
// constraint (in-game HUD JS has no mouse-move event) every draggable control is a
// native Slider panel.
//
//   C++ -> JS : attribute "state" (camera readouts + selected-key settings), then
//               $.CamEditor.render().
//   JS  -> C++: buttons / sliders issue console commands.

namespace Filmmaker {

inline const char* kCameraEditorJs = R"EDJS(
(function () {
  try {
    var existing = $('#CamEditorRoot'); if (existing) existing.DeleteAsync(0);
    var existingDbg = $('#CamEditorDebugRoot'); if (existingDbg) existingDbg.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var S = {
      accent: '#f0b323ff', freeze: '#4aa3ffff', danger: '#c92a2acc',
      bg: '#0c0e12ff', bgSoft: '#14181eff',
      panelBorder: '#ffffff1f', sectionBg: '#1a1f26ff',
      label: '#9aa4b0ff', value: '#eef2f6ff', dim: '#6b7480ff',
      btnBg: '#ffffff14', btnOn: '#f0b32333',
      // Redesign tokens (ADDITIVE -- the keys above are load-bearing across every #include'd
      // fragment; never rename/remove them). Elevation tiers, button hierarchy, slider tracks.
      bgRaised: '#232a35ff',                       // active/selected surfaces (3rd elevation tier)
      cardBorder: '#ffffff10',
      cardShadow: '#00000066 0px 3px 12px 0px',    // section cards sit ON the inspector bg
      accentText: '#151004ff',                     // dark text on a gold PRIMARY fill
      dangerBg: '#c92a2a26', dangerBorder: '#c92a2a66', dangerText: '#ff6b6bff',
      trackBg: '#00000066',
      clear: 'rgba(0,0,0,0)',                      // NEVER style.X = '' (aborts the script)
      // Per-axis slider fill colors -- keep in sync with kChColors in GraphEditorExperimentHud.cpp
      // so the Framing sliders visually tie to the Graph editor's channel swatches/curves.
      axis: ['#ff6b6bcc', '#7be07bcc', '#6ba8ffcc', '#ffcf5acc', '#c08affcc', '#5ad0ffcc', '#ff9a5acc'],
      font: 'Stratum2, "Arial Unicode MS"'
    };
    var INSPECTOR_W = 430, BOTTOM_H = 176, BOTTOM_LIFT = 28;
    // 42px: the tab buttons are ~30px tall (14px bold text + 12px padding) and the bar has 8px of
    // its own padding -- at the old 34px the button labels were clipped at the bottom.
    var TAB_BAR_H = 42, TAB_GAP = 4; // bottom-editor tab bar (DIVIDER above the active editor) height + gap
    var NATIVE_BAR_H = 96;  // fallback height for CS2's native demo bar (measured at runtime when shown)
    var GRAPH_DOCK_H = 556; // experimental graph-editor dock height; when open it REPLACES the timeline
                            // and fills the whole bottom (keep in sync with GraphEditorJs DOCK_H).
    var CH_ROLL = 5, CH_FOV = 6;
    var dbgGeom = null; // [debug overlay] last preview geometry stashed by render() (px + fractions)

    function cmd(c) {
      try { GameInterfaceAPI.ConsoleCommand(c); }
      catch (e) { $.Msg('[editor] cmd failed: ' + e + '\n'); }
    }

)EDJS"
#include "CameraEditorWidgetsJs.h"
R"EDJS(
    // ---- root: full-screen, non-hittest. Children draw bottom -> top. ----
    var root = $.CreatePanel('Panel', ctx, 'CamEditorRoot', {});
    root.hittest = false; root.style.width = '100%'; root.style.height = '100%'; root.style.zIndex = '53'; // z-layer map: PanoramaBridge.h

    // TOP-LEVEL root (sibling of CamEditorRoot, NOT a child): a child z-index is local to its
    // parent, so as a z200 child of the z53 editor root this modal rendered BEHIND the Graph
    // editor root (z65) and the timeline root (z55). Own root at z220 wins over every HUD root.
    // Deleted alongside CamEditorRoot in CameraEditorHud::Teardown.
    var confirmOverlay = $.CreatePanel('Panel', ctx, 'CamEditorConfirmRoot', {});
    confirmOverlay.visible = false; confirmOverlay.hittest = true;
    confirmOverlay.style.width = '100%'; confirmOverlay.style.height = '100%'; confirmOverlay.style.zIndex = '220';
    confirmOverlay.style.backgroundColor = 'rgba(0,0,0,0.72)';
    confirmOverlay.SetPanelEvent('onactivate', function () { hideClearConfirm(); });
    var confirmBox = mk('Panel', confirmOverlay); confirmBox.hittest = true;
    confirmBox.style.width = '520px'; confirmBox.style.height = '230px';
    confirmBox.style.horizontalAlign = 'center'; confirmBox.style.verticalAlign = 'center';
    confirmBox.style.backgroundColor = 'rgba(16,20,26,0.99)';
    confirmBox.style.border = '1px solid ' + S.accent; confirmBox.style.borderRadius = '6px';
    confirmBox.style.boxShadow = '#000000ee 0px 0px 28px 6px';
    confirmBox.style.flowChildren = 'down';
    confirmBox.style.paddingTop = '26px'; confirmBox.style.paddingBottom = '24px';
    confirmBox.style.paddingLeft = '28px'; confirmBox.style.paddingRight = '28px';
    confirmBox.SetPanelEvent('onactivate', function () { /* keep clicks inside from dismissing */ });
    var confirmTitle = lbl(confirmBox, 'CLEAR ALL CAMERA PATHS?', S.value, 22);
    confirmTitle.style.fontWeight = 'bold'; confirmTitle.style.letterSpacing = '2px';
    confirmTitle.style.horizontalAlign = 'center'; confirmTitle.style.marginBottom = '14px';
    var confirmBody = lbl(confirmBox, 'This deletes every path camera/keyframe for this demo.', S.label, 14);
    confirmBody.style.horizontalAlign = 'center'; confirmBody.style.marginBottom = '22px';
    var confirmActions = mk('Panel', confirmBox); confirmActions.style.flowChildren = 'right';
    confirmActions.style.horizontalAlign = 'center';
    function hideClearConfirm() { confirmOverlay.visible = false; }
    function showClearConfirm() { confirmOverlay.visible = true; }
    var confirmNo = btn(confirmActions, 'No', function () { hideClearConfirm(); }, S.value);
    confirmNo.style.width = '132px'; confirmNo.__lbl.style.textAlign = 'center'; confirmNo.__lbl.style.width = '100%';
    var confirmYes = btnDanger(confirmActions, 'Yes, clear all', function () {
      hideClearConfirm();
      cmd('mirv_filmmaker camtl clear');
    });
    confirmYes.style.width = '172px'; confirmYes.style.marginRight = '0px';
    confirmYes.__lbl.style.textAlign = 'center'; confirmYes.__lbl.style.width = '100%';

    // Preview catcher: full-screen click swallow (UI-cursor mode only) so clicking the
    // game preview can't leak through to spectator target switching. Inspector/backdrop
    // children are drawn AFTER it, so their own clicks still land.
    var catcher = mk('Panel', root); catcher.style.width = '100%'; catcher.style.height = '100%';
    catcher.style.position = '0px 0px 0px'; catcher.hittest = false;
    catcher.SetPanelEvent('onactivate', function () { closeAllDrops(); /* + swallow preview clicks */ });

    // View status tag pinned to the top-left of the live preview.
    var tag = mk('Panel', root); tag.hittest = false;
    tag.style.position = '18px 14px 0px'; tag.style.flowChildren = 'right';
    tag.style.height = '22px';
    tag.style.backgroundColor = 'rgba(12,14,18,0.65)'; tag.style.borderRadius = '3px';
    tag.style.paddingLeft = '8px'; tag.style.paddingRight = '10px';
    var dot = mk('Panel', tag); dot.style.width = '7px'; dot.style.height = '7px';
    dot.style.backgroundColor = S.danger; dot.style.borderRadius = '4px';
    dot.style.verticalAlign = 'center'; dot.style.marginRight = '7px';
    var tagLbl = lbl(tag, 'PREVIEW', S.value, 11); tagLbl.style.fontWeight = 'bold';
    tagLbl.style.letterSpacing = '2px'; tagLbl.style.verticalAlign = 'center';

    // Recording-style status pulse. Only opacity changes, so the tag never shifts or
    // reflows while flashing.
    var previewPulseBright = true;
    function pulsePreviewDot() {
      if (!dot || !dot.IsValid || !dot.IsValid()) return;
      previewPulseBright = !previewPulseBright;
      dot.style.opacity = previewPulseBright ? '1.0' : '0.22';
      $.Schedule(previewPulseBright ? 0.75 : 0.30, pulsePreviewDot);
    }
    $.Schedule(0.75, pulsePreviewDot);

    // Bottom backdrop (sits BEHIND the CameraTimeline panel, which is z55) = workspace
    // footer under the preview.
    var backdrop = mk('Panel', root); backdrop.hittest = false;
    backdrop.style.verticalAlign = 'top'; backdrop.style.horizontalAlign = 'left';
    backdrop.style.position = '0px 0px 0px';
    backdrop.style.width = '100%'; backdrop.style.height = BOTTOM_H + 'px';
    backdrop.style.backgroundColor = S.bg; backdrop.style.borderTop = '1px solid #ffffff14';

    // Bottom-editor tab bar: a DIVIDER sitting directly ABOVE the active bottom editor with the
    // three bottom-panel switches (where CS2's old CAM EDITOR / MOUSE buttons sat). It stays put
    // across modes; render() positions it just above whichever editor is docked below it, and sizes
    // its width. The native demo bar's own buttons are suppressed while the editor is open, so these
    // tabs replace them. Pure switches (NOT toggles): always select that mode -> never an empty bottom.
    var curBottomMode = 'native';
    var tabBar = mk('Panel', root); tabBar.hittest = false;
    tabBar.style.verticalAlign = 'top'; tabBar.style.horizontalAlign = 'left';
    tabBar.style.position = '0px 0px 0px';
    tabBar.style.height = TAB_BAR_H + 'px';
    tabBar.style.backgroundColor = S.bgSoft;
    tabBar.style.borderTop = '1px solid ' + S.accent; tabBar.style.borderBottom = '1px solid ' + S.accent;
    tabBar.style.flowChildren = 'right';
    tabBar.style.paddingTop = '4px'; tabBar.style.paddingBottom = '4px';
    tabBar.style.paddingLeft = '12px'; tabBar.style.paddingRight = '12px';
    // Order (left->right): Regular Timeline (native CS2 bar), Camera, Graph.
    var regularViewBtn = tabify(btn(tabBar, 'Regular Timeline', function () {
      cmd('mirv_filmmaker editor curveeditor native');
    }, S.value));
    regularViewBtn.style.verticalAlign = 'center'; regularViewBtn.style.width = '150px';
    var timelineViewBtn = tabify(btn(tabBar, 'Camera', function () {
      cmd('mirv_filmmaker editor curveeditor timeline');
    }, S.value));
    timelineViewBtn.style.verticalAlign = 'center'; timelineViewBtn.style.width = '150px';
    var graphViewBtn = tabify(btn(tabBar, 'Graph', function () {
      cmd('mirv_filmmaker editor curveeditor graph');
    }, S.value));
    graphViewBtn.style.verticalAlign = 'center'; graphViewBtn.style.width = '150px';
)EDJS"
R"EDJS(
    // Spacer pushes the gear to the far right of the bottom bar.
    var tabSpacer = mk('Panel', tabBar); tabSpacer.hittest = false;
    tabSpacer.style.width = 'fill-parent-flow(1.0)'; tabSpacer.style.height = '1px';
    // Gear: opens the cam-editor settings menu (native demo-playback toggles). Lives ON the
    // bottom bar so it only exists while the editor is open. ⚙ glyph keeps it asset-free.
    var gearBtn = btn(tabBar, '⚙', function () { toggleSettings(); }, S.value);
    gearBtn.style.verticalAlign = 'center'; gearBtn.style.marginRight = '0px';
    gearBtn.style.width = '38px'; gearBtn.__lbl.style.horizontalAlign = 'center';
    gearBtn.__lbl.style.fontSize = '18px';

    // ===================== CAM-EDITOR SETTINGS MENU =====================
    // Small fly-out over the bottom-right that mirrors CS2's native demo-playback settings
    // (huddemocontroller.xml). Each toggle drives the SAME cvar the native panel does, read
    // back live so the pill reflects the engine state:
    //   X-Ray              -> spec_show_xray 0|1
    //   True View          -> cl_demo_predict 0|1  (2 = also allow wrong demo version)
    //   Include DOA actions -> cl_trueview_show_doa_predictions 0|1   (needs True View on)
    //   Allow demo mismatch -> cl_demo_predict 1<->2                  (needs True View on)
    function rdInt(name) {
      try { var v = parseInt(GameInterfaceAPI.GetSettingString(name)); return isFinite(v) ? v : 0; }
      catch (e) { return 0; }
    }
)EDJS"
R"EDJS(
    // TOP-LEVEL root for the same reason as confirmOverlay above: as a z210 child of the z53
    // editor root it rendered behind the Graph (z65) / timeline (z55) roots, so the gear menu
    // was covered whenever those bottom editors were open. Deleted in Teardown with the rest.
    var settingsOverlay = $.CreatePanel('Panel', ctx, 'CamEditorSettingsRoot', {});
    settingsOverlay.visible = false; settingsOverlay.hittest = true;
    settingsOverlay.style.width = '100%'; settingsOverlay.style.height = '100%'; settingsOverlay.style.zIndex = '221';
    settingsOverlay.SetPanelEvent('onactivate', function () { closeSettings('bgClick'); });
    var settingsCard = mk('Panel', settingsOverlay); settingsCard.hittest = true;
    settingsCard.style.width = '360px'; settingsCard.style.horizontalAlign = 'right'; settingsCard.style.verticalAlign = 'bottom';
    settingsCard.style.marginRight = '16px'; settingsCard.style.marginBottom = (BOTTOM_H + 16) + 'px';
    settingsCard.style.backgroundColor = 'rgba(14,18,24,0.99)';
    settingsCard.style.border = '1px solid ' + S.accent; settingsCard.style.borderRadius = '6px';
    settingsCard.style.boxShadow = '#000000ee 0px 0px 26px 5px';
    settingsCard.style.flowChildren = 'down';
    settingsCard.style.paddingTop = '14px'; settingsCard.style.paddingBottom = '16px';
    settingsCard.style.paddingLeft = '16px'; settingsCard.style.paddingRight = '16px';
    settingsCard.SetPanelEvent('onactivate', function () { /* swallow clicks inside */ });
    var settingsTitle = lbl(settingsCard, 'CAM EDITOR SETTINGS', S.dim, 12);
    settingsTitle.style.fontWeight = 'bold'; settingsTitle.style.letterSpacing = '2px'; settingsTitle.style.marginBottom = '4px';
    // One toggle row: label (+ optional explanation) on the left, a pill switch on the right.
    function settingToggle(label, explain, onClick) {
      var r = mk('Panel', settingsCard); r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = '12px'; r.style.verticalAlign = 'middle';
      var col = mk('Panel', r); col.style.flowChildren = 'down'; col.style.width = 'fill-parent-flow(1.0)';
      col.style.verticalAlign = 'center'; col.style.paddingRight = '12px';
      var t = lbl(col, label, S.value, 14); t.style.fontWeight = 'bold';
      if (explain) { var e = lbl(col, explain, S.dim, 10); e.style.marginTop = '2px'; e.style.whiteSpace = 'normal'; }
      var pill = mk('Panel', r); pill.hittest = true; pill.style.width = '46px'; pill.style.height = '24px';
      pill.style.borderRadius = '12px'; pill.style.backgroundColor = '#00000088'; pill.style.verticalAlign = 'center';
      pill.style.border = '1px solid #ffffff14';
      var knob = mk('Panel', pill); knob.hittest = false; knob.style.width = '18px'; knob.style.height = '18px';
      knob.style.borderRadius = '9px'; knob.style.backgroundColor = '#cfd6deff'; knob.style.verticalAlign = 'center';
      knob.style.marginLeft = '3px'; knob.style.marginRight = '3px';
      pill.SetPanelEvent('onactivate', function () { onClick(); $.Schedule(0, refreshSettings); refreshSettings(); });
      return { row: r, label: t, pill: pill, knob: knob };
    }
    function setToggle(rec, on, enabled) {
      rec.pill.style.backgroundColor = on ? '#2bb24cff' : '#00000088';
      rec.pill.style.border = '1px solid ' + (on ? '#1c8a3aff' : '#ffffff14');
      rec.knob.style.horizontalAlign = on ? 'right' : 'left';
      rec.row.style.opacity = enabled ? '1.0' : '0.4';
      rec.pill.hittest = enabled;
    }
    var xrayTog = settingToggle('X-Ray', 'See players through walls (spec_show_xray).', function () {
      cmd('spec_show_xray ' + (rdInt('spec_show_xray') ? 0 : 1));
    });
    var trueviewTog = settingToggle('True View', 'Render each POV as the player actually saw it.', function () {
      var p = rdInt('cl_demo_predict');
      if (p > 0) cmd('cl_demo_predict 0');
      else cmd('cl_demo_predict ' + (rdInt('cl_demo_predict') >= 2 ? 2 : 1));
    });
    var doaTog = settingToggle('Include DOA actions', 'Predict dead-or-alive actions (needs True View).', function () {
      if (rdInt('cl_demo_predict') <= 0) return;
      cmd('cl_trueview_show_doa_predictions ' + (rdInt('cl_trueview_show_doa_predictions') ? 0 : 1));
    });
    var mismatchTog = settingToggle('Allow demo mismatch versions', 'Apply True View even on a mismatched demo build.', function () {
      var p = rdInt('cl_demo_predict');
      if (p === 1) cmd('cl_demo_predict 2');
      else if (p === 2) cmd('cl_demo_predict 1');
    });
    function refreshSettings() {
      var predict = rdInt('cl_demo_predict');
      var tvOn = predict > 0;
      setToggle(xrayTog, rdInt('spec_show_xray') !== 0, true);
      setToggle(trueviewTog, tvOn, true);
      setToggle(doaTog, rdInt('cl_trueview_show_doa_predictions') !== 0, tvOn);
      setToggle(mismatchTog, predict >= 2, tvOn);
    }
    function openSettings() {
      closeAllDrops(); refreshSettings(); settingsOverlay.visible = true; gearBtn.style.backgroundColor = S.btnOn;
      if (mvmDebugOn()) $.Msg('[camsettings] open\n');
    }
    function closeSettings(why) {
      // Diagnostic for the "popup won't stay open" report: log WHICH call site closed it and the
      // state at that instant, gated on mvm_debug so it's silent otherwise (see CameraEditorCustomizeJs.h
      // previewLog for the same pattern). Only logs when it was actually open (skips no-op closes).
      if (mvmDebugOn() && settingsOverlay.visible)
        $.Msg('[camsettings] close why=' + (why || '?') + ' en=' + (st && st.enabled) + ' cur=' + (st && st.cursor) + '\n');
      settingsOverlay.visible = false; gearBtn.style.backgroundColor = S.btnBg;
    }
    function toggleSettings() { if (settingsOverlay.visible) closeSettings('gearReclick'); else openSettings(); }
)EDJS"
#include "CameraEditorCustomizeJs.h"
R"EDJS(
    // Right inspector column.
    var inspector = mk('Panel', root); inspector.hittest = false;
    inspector.style.horizontalAlign = 'right'; inspector.style.verticalAlign = 'top';
    inspector.style.width = INSPECTOR_W + 'px'; inspector.style.height = '100%';
    inspector.style.backgroundColor = S.bg; inspector.style.borderLeft = '1px solid #ffffff14';
    inspector.style.flowChildren = 'down';
    inspector.style.paddingTop = '14px'; inspector.style.paddingBottom = '14px';
    inspector.style.paddingLeft = '14px'; inspector.style.paddingRight = '14px';
    inspector.style.fontFamily = S.font;
    // Scroll the WHOLE inspector vertically. Without this, once the Follow panel grows
    // (target dropdown / event list / advanced tracking open) the lower controls fall
    // off the bottom of the screen with no way to reach them. 'squish scroll' = no
    // horizontal scroll, vertical scrollbar + wheel when the content overflows.
    inspector.style.overflow = 'squish scroll';

    // Aspect-ratio letterbox: the live preview is sized to the GAME's aspect ratio
    // (rootW/rootH) and CENTRED in the available area (screen minus the right inspector and
    // the bottom bar); the leftover is masked with black bars on all four sides and an accent
    // frame outlines the rect. Geometry is computed each render from the laid-out root size.
    // (The game still renders full-screen behind, so the preview is a CROP shaped to the
    // render aspect when the scaler is off, or a true scaled blit when it is on.)
    var barBottom = mk('Panel', root); barBottom.hittest = false; barBottom.visible = false;
    barBottom.style.backgroundColor = '#000000ff'; barBottom.style.horizontalAlign = 'left'; barBottom.style.verticalAlign = 'top';
    var barRight = mk('Panel', root); barRight.hittest = false; barRight.visible = false;
    barRight.style.backgroundColor = '#000000ff'; barRight.style.horizontalAlign = 'left'; barRight.style.verticalAlign = 'top';
    var barLeft = mk('Panel', root); barLeft.hittest = false; barLeft.visible = false;
    barLeft.style.backgroundColor = '#000000ff'; barLeft.style.horizontalAlign = 'left'; barLeft.style.verticalAlign = 'top';
    var barTop = mk('Panel', root); barTop.hittest = false; barTop.visible = false;
    barTop.style.backgroundColor = '#000000ff'; barTop.style.horizontalAlign = 'left'; barTop.style.verticalAlign = 'top';
    var frameV = mk('Panel', root); frameV.hittest = false; frameV.visible = false;
    frameV.style.width = '2px'; frameV.style.backgroundColor = S.accent; frameV.style.horizontalAlign = 'left'; frameV.style.verticalAlign = 'top';
    var frameH = mk('Panel', root); frameH.hittest = false; frameH.visible = false;
    frameH.style.height = '2px'; frameH.style.backgroundColor = S.accent; frameH.style.horizontalAlign = 'left'; frameH.style.verticalAlign = 'top';
    var frameL = mk('Panel', root); frameL.hittest = false; frameL.visible = false;
    frameL.style.width = '2px'; frameL.style.backgroundColor = S.accent; frameL.style.horizontalAlign = 'left'; frameL.style.verticalAlign = 'top';
    var frameT = mk('Panel', root); frameT.hittest = false; frameT.visible = false;
    frameT.style.height = '2px'; frameT.style.backgroundColor = S.accent; frameT.style.horizontalAlign = 'left'; frameT.style.verticalAlign = 'top';

    root.SetAttributeString('debugpanels', '[]');
    function hidePreviewLayout() {
      frameL.visible = frameV.visible = frameT.visible = frameH.visible = false;
      barLeft.visible = barRight.visible = barTop.visible = barBottom.visible = false;
      tag.visible = false;
      root.SetAttributeString('previewrect', '');
      root.SetAttributeString('debugpanels', '[]');
      dbgGeom = null;
    }

    // ===================== HEADER =======================================
    var head = row(inspector); head.style.marginTop = '0px';
    var hTitle = lbl(head, 'CAMERA EDITOR', S.value, 16); hTitle.style.fontWeight = 'bold';
    hTitle.style.letterSpacing = '2px'; hTitle.style.width = 'fill-parent-flow(1.0)';
    hTitle.style.verticalAlign = 'center';
    // Exit fully closes the editor and restores the native CS2 demo bar. The bottom-panel switch
    // (Camera Timeline / Graph) lives in the persistent bottom tab bar built above, not here.
    var exitBtn = btn(head, '✕ Exit', function () { cmd('mirv_filmmaker editor close'); }, S.value);
    exitBtn.style.marginRight = '0px';

    var mouseRow = row(inspector);
    var mouseBtn = btn(mouseRow, 'MOUSE: UI  (G)', function () { cmd('mirv_filmmaker camtl cursor toggle'); }, S.accent);
    mouseBtn.style.width = 'fill-parent-flow(1.0)'; mouseBtn.style.marginRight = '0px';
    mouseBtn.__lbl.style.horizontalAlign = 'center';
    // (The Customize button is NOT here -- it's a persistent button pinned to the editor's
    //  bottom-right corner, built with the modal below so it survives Path/Follow tab switches.)

    var inspectorMode = 'path';
    var modeRow = row(inspector);
    var pathTab = tabify(btn(modeRow, 'PATH CAMERA', function () { inspectorMode = 'path'; }, S.accent));
    var followTab = tabify(btn(modeRow, 'FOLLOW CAMERA', function () { inspectorMode = 'follow'; }, S.value));
    pathTab.style.width = 'fill-parent-flow(1.0)';
    followTab.style.width = 'fill-parent-flow(1.0)'; followTab.style.marginRight = '0px';

    // Game-UI visibility (persists across the Path/Follow tabs): how much of CS2's own HUD
    // shows behind the editor. Hide All = clean; In-Game = radar + HP/ammo with NO spectator
    // observer panel; Show All = full spectator HUD.
    var hudLabel = lbl(inspector, 'GAME UI', S.dim, 10);
    hudLabel.style.letterSpacing = '1px'; hudLabel.style.marginTop = '7px';
    // One button that cycles Hide All -> In-Game -> Show All (text updates each click).
    var HUD_CYCLE = [['hidden', 'Hide All'], ['game', 'In-Game'], ['full', 'Show All']];
    var hudCycle = btn(inspector, 'Game UI: Show All', function () {
      var cur = (st && st.hudView) || 'full', idx = 0;
      for (var i = 0; i < HUD_CYCLE.length; i++) if (HUD_CYCLE[i][0] === cur) idx = i;
      cmd('mirv_filmmaker editor hud ' + HUD_CYCLE[(idx + 1) % HUD_CYCLE.length][0]);
    }, S.value);
    hudCycle.style.width = '100%'; hudCycle.style.marginRight = '0px';
    hudCycle.__lbl.style.horizontalAlign = 'center';

)EDJS"
#include "CameraEditorFollowInspectorJs.h"
#include "CameraEditorPathInspectorJs.h"
#include "CameraEditorDebugJs.h"
R"EDJS(
    // =====================================================================
    // The native CS2 demo controller's "End Playback" button and its own gear (X-Ray/True View
    // panel, replaced by our ⚙ menu) are hidden PERMANENTLY now -- not just while this editor is
    // open -- see CameraTimelineJs.h patchNativeBar(), which runs every frame from demo start
    // regardless of editor state (this workspace doesn't even exist until first opened).

    var st = null;
    var api = {};
    api.setInspectorMode = function (mode) {
      inspectorMode = mode === 'follow' ? 'follow' : 'path';
    };
    api.setAttachmentMenuOpen = function (open) {
      // Attach pt is a native DropDown now; toggling it open programmatically isn't needed.
    };
    // Live-tweak hooks for the 3D preview so the panel incantation / model paths can be dialed in
    // over netcon without rebuilding, e.g.:
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.previewInfo()+String.fromCharCode(10));
    //   mirv_filmmaker editor eval $.CamEditor.previewModel('agents/models/ctm_fbi/ctm_fbi.vmdl');
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.previewCall('SetActiveCharacter',5)+'\n');
    api.previewModel = function (p) { ensurePreview3d(p || teamDefaultModel()); applyPreview(); return previewState(); };
    api.previewItem = function (defIndex, paintKit) { var p = ensureViewmodelPreview(); setItemPreviewItem(p, makeFauxItemId(defIndex, paintKit || 0)); return previewState(); };
    api.previewCall = function (m, a) { if (preview3d && typeof preview3d[m] === 'function') { try { return '' + preview3d[m](a); } catch (e) { return 'ERR ' + e; } } return 'no3d'; };
    api.previewItemCall = function (m, a) { if (previewItem3d && typeof previewItem3d[m] === 'function') { try { return '' + previewItem3d[m](a); } catch (e) { return 'ERR ' + e; } } return 'noitem'; };
    api.openCustomize = function () { openCustomize(); return previewState(); };
    api.closeCustomize = function () { closeCustomize(); return 'closed'; };
    api.previewPick = function (slot, value) { pickCosmetic(slot, '' + value); return previewState(); };
    api.customizePick = function (slot, value) { pickCosmetic(slot, '' + value); return JSON.stringify({ sel: custSel, wear: custWear, preview: previewState() }); };
    api.customizeWear = function (slot, preset, value) { setWear(slot, preset || 'fn', value === undefined ? null : parseFloat(value)); return JSON.stringify({ sel: custSel, wear: custWear, preview: previewState() }); };
    api.customizeOpenPicker = function (slot) { populateCustomize(); var d = customizeDrops[slot]; if (d && d.open) d.open(); return slot || ''; };
    api.customizeOpenWear = function (slot) { populateCustomize(); var d = wearDrops[slot]; if (d && d.open) d.open(); return slot || ''; };
    api.customizeSearch = function (slot, query) { populateCustomize(); var d = customizeDrops[slot]; if (d && d.setSearch) { d.setSearch(query || ''); d.open(); } return query || ''; };
    // Captured-input pipe from CameraEditorHud (WndProc-thread wheel + WM_CHAR, forwarded on the main
    // thread). custWheel/custChars are hoisted function declarations in CameraEditorCustomizeJs.h.
    api.custWheel = function (n, x, y) { return custWheel(n, x, y); };
    api.custChars = function (csv) { return custChars(csv); };
    api.customizeOptions = function (slot) {
      slot = (slot || '').toString().replace(/^\s+|\s+$/g, '');
      var arr = decoratedOptions(slot), sample = [];
      for (var i = 0; i < arr.length && i < 12; i++) sample.push({ label: arr[i][0], value: arr[i][1], meta: arr[i][2] || {} });
      return JSON.stringify({ slot: slot, count: arr.length, sample: sample });
    };
    api.customizePickFirst = function (slot) {
      slot = (slot || '').toString().replace(/^\s+|\s+$/g, '');
      var arr = decoratedOptions(slot), value = '';
      for (var i = 0; i < arr.length; i++) {
        var v = '' + arr[i][1];
        if (!v || v === 'default' || v === 'ct_default' || v === 't_default') continue;
        if ((slot === 'primary' || slot === 'secondary') && (v === '0' || /:0$/.test(v))) continue;
        if ((slot === 'knife' || slot === 'gloves') && /:0$/.test(v)) continue;
        value = v;
        break;
      }
      if (!value && arr.length) value = '' + arr[0][1];
      if (value) pickCosmetic(slot, value);
      return JSON.stringify({ slot: slot, value: value, sel: custSel, wear: custWear, preview: previewState() });
    };
    // Apply/Cancel/Reset/Clear test hooks for the staged Finish/Wear/Agent/Gloves picks (see
    // markTouched/commitTouchedSlots in CameraEditorCustomizeJs.h) -- lets netcon automation drive the
    // same commit/discard paths the action-bar buttons use, without simulating clicks.
    api.customizeApply = function () { var applied = commitTouchedSlots(); return applied ? 'applied' : 'nothing-to-apply'; };
    api.customizeCancelPending = function () { custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false }; custDirty = false; updateActionBar(); return 'cancelled'; };
    api.customizeResetToDefault = function () { resetToDefault(); return 'reset'; };
    api.customizeState = function () {
      return JSON.stringify({
        visible: !!custOverlay.visible, target: custTargetIndex, key: custTargetKey, name: custTargetName,
        team: normalizeTeamName(custTargetTeam), activeWeaponDef: custActiveWeaponDef, activeWeaponSlot: custActiveWeaponSlot,
        loadout: custLoadout, sel: custSel, wear: custWear, itemWear: custItemWear, preview: previewState(),
        previewGeom: previewGeom(),
        touched: custTouched, dirty: custDirty, applyTarget: applyLabelTarget(),
        pickup: { active: custActiveWeaponPickup, ownerSteamId: custActiveWeaponOwnerSteam, ownerName: custActiveWeaponOwnerName },
        // Slot-level pickup/warning state (entity ownership; survives grenade/knife switches).
        pickupSlots: {
          primary: { pickup: slotPickup('primary'), ownerSteamId: slotOwnerSteam('primary'), ownerName: slotOwnerName('primary') },
          secondary: { pickup: slotPickup('secondary'), ownerSteamId: slotOwnerSteam('secondary'), ownerName: slotOwnerName('secondary') },
          warned: pickupSlots(), banner: !!custPickupBanner.visible
        }
      });
    };
    api.previewInfo = function () { return previewState(); };
    // Debug hooks for the customDrop popups (dropdowns are keyed off field.id set in customDrop()):
    //   mirv_filmmaker editor eval "$.Msg($.CamEditor.dbgDropState()+String.fromCharCode(10))"
    //   mirv_filmmaker editor eval "$.Msg($.CamEditor.dbgToggle('FmTargetDrop')+String.fromCharCode(10))"
    //   mirv_filmmaker editor eval "$.Msg($.CamEditor.dbgPick('FmAttachDrop',0)+String.fromCharCode(10))"
    api.dbgDropState = function () {
      var out = { open: openDrop ? openDrop.field.id : null, drops: [] };
      for (var i = 0; i < customDrops.length; i++) {
        var d = customDrops[i];
        out.drops.push({ id: d.field.id, opts: d.opts.length, open: !!d.pop.visible, ht: !!d.field.hittest, vis: !!d.field.visible });
      }
      return JSON.stringify(out);
    };
    api.dbgToggle = function (id) {
      for (var i = 0; i < customDrops.length; i++) if (customDrops[i].field.id === id) { toggleDrop(customDrops[i]); return 'toggled:' + id; }
      return 'not-found:' + id;
    };
    api.dbgPick = function (id, idx) {
      for (var i = 0; i < customDrops.length; i++) {
        var d = customDrops[i];
        if (d.field.id !== id) continue;
        if (idx < 0 || idx >= d.opts.length) return 'bad-idx:' + d.opts.length;
        var val = d.opts[idx][1];
        closeAllDrops();
        d.onPick(val);
        return 'picked:' + id + ':' + idx;
      }
      return 'not-found:' + id;
    };
    // Index-based variants: customDrop()'s `field.id = id` assignment does not persist through to
    // a readback (confirmed live -- every drop's field.id reads back '' regardless of the string
    // passed to customDrop()), so the id-keyed hooks above can never find anything. Array position
    // is stable across a build (creation order), so index into customDrops directly instead.
    api.dbgToggleAt = function (i) { if (i < 0 || i >= customDrops.length) return 'oob'; toggleDrop(customDrops[i]); return 'toggled:' + i; };
    // Atomic toggle+read (no netcon round-trip gap, so a same-frame revert can't hide behind
    // the ~0.7s/dozens-of-frames gap between two separate netcon commands).
    api.dbgToggleAndState = function (i) {
      if (i < 0 || i >= customDrops.length) return 'oob';
      var d = customDrops[i];
      var beforeWasOpen = (openDrop === d);
      var beforePopVis = !!d.pop.visible;
      toggleDrop(d);
      return JSON.stringify({
        i: i, beforeWasOpen: beforeWasOpen, beforePopVis: beforePopVis,
        afterPopVis: !!d.pop.visible, afterPopHt: !!d.pop.hittest, afterOpenIsThis: (openDrop === d), optCount: d.opts.length
      });
    };
    api.dbgPickAt = function (i, idx) {
      if (i < 0 || i >= customDrops.length) return 'oob';
      var d = customDrops[i];
      if (idx < 0 || idx >= d.opts.length) return 'bad-idx:' + d.opts.length;
      var val = d.opts[idx][1];
      closeAllDrops();
      d.onPick(val);
      return 'picked:' + i + ':' + idx;
    };
    // Debug hooks for the gear settings fly-out (mirrors the previewInfo/previewCall pattern):
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.toggleSettings()+String.fromCharCode(10));
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.settingsGeom()+String.fromCharCode(10));
    api.toggleSettings = function () { toggleSettings(); return settingsOverlay.visible ? 'open' : 'closed'; };
    api.settingsGeom = function () {
      var p = settingsCard.GetPositionWithinWindow ? settingsCard.GetPositionWithinWindow() : null;
      var gp = gearBtn.GetPositionWithinWindow ? gearBtn.GetPositionWithinWindow() : null;
      return JSON.stringify({
        ov: !!settingsOverlay.visible, cv: !!settingsCard.visible,
        mb: settingsCard.style.marginBottom, mr: settingsCard.style.marginRight,
        card: p ? { x: p.x, y: p.y, w: p.width, h: p.height } : null,
        gear: gp ? { x: gp.x, y: gp.y, w: gp.width, h: gp.height } : null
      });
    };
    api.previewFit = function (json) { return previewFit(json); };
    // Click-and-drag preview repositioning (user-driven, live): drag inside the preview box to nudge
    // the character, then call previewSave('start') while zoomed out or previewSave('zoom') while
    // zoomed in to bake it; previewPanReset() discards an in-progress drag without saving.
    api.previewSave = function (which) { return previewSavePosition(which); };
    api.previewPanReset = function () { return previewPanReset(); };
    // Live A/B of the preview SCENE preset (0=match_mvp, 1=vanity, 2=loadout-grid) without a
    // rebuild, so the HUD-compatible scene can be confirmed by eye over netcon:
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.previewTry(0)+String.fromCharCode(10));
    api.previewTry = function (idx) {
      previewSceneIdx = (idx | 0);
      USE_3D_PREVIEW = true; // force-enable for live experimentation (off by default in the HUD)
      try { if (preview3d && preview3d.DeleteAsync) preview3d.DeleteAsync(0); } catch (e) {}
      preview3d = null; preview3dTried = false; previewModelKey = '';
      pokePreviewSoon();
      applyPreview();
      return 'scene=' + previewSceneIdx + ' map=' + currentScene().map + ' ' + previewState();
    };
)EDJS"
R"EDJS(
    // Destroy + recreate the preview with override attrs (JSON), e.g. to try a different map/camera:
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.previewRebuild('{"map":"ui/match_mvp","camera":"camera"}')+String.fromCharCode(10));
    api.previewRebuild = function (json) {
      var ov = null; try { ov = json ? JSON.parse(json) : null; } catch (e) { return 'bad json'; }
      try { if (preview3d && preview3d.DeleteAsync) preview3d.DeleteAsync(0); } catch (e) {}
      try { if (previewItem3d && previewItem3d.DeleteAsync) previewItem3d.DeleteAsync(0); } catch (e) {}
      preview3d = createPreview3d(ov); previewItem3d = null; preview3dTried = true;
      previewModelKey = (ov && ov.playermodel) ? ov.playermodel : '';
      preview2d.visible = !preview3d;
      if (preview3d) applyPreview();
      return previewState();
    };
    api.render = function () {
      var raw = root.GetAttributeString('state', '');
      if (!raw) { root.visible = false; return; }
      try { st = JSON.parse(raw); } catch (e) { return; }

      root.visible = !!(st.enabled || st.debug);
      if (!st.enabled) {
        closeAllDrops();
        closeSettings('notEnabled');
        closeCustomize();
        customizeBtn.visible = false;
        confirmOverlay.visible = false;
        catcher.hittest = false;
        tag.visible = false;
        backdrop.visible = false;
        tabBar.visible = false;
        inspector.visible = false;
        barLeft.visible = barRight.visible = barTop.visible = barBottom.visible = false;
        frameL.visible = frameV.visible = frameT.visible = frameH.visible = false;
        var crsx = root.actualuiscale_x || ctx.actualuiscale_x || 1, crsy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
        var crawW = root.actuallayoutwidth || 0, crawH = root.actuallayoutheight || 0;
        if (crawW < 16) { crawW = ctx.actuallayoutwidth || 0; crawH = ctx.actuallayoutheight || 0; }
        var crw = crawW / crsx, crh = crawH / crsy;
        dbgGeom = (crw > 1 && crh > 1) ? { x0: 0, y0: 0, x1: 1, y1: 1, ox: 0, oy: 0, pw: crw, ph: crh } : null;
        root.SetAttributeString('previewrect', '0 0 1 1');
        updateDebug(st);
        return;
      }
      inspector.visible = true;
      tabBar.visible = true;

      // UI-cursor gating: panels are only clickable in UI-mouse mode; in GAME mode the
      // mouse flies the free cam and Panorama receives no clicks anyway.
      var cur = !!st.cursor;
      if (!cur) { closeAllDrops(); closeSettings('noCursor'); closeCustomize(); } // can't click popups in GAME-mouse mode; don't leave them hanging
      repositionOpenDrop(); // keep an open popup glued to its field while a scroll container moves it
      custScrollMaintain();  // size + clamp the modal's manual-scroll viewports (right column + open dropdown)
      previewUpdateDrag();   // click-and-drag preview panning (reads the live OS-cursor pipe in st.mx/my/lmb)
      publishCustomizeOpenState(); // safety net: covers visibility flips that skip open/closeCustomize
      catcher.hittest = cur;
      inspector.hittest = cur;
      backdrop.hittest = cur;
      tabBar.hittest = cur;
      mouseBtn.__lbl.text = cur ? 'MOUSE: UI  (G)' : 'MOUSE: GAME  (G)';
      mouseBtn.style.backgroundColor = cur ? S.btnOn : S.btnBg;
      mouseBtn.__lbl.style.color = cur ? S.accent : S.label;

      // Game-UI visibility picker highlight (default 'full' = full game UI).
      var hv = st.hudView || 'full', hudName = 'Show All';
      for (var hb = 0; hb < HUD_CYCLE.length; hb++) if (HUD_CYCLE[hb][0] === hv) hudName = HUD_CYCLE[hb][1];
      hudCycle.__lbl.text = 'Game UI: ' + hudName;

      // Customize button: shown while spectating a player, HIDDEN while flying the actual free cam.
      // obsTarget / obsMode are unreliable in demos, so do not require them for visibility. The
      // "live free cam" transform hint only means "no selected camera key" and can still be shown
      // during first-person spectate with a weapon viewmodel.
      var onPlayer = !st.freeCam;
      customizeBtn.visible = onPlayer;
      if (!onPlayer) closeCustomize();
      var ff = st.follow || {};
      var prevTargetKey = custTargetKey;
      var ct = st.customizeTarget || null;
      custTargetIndex = (typeof st.obsTarget === 'number' && st.obsTarget >= 0) ? st.obsTarget : -1;
      custTargetKey = custTargetIndex >= 0 ? ('pawn:' + custTargetIndex) : 'pawn:-1';
      custActiveWeaponDef = 0;
      custActiveWeaponSlot = '';
      custActiveWeaponPickup = false;
      custActiveWeaponOwnerSteam = '';
      custActiveWeaponOwnerName = '';
      if (ct && typeof ct.pawnIndex === 'number' && ct.pawnIndex >= 0) {
        custTargetIndex = ct.pawnIndex;
        custTargetKey = ct.key || ('pawn:' + ct.pawnIndex);
        custTargetName = ct.name || ('Player #' + ct.pawnIndex);
        custTargetTeam = ct.team;
        custActiveWeaponDef = parseInt(ct.activeWeaponDefIndex || 0, 10) || 0;
        custActiveWeaponSlot = WEAPON_SLOT_BY_DEF[custActiveWeaponDef] || '';
        custActiveWeaponPickup = !!ct.activeWeaponPickup;
        custActiveWeaponOwnerSteam = ct.activeWeaponOwnerSteamId ? ('' + ct.activeWeaponOwnerSteamId) : '';
        custActiveWeaponOwnerName = ct.activeWeaponOwnerName ? ('' + ct.activeWeaponOwnerName) : '';
      }
      if (custTargetIndex < 0 && typeof ff.targetIndex === 'number' && ff.targetIndex >= 0) custTargetIndex = ff.targetIndex;
      var cand = ff.candidates || [];
      if (custTargetIndex < 0 && cand.length) custTargetIndex = cand[0].index; // best demo fallback
      if (!ct && custTargetIndex >= 0 && st.customizePlayers)
        ct = st.customizePlayers['' + custTargetIndex] || null;
      if (ct && typeof ct.pawnIndex === 'number' && ct.pawnIndex >= 0) {
        custTargetIndex = ct.pawnIndex;
        custTargetKey = ct.key || ('pawn:' + ct.pawnIndex);
        custTargetName = ct.name || ('Player #' + ct.pawnIndex);
        custTargetTeam = ct.team;
        custActiveWeaponDef = parseInt(ct.activeWeaponDefIndex || 0, 10) || 0;
        custActiveWeaponSlot = WEAPON_SLOT_BY_DEF[custActiveWeaponDef] || '';
        custActiveWeaponPickup = !!ct.activeWeaponPickup;
        custActiveWeaponOwnerSteam = ct.activeWeaponOwnerSteamId ? ('' + ct.activeWeaponOwnerSteamId) : '';
        custActiveWeaponOwnerName = ct.activeWeaponOwnerName ? ('' + ct.activeWeaponOwnerName) : '';
      }
      if (!(ct && typeof ct.pawnIndex === 'number' && ct.pawnIndex >= 0))
        custTargetKey = custTargetIndex >= 0 ? ('pawn:' + custTargetIndex) : 'pawn:-1';
      var nm = ct && ct.name ? ct.name : null, tm = (ct && ct.team !== undefined) ? ct.team : '';
      if (!ct) for (var ci = 0; ci < cand.length; ci++) if (cand[ci].index === custTargetIndex) { nm = cand[ci].name; tm = cand[ci].team || ''; break; }
      custTargetName = nm || (custTargetIndex >= 0 ? ('Player #' + custTargetIndex) : 'Player');
      custTargetTeam = tm;
      applyCustomizeTargetLoadout(ct);
      updatePickupBanner();
      var nextLoadoutSig = customizeLoadoutSignature();
      if (custOverlay.visible && (custTargetKey !== prevTargetKey || custDisplayedLoadoutSig !== nextLoadoutSig)) {
        // Target/loadout changed under the open modal (player switch, or the demo's own state moved
        // on) -- discard any unstaged picks and reseed from the live loadout, same as opening fresh.
        custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false };
        custDirty = false;
        resetSelectionsFromLoadout();
        custTitleName.text = (custTargetName || 'PLAYER').toUpperCase();
        populateCustomize();
        updatePreview();
        pokePreviewSoon();
        custDisplayedLoadoutSig = nextLoadoutSig;
      }
      maintainPreview(); // self-heals the 3D preview for a few frames after it (re)opens

      var followMode = inspectorMode === 'follow';
      followSec.visible = followMode;
      for (var pm = 0; pm < pathPanels.length; pm++) pathPanels[pm].visible = !followMode;
      styleTab(followTab, followMode);
      styleTab(pathTab, !followMode);
      var bm = st.bottomMode || (st.graphExp ? 'graph' : 'native');
      curBottomMode = bm;
      var regularActive = (bm !== 'camera' && bm !== 'graph'); // 'native' (Regular Timeline)
      styleTab(timelineViewBtn, bm === 'camera');
      styleTab(graphViewBtn, bm === 'graph');
      styleTab(regularViewBtn, regularActive);
)EDJS"
R"EDJS(
      var f = st.follow || {};
      var liveBadge = !!(f.enabled || st.pathLive || (st.graphExp && st.graphDrive));
      tagLbl.text = liveBadge ? 'LIVE' : 'PREVIEW';
      var attachModeOn = (f.mode === 1);
      // Camera-model toggle (one button; tap flips Lock-on <-> Attach). Raised-surface
      // treatment, NOT gold: it's a mode switch, not the row's primary action.
      modeToggle.__lbl.text = (attachModeOn ? 'Camera Mode: Attach' : 'Camera Mode: Lock-on') + '   ⇄';
      modeToggle.style.backgroundColor = S.bgRaised; modeToggle.__lbl.style.color = S.value;
      modeHint.text = attachModeOn ? 'Rides the target (offset / rotation / FOV). Tap to switch.'
                                   : 'A placed camera rotates to track the target. Tap to switch.';
      if (attachModeOn) {
        advancedOpen = false;
        advancedRow.visible = false;
        advancedToggle.visible = false;
        advancedReset.visible = false;
        advancedPanel.visible = false;
        advancedToggle.__lbl.text = 'Advanced Tracking  ▸';
      } else {
        advancedRow.visible = true;
        advancedToggle.visible = true;
        advancedReset.visible = advancedOpen;
        advancedPanel.visible = advancedOpen;
        advancedToggle.__lbl.text = advancedOpen ? 'Advanced Tracking  ▾' : 'Advanced Tracking  ▸';
      }
      // Target-type dropdown: options follow the mode; selection reflects the current type.
      var typeVal = (f.type === 1) ? 'grenade' : (f.type === 2 ? 'weapon' : 'player');
      typeDrop.update(attachModeOn ? TYPE_ATTACH : TYPE_LOCKON, typeVal);
      // Place/Clear are lock-on only. Reposition is shared: lock-on moves the placed camera,
      // attach captures the current freecam pose as offset / rotation / FOV relative to the bone.
      if (singleHint) singleHint.text = attachModeOn ? 'RELATIVE MOUNT · REPOSITION SAVES OFFSET/ROT/FOV' : 'ONE CAMERA · PLACING AGAIN REPLACES IT';
      followPlace.visible = !attachModeOn;
      followReposition.visible = true;
      followClear.visible = !attachModeOn;
      followPlace.__lbl.text = f.hasCamera ? 'Replace' : 'Place';
      // Live = the preview toggle (short label so it never wraps). Full gold fill while
      // running so the active "you are live" state is unmistakable.
      followPreview.__lbl.text = f.enabled ? 'Stop' : 'Live';
      followPreview.style.backgroundColor = f.enabled ? S.accent : S.btnBg;
      followPreview.__lbl.style.color = f.enabled ? S.accentText : S.accent;
      var haveTarget = (f.targetIndex >= 0) || (f.type === 2 && f.weaponPlayerIndex >= 0);
      followStatus.text = haveTarget
        ? ((f.targetName || ('Entity #' + f.targetIndex)) + '  -  ' + (f.typeName || 'Player') + (attachModeOn ? ' (attach)' : ' (lock-on)'))
        : (attachModeOn ? 'Select a target to ride.' : 'Place a camera, then select a target.');
      followStatus.style.color = f.targetValid ? S.value : S.dim;
      followWarn.text = f.repositioning ? (attachModeOn
        ? 'Reposition active: move in freecam, then left-click to save attach pose.'
        : 'Reposition active: move in freecam, then left-click to place.')
        : (f.message ? f.message
        : (!attachModeOn && f.distanceLevel >= 3 ? 'Very far / likely outside the useful shot area.'
        : (!attachModeOn && f.distanceLevel >= 2 ? 'Selected target is far from this camera.' : '')));
      // Target selector. Small "Select Nearest" button; the closest candidate (list is
      // distance-sorted, so [0]) is named to its right.
      nearestBtn.__lbl.text = (f.type === 1) ? 'Nearest Throw' : 'Select Nearest';
      var nearestCand = (f.candidates && f.candidates.length) ? f.candidates[0] : null;
      nearestNameLbl.text = nearestCand
        ? ('closest: ' + nearestCand.name + (nearestCand.team && nearestCand.team !== '-' ? '  ·  ' + nearestCand.team : ''))
        : 'closest: —';
      nearestNameLbl.style.color = nearestCand ? S.value : S.dim;
      // The native DropDown shows the current target itself (no separate caret button).
      renderCandidates(f);
      // Weapon source (Weapon type only).
      srcRow.visible = (f.type === 2);
      if (f.type === 2) {
        var srcVal = (f.weaponSource === 1) ? 'held' : (f.weaponSource === 2 ? 'dropped' : 'auto');
        srcDrop.update(SRC_OPTS, srcVal);
      }
      // Attach point (attach mode only). The native dropdown shows the chosen point itself.
      attachRow.visible = attachModeOn;
      if (attachModeOn) renderAttachmentMenu(f);
      var ad = f.attachDebug || {};
      attachDebugText.visible = !!(f.debug && attachModeOn && ad.active);
      if (attachDebugText.visible) {
        attachDebugText.text = 'entity ' + (ad.entityIndex == null ? '-' : ad.entityIndex) +
          '  attach ' + (ad.attachId || '-') + '  ' + (ad.source || '-') + '\n' +
          'target ' + ((ad.rawTarget || []).join ? ad.rawTarget.join(', ') : '-') + '\n' +
          'camera ' + ((ad.camera || []).join ? ad.camera.join(', ') : '-') + '\n' +
          'dist ' + Number(ad.distance || 0).toFixed(1) +
          '  aim ' + Number(ad.aimError || 0).toFixed(1) +
          '  jitter ' + Number(ad.jitter || 0).toFixed(2) +
          '  dC ' + Number(ad.cameraDelta || 0).toFixed(2) +
          '  dT ' + Number(ad.targetDelta || 0).toFixed(2);
      } else {
        attachDebugText.text = '';
      }
      // Events + Preview Tick (grenade throws / weapon-C4 drops).
      var eventsRelevant = (f.type === 1 || f.type === 2);
      eventsLabel.visible = eventsRelevant;
      previewTickBtn.visible = eventsRelevant;
      eventRow.visible = (f.type === 2);
      previewTickBtn.__lbl.text = (f.type === 1)
        ? (f.grenadePending ? 'Waiting for Throw...' : 'Preview Tick (track grenade throw)')
        : 'Preview Tick (jump before drop)';
      previewTickBtn.style.backgroundColor = f.grenadePending ? S.btnOn : S.btnBg;
      if (f.type === 2) renderEvents(f);
      else eventDrop.update([], null, 'hidden', 'No events');
      // Framing sliders (offset / rotation / FOV).
      for (var xs = 0; xs < xformSliders.length; xs++) {
        var xr = xformSliders[xs], xv = num(xr.getVal(f));
        var xfrac = clamp01((xv - xr.lo) / (xr.hi - xr.lo));
        xr.busy = true; xr.sl.value = xfrac; xr.busy = false;
        if (xr.setFill) xr.setFill(xfrac);
        xr.vl.text = xv.toFixed(xr.decimals);
      }
      // Advanced smoothing sliders (now getVal-based, same as Framing).
      for (var fs = 0; fs < followSliders.length; fs++) {
        var rec = followSliders[fs], value = num(rec.getVal(f));
        var ffrac = clamp01((value - rec.lo) / (rec.hi - rec.lo));
        rec.busy = true; rec.sl.value = ffrac; rec.busy = false;
        if (rec.setFill) rec.setFill(ffrac);
        rec.vl.text = value.toFixed(rec.decimals);
      }
      for (var fk in optionBtns) if (optionBtns.hasOwnProperty(fk)) {
        var togOn = !!f[fk];
        optionBtns[fk].style.backgroundColor = togOn ? S.btnOn : S.btnBg;
        optionBtns[fk].__lbl.style.color = togOn ? S.accent : S.value;
        optionBtns[fk].__ind.style.backgroundColor = togOn ? S.accent : S.clear;
        optionBtns[fk].__ind.style.border = '1px solid ' + (togOn ? S.accent : '#ffffff55');
      }
      // Aspect-ratio letterbox. Virtual px = actuallayout / uiscale (matches style px).
      // Measure from the ALREADY-laid-out HUD context panel when our fresh root still reports 0
      // (the first layout passes after a build / resolution switch) -- otherwise the preview +
      // inspector squish into the top-left for ~half a second until root settles.
      var rsx = root.actualuiscale_x || ctx.actualuiscale_x || 1, rsy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      var rawW = root.actuallayoutwidth || 0, rawH = root.actuallayoutheight || 0;
      if (rawW < 16) { rawW = ctx.actuallayoutwidth || 0; rawH = ctx.actuallayoutheight || 0; }
      var rw = rawW / rsx, rh = rawH / rsy;
      // The tab bar + backdrop span the content width left of the inspector. The tab bar is a
      // DIVIDER docked directly ABOVE the active bottom editor; the editor rests flush at the
      // bottom and the preview shrinks to clear both. Anchor from the top; Panorama does not
      // reliably apply marginBottom on this full-screen root after the viewport is scaled.
      tabBar.style.width = Math.floor(Math.max(0, rw - INSPECTOR_W)) + 'px';
      if (!finitePositive(rw) || !finitePositive(rh)) {
        hidePreviewLayout();
      } else {
      // editorTop = y-from-bottom of the TOP edge of whatever editor is docked below the tab bar.
      // Camera/Graph overlays rest at marginBottom=BOTTOM_LIFT; the native bar rests at the very
      // bottom (Regular Timeline mode). Measure live heights where they can grow.
      var nativeBottom = (bm !== 'camera' && bm !== 'graph'); // 'native' = Regular Timeline
)EDJS"
R"EDJS(
      var editorTop;
      if (bm === 'graph') {
        editorTop = BOTTOM_LIFT + GRAPH_DOCK_H;
      } else if (bm === 'camera') {
        // The hosted CameraTimeline bar (#CamTimelineBar) is fit-children, so it grows when the
        // curve editor opens. Read its ACTUAL height; fall back to BOTTOM_H until it lays out.
        var camH = BOTTOM_H;
        try {
          var tlBar = ctx.FindChildTraverse && ctx.FindChildTraverse('CamTimelineBar');
          var bh = (tlBar && tlBar.visible) ? tlBar.actuallayoutheight / rsy : 0;
          if (bh > 10 && bh < rh * 0.6) camH = bh;
        } catch (eBar) {}
        editorTop = BOTTOM_LIFT + camH;
      } else {
        // Regular Timeline = CS2's native demo bar (docked left of the inspector by CameraTimelineJs),
        // resting at the very bottom. Measure its height so the tab bar sits right above it.
        var natH = NATIVE_BAR_H;
        try {
          var sr = ctx.FindChildTraverse && ctx.FindChildTraverse('SliderRow');
          var nc = sr && sr.GetParent ? sr.GetParent() : null;
          var nh = (nc && nc.visible) ? nc.actuallayoutheight / rsy : 0;
          if (nh > 10 && nh < rh * 0.6) natH = nh;
        } catch (eNat) {}
        editorTop = natH;
      }
      var bottomH = Math.max(TAB_BAR_H + BOTTOM_LIFT, Math.min(editorTop + TAB_GAP + TAB_BAR_H, Math.max(0, rh - 160)));
      var dockTopY = Math.max(0, rh - bottomH);
      var desiredTabY = rh - editorTop - TAB_GAP - TAB_BAR_H;
      var tabY = Math.max(dockTopY, desiredTabY);
      tabBar.style.position = '0px 0px 0px';
      tabBar.style.marginTop = Math.floor(tabY) + 'px';
      tabBar.style.marginBottom = '0px';
      // Keep the gear settings card glued right above the gear button: bottom edge just above the
      // tab bar's top (which moves with whichever editor is docked below), right edge aligned to
      // the bar's right end (the bar stops at the inspector, so a plain screen-right anchor would
      // put the card over the inspector instead of over the gear).
      // CLAMP to keep the whole card on-screen: the card is bottom-anchored and grows UPWARD, so when
      // a tall editor is docked below (e.g. the graph editor) an unclamped "glue above the tab bar"
      // marginBottom pushed the card's TOP off the top of the screen -- the gear toggled the overlay
      // visible but nothing was on screen ("gear reacts but no panel appears"). Cap marginBottom so
      // the card top stays within the viewport, using its real laid-out height when available.
      var scMB = Math.max(0, rh - tabY) + 8;
      var scH = (settingsCard.actuallayoutheight || 0) / rsy;
      if (scH > 8 && rh > scH + 16) scMB = Math.min(scMB, rh - scH - 8);
      settingsCard.style.marginBottom = Math.floor(Math.max(8, scMB)) + 'px';
      settingsCard.style.marginRight = (INSPECTOR_W + 16) + 'px';
      // In Regular Timeline mode the native CS2 bar (a LOWER-z HUD root) is the editor: our opaque
      // backdrop (z53) would HIDE the bar if it covered it, and our full-screen catcher would eat the
      // bar's clicks -- so the catcher shrinks to the preview area (the scaler blacks the rest).
      // For the backdrop we DON'T drop it entirely (that left the TAB_GAP strip just above the bar
      // see-through, bleeding the game render as a thin line); instead we anchor it ABOVE the bar
      // (editorTop..bottomH) so it fills that gap behind the tab bar without covering the native bar.
      // Camera/graph overlays render ABOVE z53, so there the backdrop covers the whole band (0..bottomH).
      backdrop.style.position = '0px 0px 0px';
      backdrop.style.marginTop = Math.floor(dockTopY) + 'px';
      backdrop.style.marginBottom = '0px';
      if (nativeBottom) {
        backdrop.style.height = Math.floor(Math.max(0, bottomH - editorTop)) + 'px';
      } else {
        backdrop.style.height = Math.floor(bottomH) + 'px';
      }
      backdrop.visible = true;
      catcher.style.height = nativeBottom ? Math.floor(Math.max(0, rh - bottomH)) + 'px' : '100%';
      var areaW = rw - INSPECTOR_W, areaH = rh - bottomH;
      if (finitePositive(areaW) && finitePositive(areaH)) {
        var aspect = rw / rh;            // the game's render aspect (full window)
        var pw = areaW, ph = pw / aspect;
        if (ph > areaH) { ph = areaH; pw = ph * aspect; } // fit the rect inside the area
        pw = Math.floor(pw); ph = Math.floor(ph);
        if (!finitePositive(pw) || !finitePositive(ph)) {
          hidePreviewLayout();
        } else {
        // Centre the shrunk preview in the available area so the black letterbox is even on
        // all sides instead of pinned top-left with one fat gap (the curve editor grows the
        // bottom bar, shrinking the preview -- this keeps it middled).
        var ox = Math.floor((areaW - pw) / 2); if (ox < 0) ox = 0;
        var oy = Math.floor((areaH - ph) / 2); if (oy < 0) oy = 0;
        var rgt = ox + pw, bot = oy + ph; // preview right / bottom edges (area space)

        // Four black mask bars: full-height left & right columns + top & bottom caps over the
        // preview column. Together they cover the whole area except the preview rect.
        barLeft.style.position = '0px 0px 0px';
        barLeft.style.width = ox + 'px'; barLeft.style.height = areaH + 'px';
        barLeft.visible = ox > 1;
        barRight.style.position = rgt + 'px 0px 0px';
        barRight.style.width = (areaW - rgt) + 'px'; barRight.style.height = areaH + 'px';
        barRight.visible = (areaW - rgt) > 1;
        barTop.style.position = ox + 'px 0px 0px';
        barTop.style.width = pw + 'px'; barTop.style.height = oy + 'px';
        barTop.visible = oy > 1;
        barBottom.style.position = ox + 'px ' + bot + 'px 0px';
        barBottom.style.width = pw + 'px'; barBottom.style.height = (areaH - bot) + 'px';
        barBottom.visible = (areaH - bot) > 1;

        // Accent frame: full rectangle around the centred preview.
        frameL.style.position = ox + 'px ' + oy + 'px 0px'; frameL.style.height = ph + 'px'; frameL.visible = true;
        frameV.style.position = (rgt - 2) + 'px ' + oy + 'px 0px'; frameV.style.height = ph + 'px'; frameV.visible = true;
        frameT.style.position = ox + 'px ' + oy + 'px 0px'; frameT.style.width = pw + 'px'; frameT.visible = true;
        frameH.style.position = ox + 'px ' + (bot - 2) + 'px 0px'; frameH.style.width = pw + 'px'; frameH.visible = true;

        // Keep the status tag pinned to the centred preview's top-left corner.
        tag.visible = true;
        tag.style.position = (ox + 18) + 'px ' + (oy + 14) + 'px 0px';

        // Publish the preview rect as NORMALISED root fractions (x0 y0 x1 y1 corners) so the
        // render-layer scaler (CViewportScaler) blits the whole frame into exactly this centred
        // rect when "editor scale" is on (it clears the rest black). Single source of truth
        // shared between Panorama (virtual px) and D3D (physical px); fractions stay resolution/
        // uiscale-independent so the two passes line up. The bars double as the visual frame
        // when scaling is OFF (crop mode); when ON they sit black-on-black.
        root.SetAttributeString('previewrect',
          (ox / rw).toFixed(5) + ' ' + (oy / rh).toFixed(5) + ' ' + (rgt / rw).toFixed(5) + ' ' + (bot / rh).toFixed(5));
        // [debug overlay] stash the laid-out preview geometry (fractions + virtual px) for updateDebug.
        dbgGeom = { x0: ox / rw, y0: oy / rh, x1: rgt / rw, y1: bot / rh, ox: ox, oy: oy, pw: pw, ph: ph };
        }
      } else {
        hidePreviewLayout();
      }
      }

      // NOTE: keep this a real BOOLEAN. Assigning the st.sel OBJECT to panel.visible
      // below throws in Panorama and aborts the whole render (which previously blanked
      // Transform, froze the Lens sliders, and desynced the Path section).
      var has = (st.selected >= 0 && !!st.sel);
      selLbl.text = has ? ('Key #' + (st.selected + 1) + ' / ' + st.count) : (st.count > 0 ? '— / ' + st.count : 'no cameras');
      delBtn.visible = retimeBtn.visible = has;
      // Path-tab empty state: only while the path has NO cameras (the pathPanels loop above
      // already hid it on the Follow tab; this narrows it further on the Path tab).
      pathEmpty.visible = !followMode && !(st.count > 0);

      // Transform readout: the selected key if any, else the live free cam.
      var src = has ? st.sel : (st.cam || null);
      if (src) {
        posLbl.text = 'POS   ' + num(src.x).toFixed(1) + '   ' + num(src.y).toFixed(1) + '   ' + num(src.z).toFixed(1);
        angLbl.text = 'ANG   p ' + num(src.pitch).toFixed(1) + '   y ' + num(src.yaw).toFixed(1) + '   r ' + num(src.roll).toFixed(1);
      } else { posLbl.text = 'POS   -'; angLbl.text = 'ANG   -'; }
      xfHint.text = has ? ('tick ' + st.sel.tick + '   ·   editing selected camera')
                        : 'live free cam   ·   press K to place a camera';

      syncSliders();
      lensHint.text = has ? 'Drag to edit FOV / roll of the selected camera.'
                          : 'Select a camera to edit its lens.';

      interpBtn.__lbl.text = 'Curve: ' + (st.interp || 'Linear');
      easeBtn.__lbl.text = 'Ease: ' + EASE_LBL[(has && st.sel.ease) || 0];
      easeBtn.visible = has;
      smBtn.__lbl.text = 'Speed: ' + (st.speedMode || 'Manual');

      var perSeg = (st.speedMode === 'Per-Segment');
      segRow.visible = perSeg && has && !st.sel.isLast; // visible:false collapses the row
      if (has) segLbl.text = 'Seg x' + st.sel.speedMul.toFixed(2);

      conRow.visible = (st.speedMode === 'Constant');
      conLbl.text = 'Const x' + (st.constSpeed != null ? st.constSpeed.toFixed(2) : '1.00');

      var freeze = (st.timing === 'Freeze');
      timeBtn.__lbl.text = 'Timing: ' + (freeze ? 'Freeze' : 'Live');
      timeBtn.__lbl.style.color = freeze ? S.freeze : S.value;

      updateDebug(st); // viewport/HUD debug overlay (no-op unless st.debug)
    };

    $.CamEditor = api;
    api.render();
    $.Msg('[editor] camera editor workspace built.\n');
  } catch (err) {
    $.Msg('[editor] gui error: ' + err + '\n');
  }
})();
)EDJS";

} // namespace Filmmaker
