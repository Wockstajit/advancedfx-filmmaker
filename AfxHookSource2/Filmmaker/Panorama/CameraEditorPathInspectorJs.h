#pragma once

// SELECTED CAMERA / TRANSFORM / LENS / PATH / SHORTCUTS inspector sections (path camera controls).
// JS fragment concatenated (via #include, as an adjacent raw string literal) into the
// kCameraEditorJs script body assembled in CameraEditorJs.h. Not a standalone script --
// shares scope/closures with the root IIFE. See CameraEditorJs.h for the entry point.

R"EDJS(
    // ===================== SELECTED CAMERA ==============================
    var pathPanels = [];
    var selSec = section(inspector, 'SELECTED CAMERA');
    pathPanels.push(selSec);
    var navRow = row(selSec);
    btn(navRow, '◀', function () { cmd('mirv_filmmaker camtl selectdelta -1'); }, S.value);
    var selLbl = lbl(navRow, 'none', S.value, 14); selLbl.style.fontWeight = 'bold';
    selLbl.style.width = 'fill-parent-flow(1.0)'; selLbl.style.textAlign = 'center';
    selLbl.style.verticalAlign = 'center';
    btn(navRow, '▶', function () { cmd('mirv_filmmaker camtl selectdelta 1'); }, S.value);
    var actRow = row(selSec);
    btn(actRow, '+ Add', function () { cmd('mirv_filmmaker camtl addkey'); }, S.accent);
    var delBtn = btn(actRow, '− Delete', function () { if (st && st.selected >= 0) cmd('mirv_filmmaker camtl delkey ' + st.selected); }, S.value);
    var retimeBtn = btn(actRow, '⟲ Tick→Here', function () { if (st && st.selected >= 0) cmd('mirv_filmmaker camtl movekey ' + st.selected + ' ' + st.tick); }, S.value);
    var clearAllBtn = btn(actRow, 'Clear All', function () { showClearConfirm(); }, S.danger);
    clearAllBtn.style.marginRight = '0px';

    // ===================== TRANSFORM (readout) ==========================
    var xfSec = section(inspector, 'TRANSFORM');
    pathPanels.push(xfSec);
    var posLbl = lbl(xfSec, '', S.value, 12); posLbl.style.marginTop = '2px';
    var angLbl = lbl(xfSec, '', S.value, 12); angLbl.style.marginTop = '3px';
    var xfHint = lbl(xfSec, '', S.dim, 10); xfHint.style.marginTop = '4px';
)EDJS"
R"EDJS(
    // ===================== LENS (FOV / ROLL sliders) ====================
    var sliders = [];
    function valSlider(parent, name, channel, lo, hi) {
      var r = row(parent);
      var nl = lbl(r, name, S.label, 12); nl.style.width = '46px'; nl.style.verticalAlign = 'center';
      var sl = $.CreatePanel('Slider', r, '', { direction: 'horizontal' }); sl.AddClass('HorizontalSlider');
      sl.style.width = 'fill-parent-flow(1.0)'; sl.style.height = '16px'; sl.style.verticalAlign = 'center';
      var vl = lbl(r, '-', S.accent, 12); vl.style.width = '54px'; vl.style.textAlign = 'right';
      vl.style.verticalAlign = 'center'; vl.style.marginLeft = '8px'; vl.style.fontWeight = 'bold';
      var rec = { sl: sl, vl: vl, lo: lo, hi: hi, ch: channel, drag: false };
      $.RegisterEventHandler('SliderValueChanged', sl, function (p, v) {
        var value = lo + v * (hi - lo); vl.text = value.toFixed(1);
        if (st && st.selected >= 0) {
          if (!rec.drag) { rec.drag = true; cmd('mirv_filmmaker camtl editbegin'); }
          cmd('mirv_filmmaker camtl setvalpreview ' + st.selected + ' ' + channel + ' ' + value.toFixed(3));
        }
      });
      $.RegisterEventHandler('SliderReleased', sl, function (p, v) {
        if (rec.drag) { rec.drag = false; cmd('mirv_filmmaker camtl editend'); }
      });
      sliders.push(rec); return rec;
    }
    var lensSec = section(inspector, 'LENS');
    pathPanels.push(lensSec);
    valSlider(lensSec, 'FOV', CH_FOV, 1, 170);
    valSlider(lensSec, 'Roll', CH_ROLL, -180, 180);
    var lensHint = lbl(lensSec, '', S.dim, 10); lensHint.style.marginTop = '4px';

    function syncSliders() {
      for (var i = 0; i < sliders.length; i++) {
        var s = sliders[i];
        if (!st || !st.sel) { s.vl.text = '-'; s.vl.style.color = S.dim; continue; }
        s.vl.style.color = S.accent;
        var cur = num((s.ch === CH_FOV) ? st.sel.fov : st.sel.roll);
        if (!s.sl.mousedown && !s.drag) s.sl.value = clamp01((cur - s.lo) / (s.hi - s.lo));
        if (!s.drag) s.vl.text = cur.toFixed(1);
      }
    }

    // ===================== PATH (interp / ease / speed / timing) ========
    var pathSec = section(inspector, 'PATH');
    pathPanels.push(pathSec);
    var interpBtn = btn(pathSec, 'Curve: Linear', function () {
      cmd('mirv_filmmaker marker interp cycle');
      // Mirror onto the experimental graph editor (the curve editor): the path interp is binary,
      // so flip the graph to match the value it is ABOUT to become (opposite of the current one).
      cmd('mirv_filmmaker grapheditor ' + ((st && st.interp === 'Linear') ? 'smooth' : 'linear'));
    }, S.value);
    interpBtn.style.marginTop = '2px';
    var EASE = ['none', 'in', 'out', 'inout'], EASE_LBL = ['None', 'Ease In', 'Ease Out', 'Ease In/Out'];
    var easeBtn = btn(pathSec, 'Ease: None', function () {
      if (!st || st.selected < 0) return;
      var nx = (((st.sel && st.sel.ease) || 0) + 1) % 4;
      cmd('mirv_filmmaker camtl ease ' + st.selected + ' ' + EASE[nx]);
      // Mirror onto the experimental graph editor (whole graph): None clears handles back to
      // straight lines; In/Out/In-Out flatten the matching tangents on every keyframe.
      cmd('mirv_filmmaker grapheditor ' + (nx === 0 ? 'linear' : ('ease ' + EASE[nx] + ' all')));
    }, S.value);
    easeBtn.style.marginTop = '5px';
    var smBtn = btn(pathSec, 'Speed: Manual', function () { cmd('mirv_filmmaker marker speedmode cycle'); }, S.value);
    smBtn.style.marginTop = '5px';

    // Segment speed stepper (Per-Segment, non-last key).
    var segRow = row(pathSec); segRow.style.marginTop = '5px';
    var segMinus = btn(segRow, '−', function () { stepSeg(-1); }, S.accent);
    var segLbl = lbl(segRow, 'Seg x1.00', S.value, 13); segLbl.style.width = 'fill-parent-flow(1.0)';
    segLbl.style.textAlign = 'center'; segLbl.style.verticalAlign = 'center';
    var segPlus = btn(segRow, '+', function () { stepSeg(1); }, S.accent); segPlus.style.marginRight = '0px';
    function stepSeg(d) {
      if (!st || !st.sel || st.selected < 0) return;
      var steps = [0.2, 0.5, 0.8, 1.0], cur = st.sel.speedMul, idx = 0, best = 1e9;
      for (var i = 0; i < steps.length; i++) { var dd = Math.abs(steps[i] - cur); if (dd < best) { best = dd; idx = i; } }
      idx += (d > 0 ? 1 : -1); if (idx < 0) idx = 0; if (idx >= steps.length) idx = steps.length - 1;
      cmd('mirv_filmmaker camtl speed ' + st.selected + ' ' + steps[idx].toFixed(2));
    }

    // Constant speed stepper (Constant mode).
    var conRow = row(pathSec); conRow.style.marginTop = '5px';
    var conMinus = btn(conRow, '−', function () { stepConst(-1); }, S.accent);
    var conLbl = lbl(conRow, 'Const x1.00', S.value, 13); conLbl.style.width = 'fill-parent-flow(1.0)';
    conLbl.style.textAlign = 'center'; conLbl.style.verticalAlign = 'center';
    var conPlus = btn(conRow, '+', function () { stepConst(1); }, S.accent); conPlus.style.marginRight = '0px';
    function stepConst(d) {
      if (!st) return;
      var steps = [0.2, 0.5, 0.8, 1.0], cur = st.constSpeed, idx = 0, best = 1e9;
      for (var i = 0; i < steps.length; i++) { var dd = Math.abs(steps[i] - cur); if (dd < best) { best = dd; idx = i; } }
      idx += (d > 0 ? 1 : -1); if (idx < 0) idx = 0; if (idx >= steps.length) idx = steps.length - 1;
      cmd('mirv_filmmaker marker constspeed ' + steps[idx].toFixed(2));
    }

    var timeBtn = btn(pathSec, 'Timing: Live', function () { cmd('mirv_filmmaker marker timing toggle'); }, S.value);
    timeBtn.style.marginTop = '5px';

    // ===================== PLAYBACK hints ===============================
    var playSec = section(inspector, 'SHORTCUTS');
    pathPanels.push(playSec);
    lbl(playSec, 'Space  ▶ / ⏸     ←/→  ±15s', S.value, 12);
    lbl(playSec, 'Scrub + keyframes on the timeline below.', S.dim, 10);
)EDJS"
