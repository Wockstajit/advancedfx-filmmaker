#pragma once

// Panorama JS injected into the in-game HUD context to replace CS2's native
// demo-bar timescale DROPDOWN (#TimeScale, which opens a popup) with a row of
// inline speed buttons (0.1x .. 4x) in the same container (#SpeedControls).
//
// Each button runs `demo_timescale <x>` via GameInterfaceAPI.ConsoleCommand
// (the same API the native huddemocontroller.js uses). The native #TimeScale is
// HIDDEN (not deleted), so it can always be restored (see DemoBarButtons.cpp
// Teardown / `mirv_filmmaker speedbar off`).
//
// Panel lookups use FindChildTraverse (not $('#id')) so they resolve across the
// native demo-controller layout-file boundary.

namespace Filmmaker {

inline const char* kDemoBarButtonsJs = R"DBBJS(
(function () {
  try {
    var ctx = $.GetContextPanel();
    var speed = ctx.FindChildTraverse('SpeedControls');
    if (!speed) { $.Msg('[speedbar] SpeedControls not found\n'); return; }

    // Idempotency: drop any prior row before rebuilding.
    var prior = ctx.FindChildTraverse('MirvSpeedRow');
    if (prior) prior.DeleteAsync(0);

    // Hide (never delete) the native dropdown so it stays restorable.
    var ts = ctx.FindChildTraverse('TimeScale');
    if (ts) ts.visible = false;

    var row = $.CreatePanel('Panel', speed, 'MirvSpeedRow', {});
    row.style.flowChildren = 'right';
    row.style.verticalAlign = 'center';

    var speeds = [0.1, 0.25, 0.5, 1, 2, 4];
    var entries = []; // { panel, sp }

    function labelFor(sp) { return (sp === 1 ? '1x' : (String(sp) + 'x')); }

    function highlight(active) {
      entries.forEach(function (e) {
        var on = Math.abs(e.sp - active) < 0.0001;
        e.panel.style.color = on ? '#f0b323ff' : '#9aa4b0ff';
        e.panel.style.fontWeight = on ? 'bold' : 'normal';
      });
    }

    speeds.forEach(function (sp) {
      var b = $.CreatePanel('Label', row, '', { class: 'interactive' });
      b.text = labelFor(sp);
      b.style.minWidth = '30px';
      b.style.textAlign = 'center';
      b.style.verticalAlign = 'center';
      b.style.paddingTop = '2px'; b.style.paddingBottom = '2px';
      b.style.paddingLeft = '5px'; b.style.paddingRight = '5px';
      b.style.color = '#9aa4b0ff';
      b.SetPanelEvent('onactivate', function () {
        GameInterfaceAPI.ConsoleCommand('demo_timescale ' + sp);
        highlight(sp);
      });
      entries.push({ panel: b, sp: sp });
    });

    // Initial highlight: best-effort from the controller state, else default 1x.
    var cur = 1;
    try {
      var stt = ctx.GetDemoControllerState && ctx.GetDemoControllerState();
      if (stt && typeof stt.fTimeScale === 'number') cur = stt.fTimeScale;
    } catch (e) {}
    highlight(cur);

    $.Msg('[speedbar] injected.\n');
  } catch (err) {
    $.Msg('[speedbar] error: ' + err + '\n');
  }
})();
)DBBJS";

} // namespace Filmmaker
