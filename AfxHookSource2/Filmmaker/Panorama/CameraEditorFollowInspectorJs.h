#pragma once

// FOLLOW / LOCK-ON CAMERA inspector section: target/attach UI, events, framing sliders.
// JS fragment concatenated (via #include, as an adjacent raw string literal) into the
// kCameraEditorJs script body assembled in CameraEditorJs.h. Not a standalone script --
// shares scope/closures with the root IIFE. See CameraEditorJs.h for the entry point.

R"EDJS(
    // ===================== FOLLOW / LOCK-ON CAMERA =====================
    var followSec = section(inspector, 'FOLLOW / ATTACH CAMERA');
    var singleHint = lbl(followSec, 'ONE CAMERA · PLACING AGAIN REPLACES IT', S.dim, 9);
    singleHint.style.letterSpacing = '1px'; singleHint.style.marginBottom = '4px';
    // Fixed height + always-visible so clearing the text in Attach mode reserves the same
    // vertical space and the panel below doesn't jump up.
    singleHint.style.height = '12px';
    // Camera controls on ONE even row: Place | Reposition | Clear | Live. (Place/Clear are
    // lock-on only; Reposition also captures an attach-relative mount in Attach mode.)
    var followActions = row(followSec);
    var followPlace = btnPrimary(followActions, 'Place', function () {
      cmd('mirv_filmmaker follow place');
      // Immediate confirmation the CLICK registered (independent of engine state): flash white.
      followPlace.style.backgroundColor = '#ffffffcc';
      try { $.Schedule(0.18, function () { followPlace.style.backgroundColor = S.accent; }); } catch (e) {}
    });
    var followReposition = btn(followActions, 'Reposition', function () { cmd('mirv_filmmaker follow reposition'); }, S.value);
    var followClear = btnDanger(followActions, 'Clear', function () { cmd('mirv_filmmaker follow clear'); });
    var followPreview = btn(followActions, 'Live', function () {
      cmd((st && st.follow && st.follow.enabled) ? 'mirv_filmmaker follow stop' : 'mirv_filmmaker follow preview');
    }, S.accent);
    [followPlace, followReposition, followClear, followPreview].forEach(function (b) {
      b.style.width = 'fill-parent-flow(1.0)'; b.style.paddingLeft = '4px'; b.style.paddingRight = '4px';
      b.__lbl.style.horizontalAlign = 'center'; b.__lbl.style.fontSize = '12px'; b.__lbl.style.whiteSpace = 'nowrap';
    });
    followPreview.style.marginRight = '0px';

    var followStatus = lbl(followSec, 'Place a camera and select a target.', S.dim, 11);
    followStatus.style.marginTop = '8px';
    var followWarn = lbl(followSec, '', '#f4c95dff', 10); followWarn.style.marginTop = '3px';
    var attachDebugText = lbl(followSec, '', '#9effa0ff', 9);
    attachDebugText.style.fontFamily = 'Consolas, "Courier New", monospace';
    attachDebugText.style.marginTop = '4px';

    // Camera model: ONE toggle that flips Lock-on (placed turret) <-> Attach (ride-along),
    // instead of two separate buttons.
    var modeToggle = btn(followSec, 'Camera Mode: Lock-on', function () {
      var attach = st && st.follow && st.follow.mode === 1;
      cmd('mirv_filmmaker follow mode ' + (attach ? 'lockon' : 'attach'));
    }, S.value);
    modeToggle.style.width = '100%'; modeToggle.style.marginRight = '0px'; modeToggle.style.marginTop = '9px';
    modeToggle.__lbl.style.horizontalAlign = 'center';
    var modeHint = lbl(followSec, '', S.dim, 9); modeHint.style.marginTop = '3px';

    // Target type: ONE dropdown whose options depend on the camera mode -- Lock-on lists
    // Player / Grenade / Weapon-C4; Attach lists Grenade / Player Bone / Weapon Attach.
    var TYPE_LOCKON = [['Player', 'player'], ['Grenade', 'grenade'], ['Weapon/C4', 'weapon']];
    var TYPE_ATTACH = [['Grenade', 'grenade'], ['Player Bone', 'player'], ['Weapon Attach', 'weapon']];
    var typeRow = row(followSec); typeRow.style.marginTop = '7px';
    var typeLbl = lbl(typeRow, 'Type', S.value, 12); typeLbl.style.width = '60px'; typeLbl.style.verticalAlign = 'center'; typeLbl.style.fontWeight = 'bold';
    var typeDrop = customDrop(typeRow, 'FmTypeDrop', function (value) { cmd('mirv_filmmaker follow type ' + value); });

    var pickRow = row(followSec); pickRow.style.marginTop = '7px';
    var nearestBtn = btn(pickRow, 'Select Nearest', function () { cmd('mirv_filmmaker follow nearest'); }, S.value);
    nearestBtn.style.width = 'fit-children';   // small button; closest target is shown to its right
    var nearestNameLbl = lbl(pickRow, '', S.value, 11);
    nearestNameLbl.style.verticalAlign = 'center'; nearestNameLbl.style.marginLeft = '10px';
    nearestNameLbl.style.width = 'fill-parent-flow(1.0)';
    nearestNameLbl.style.whiteSpace = 'nowrap'; nearestNameLbl.style.textOverflow = 'ellipsis';

    // Target selector: our customDrop. Its popup floats in the editor root above the inspector
    // (high z, not clipped by the inspector's scroll) so it overlays instead of expanding the
    // panel or pushing the controls below it.
    var targetRow = row(followSec); targetRow.style.marginTop = '7px';
    var targetLbl = lbl(targetRow, 'Target', S.value, 12);
    targetLbl.style.width = '60px'; targetLbl.style.verticalAlign = 'center'; targetLbl.style.fontWeight = 'bold';
    // Custom dropdown: each option's value is the candidate object itself, so onPick gets it back.
    var targetDrop = customDrop(targetRow, 'FmTargetDrop', function (c) { if (c) chooseCandidate(c); });
    var pendingCandidate = null;
    var confirmRow = row(followSec); confirmRow.visible = false;
    var confirmText = lbl(confirmRow, 'Target is far away. Use anyway?', '#f4c95dff', 10);
    confirmText.style.width = 'fill-parent-flow(1.0)'; confirmText.style.verticalAlign = 'center';
    btn(confirmRow, 'Use', function () {
      if (pendingCandidate) cmd('mirv_filmmaker follow select ' + pendingCandidate.index);
      pendingCandidate = null; confirmRow.visible = false;
    }, S.accent);
    btn(confirmRow, 'Cancel', function () { pendingCandidate = null; confirmRow.visible = false; }, S.value);

    function chooseCandidate(c) {
      if (st && st.follow && st.follow.type === 1) {
        cmd('mirv_filmmaker follow selecthandle ' + c.handle);
        return;
      }
      if (c.distanceLevel >= 2) {
        pendingCandidate = c;
        confirmText.text = (c.distanceLevel >= 3 ? 'Very far target' : 'Far target') +
          ' (' + Math.round(c.distance) + 'u). Use anyway?';
        confirmRow.visible = true;
      } else cmd('mirv_filmmaker follow select ' + c.index);
    }
    function candidateText(f, c) {
      if (f.type === 1) {
        var delta = c.tickDelta >= 0 ? ('+' + c.tickDelta) : String(c.tickDelta);
        return (c.grenadeType || c.name) + ' · ' + (c.ownerName || 'unknown thrower') +
          (c.team !== '-' ? ' · ' + c.team : '') + ' · tick ' + c.throwTick +
          ' (' + delta + ') · ' + (c.status || 'nearby');
      }
      if (c.isWeapon) return c.name + ' · ' + (c.status || (c.dropped ? 'dropped' : 'held'));
      // Player: "name · TEAM · 123u" -- just who, side, and how close. A dead flag is added only
      // when the player is dead (the old line printed "alive · alive" by appending both the alive
      // flag AND the status string).
      return c.name + (c.team && c.team !== '-' ? ' · ' + c.team : '') +
        (f.type === 0 && !c.alive ? ' · dead' : '') +
        ' · ' + Math.round(c.distance) + 'u';
    }
    function renderCandidates(f) {
      var MAXCAND = 40;
      var list = (f && f.candidates) || [];
      if (!list.length) {
        targetDrop.update([], null, 'empty:' + (f ? f.type : -1),
          (f && f.type === 1) ? 'No throws nearby' : 'No targets — Select Nearest');
        return;
      }
      // Distance is intentionally LEFT OUT of the rebuild signature: it changes every frame as
      // entities move, and rebuilding rows would close an open popup. Only the SET of targets (and
      // which one is selected) triggers a row rebuild; the collapsed bar text still updates live.
      var opts = [], sigParts = [], displayText = null, n = Math.min(MAXCAND, list.length);
      for (var j = 0; j < n; j++) {
        var c2 = list[j];
        var sel = !!(f.targetValid && ((c2.index === f.targetIndex && f.targetIndex >= 0) || (c2.handle && c2.handle === f.targetHandle)));
        var meta = { selected: sel };
        if (c2.distanceLevel >= 2) meta.color = '#f4c95dff';
        var text = candidateText(f, c2);
        opts.push([text, c2, meta]);
        if (sel) displayText = text;
        sigParts.push(c2.index + ':' + c2.handle + ':' + c2.name + ':' + c2.status + ':' + c2.throwTick + (sel ? ':S' : ''));
      }
      targetDrop.update(opts, null, sigParts.join('|') + ':' + f.type,
        displayText || ('Select target (' + n + ')'));
    }

)EDJS"
R"EDJS(
    // ---- Weapon source: Auto / Held / Dropped (Weapon type only) -- one dropdown -------
    var SRC_OPTS = [['Auto', 'auto'], ['Held', 'held'], ['Dropped', 'dropped']];
    var srcRow = row(followSec); srcRow.style.marginTop = '7px';
    var srcLabel = lbl(srcRow, 'Source', S.value, 12); srcLabel.style.width = '60px'; srcLabel.style.verticalAlign = 'center'; srcLabel.style.fontWeight = 'bold';
    var srcDrop = customDrop(srcRow, 'FmSrcDrop', function (value) { cmd('mirv_filmmaker follow weaponsource ' + value); });

    // ---- Attach point (Attach mode only) -- the SAME native dropdown as the others -------
    var attachRow = row(followSec); attachRow.style.marginTop = '7px';
    var attachLabel = lbl(attachRow, 'Attach pt', S.value, 12); attachLabel.style.width = '66px'; attachLabel.style.verticalAlign = 'center'; attachLabel.style.fontWeight = 'bold';
    var attachDrop = customDrop(attachRow, 'FmAttachDrop', function (value) { if (value) cmd('mirv_filmmaker follow bone ' + value); });
    function renderAttachmentMenu(f) {
      var options = [];
      if (f && f.attachPoints && f.attachPoints.length) options = f.attachPoints;
      var list = (f && f.candidates) || [], selected = null;
      for (var i = 0; i < list.length; i++) {
        if ((list[i].index === f.targetIndex) || (f.targetHandle && list[i].handle === f.targetHandle)) { selected = list[i]; break; }
      }
      if (!options.length && selected && selected.attachPoints && selected.attachPoints.length) options = selected.attachPoints;
      if (!options.length && selected && selected.attachments && selected.attachments.length) {
        options = [];
        for (var si = 0; si < selected.attachments.length; si++)
          options.push({ id: selected.attachments[si], label: selected.attachments[si], kind: 'attachment', valid: true });
      }
      var hasCurrent = false;
      for (var oi = 0; oi < options.length; oi++) if (options[oi].id === f.attachment) hasCurrent = true;
      if (f && f.attachment && !hasCurrent) options.unshift({ id: f.attachment, label: f.attachment + ' (invalid)', kind: 'invalid', valid: false });
      var opts = [];
      for (var j = 0; j < options.length; j++) {
        var p = options[j], name = p.id || p.label || 'entity', label = p.label || name;
        if (p.kind && p.kind !== 'attachment') label += ' · ' + p.kind;
        if (!p.valid) label += ' · invalid';
        opts.push([label, name]);
      }
      if (!opts.length) opts = [['No valid attach points', '']];
      attachDrop.update(opts, (f && f.attachment) || '');
    }

    // ---- Events: throw / drop / pickup ticks + Preview Tick -------------
    var eventsLabel = lbl(followSec, 'EVENTS', S.dim, 10);
    eventsLabel.style.letterSpacing = '1px'; eventsLabel.style.marginTop = '9px';
    var previewTickBtn = btn(followSec, 'Preview Tick (jump before event)', function () { cmd('mirv_filmmaker follow previewtick'); }, S.accent);
    previewTickBtn.style.marginTop = '3px'; previewTickBtn.style.marginRight = '0px';
    var eventRow = row(followSec); eventRow.style.marginTop = '5px';
    var eventLabel = lbl(eventRow, 'Event', S.value, 12);
    eventLabel.style.width = '60px'; eventLabel.style.verticalAlign = 'center'; eventLabel.style.fontWeight = 'bold';
    var eventDrop = customDrop(eventRow, 'FmEventDrop', function (value) {
      if (value != null && value !== '') cmd('mirv_filmmaker follow eventselect ' + value);
    });
    var eventSig = '';
    function eventText(e) {
      var label = e.type.replace('weapon_drop', 'death drop').replace('bomb_dropped', 'C4 drop')
        .replace('bomb_pickup', 'C4 pickup').replace('bomb_planted', 'C4 plant').replace('item_pickup', 'pickup');
      return 'tick ' + e.tick + '  ' + label + (e.item ? ('  ' + e.item) : '') + (e.ownerName ? ('  ' + e.ownerName) : '');
    }
    function renderEvents(f) {
      var list = (f && f.events) || [];
      var sig = (f ? (f.eventStatus + ':' + f.selectedEvent + ':') : '') + list.length;
      for (var i = 0; i < list.length; i++) sig += '|' + list[i].index + ':' + list[i].tick;
      if (!list.length) {
        var scanText = 'No drop/pickup events near here.';
        if (f && f.eventStatus === 1) {
          scanText = 'Scanning demo for drop/pickup events...' +
            (f.eventScanElapsedMs ? (' ' + Math.round(f.eventScanElapsedMs / 1000) + 's') : '');
        } else if (f && f.eventStatus === 3) {
          scanText = f.eventError ? ('Event scan failed: ' + f.eventError) : 'Event scan unavailable for this demo.';
        }
        eventDrop.update([], null, 'empty:' + (f ? f.eventStatus : -1), scanText);
        return;
      }
      var opts = [], displayText = null;
      for (var j = 0; j < list.length; j++) {
        var e = list[j], text = eventText(e), on = (f.selectedEvent === e.index);
        opts.push([text, e.index, { selected: on, color: on ? S.accent : S.value }]);
        if (on) displayText = text;
      }
      eventDrop.update(opts, f.selectedEvent, sig, displayText || ('Select event (' + list.length + ')'));
      eventSig = sig;
    }

    // ---- Framing: offset / rotation / FOV -- 2-column grid of native Sliders ----------
    var framingHead = row(followSec); framingHead.style.marginTop = '9px';
    var framingLabel = lbl(framingHead, 'FRAMING (OFFSET / ROTATION / FOV)', S.dim, 10);
    framingLabel.style.letterSpacing = '1px'; framingLabel.style.width = 'fill-parent-flow(1.0)';
    framingLabel.style.verticalAlign = 'center';
    var framingReset = btn(framingHead, 'Reset', function () { cmd('mirv_filmmaker follow reset framing'); }, S.value);
    framingReset.style.width = '76px'; framingReset.style.marginRight = '0px';
    framingReset.__lbl.style.fontSize = '10px';
    var xformPanel = mk('Panel', followSec); xformPanel.style.flowChildren = 'down'; xformPanel.style.width = '100%'; xformPanel.style.marginTop = '3px';
    var xformSliders = [];
    // Shared slider commit handler. (Panorama's Slider/keybind APIs don't expose a held-Shift
    // state during a drag, so fine-drag-on-Shift isn't wired here; the native Slider already
    // supports arrow-key nudging by one step once it has focus.)
    function attachSliderDrag(rec, cmdPrefix) {
      $.RegisterEventHandler('SliderValueChanged', rec.sl, function (panel, v) {
        if (rec.busy) return;
        rec.lastV = v;
        if (rec.setFill) rec.setFill(v); // keep the colored track fill glued to the handle mid-drag
        var value = rec.lo + v * (rec.hi - rec.lo);
        rec.vl.text = value.toFixed(rec.decimals);
        cmd(cmdPrefix + ' ' + value.toFixed(3));
      });
    }
    // One slider CELL (label | layered slider | value), sized to half the row so two fit per line.
    // color = the track-fill tint (per-axis S.axis[..] to match the Graph channel swatches).
    function sliderCell(parent, sink, name, lo, hi, decimals, getVal, command, color) {
      var cell = mk('Panel', parent); cell.style.flowChildren = 'right';
      cell.style.width = 'fill-parent-flow(1.0)'; cell.style.marginRight = '8px';
      var nl = lbl(cell, name, S.value, 12); nl.style.width = '54px'; nl.style.verticalAlign = 'center';
      nl.style.fontWeight = 'bold'; nl.style.whiteSpace = 'nowrap';
      var ms = makeSlider(cell, 14, color);
      var vl = lbl(cell, '-', S.accent, 12); vl.style.width = '40px'; vl.style.textAlign = 'right'; vl.style.verticalAlign = 'center'; vl.style.marginLeft = '5px'; vl.style.fontWeight = 'bold';
      var rec = { sl: ms.sl, vl: vl, setFill: ms.setFill, lo: lo, hi: hi, decimals: decimals, getVal: getVal, command: command, busy: false, lastV: 0, cell: cell };
      attachSliderDrag(rec, 'mirv_filmmaker follow ' + command);
      sink.push(rec); return rec;
    }
    // Lay items [name,lo,hi,dec,getVal,cmd,color] out two-per-row; a lone final item is left in
    // the first column at half width (reads as "centered-ish", balanced).
    function sliderGrid(parent, sink, items) {
      for (var i = 0; i < items.length; i += 2) {
        var r = row(parent); r.style.marginTop = '3px';
        var a = items[i]; sliderCell(r, sink, a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
        if (items[i + 1]) { var b = items[i + 1]; sliderCell(r, sink, b[0], b[1], b[2], b[3], b[4], b[5], b[6]); }
        else { var pad = mk('Panel', r); pad.style.width = 'fill-parent-flow(1.0)'; } // keep lone slider at half width
      }
    }
    sliderGrid(xformPanel, xformSliders, [
      ['Off X', -256, 256, 1, function (f) { return f.offset ? f.offset[0] : 0; }, 'offsetx', S.axis[0]],
      ['Off Y', -256, 256, 1, function (f) { return f.offset ? f.offset[1] : 0; }, 'offsety', S.axis[1]],
      ['Off Z', -256, 256, 1, function (f) { return f.offset ? f.offset[2] : 0; }, 'offsetz', S.axis[2]],
      ['Pitch', -180, 180, 0, function (f) { return f.rotation ? f.rotation[0] : 0; }, 'rotpitch', S.axis[3]],
      ['Yaw',   -180, 180, 0, function (f) { return f.rotation ? f.rotation[1] : 0; }, 'rotyaw', S.axis[4]],
      ['Roll',  -180, 180, 0, function (f) { return f.rotation ? f.rotation[2] : 0; }, 'rotroll', S.axis[5]],
      ['FOV',      1, 170, 0, function (f) { return f.fov || 90; }, 'fov', S.axis[6]]
    ]);

)EDJS"
R"EDJS(
    var presetRow = row(followSec); presetRow.style.marginTop = '6px';
    var PRESETS = [['Center','centered'],['Above','above'],['Low','low'],['Shoulder','shoulder'],['Side','side']];
    PRESETS.forEach(function (it, idx) {
      var b = btn(presetRow, it[0], function () { cmd('mirv_filmmaker follow preset ' + it[1]); }, S.value);
      b.style.width = 'fill-parent-flow(1.0)'; b.style.paddingLeft = '3px'; b.style.paddingRight = '3px';
      b.style.paddingTop = '8px'; b.style.paddingBottom = '8px';
      b.__lbl.style.fontSize = '11px'; b.__lbl.style.horizontalAlign = 'center'; b.__lbl.style.whiteSpace = 'nowrap';
      if (idx === PRESETS.length - 1) b.style.marginRight = '0px';
    });

    var advancedOpen = false;
    var advancedRow = row(followSec); advancedRow.style.marginTop = '8px';
    var advancedToggle = btn(advancedRow, 'Advanced Tracking  ▸', function () {
      advancedOpen = !advancedOpen;
      advancedPanel.visible = advancedOpen;
      advancedReset.visible = advancedOpen;
      advancedToggle.__lbl.text = advancedOpen ? 'Advanced Tracking  ▾' : 'Advanced Tracking  ▸';
    }, S.value);
    advancedToggle.style.width = 'fill-parent-flow(1.0)';
    var advancedReset = btn(advancedRow, 'Reset', function () { cmd('mirv_filmmaker follow reset tracking'); }, S.value);
    advancedReset.style.width = '76px'; advancedReset.style.marginRight = '0px'; advancedReset.visible = false;
    advancedReset.__lbl.style.fontSize = '10px';
    var advancedPanel = mk('Panel', followSec); advancedPanel.style.flowChildren = 'down';
    advancedPanel.style.width = '100%'; advancedPanel.style.marginTop = '4px'; advancedPanel.visible = false;

    // Advanced smoothing sliders -- same 2-column grid as Framing.
    var followSliders = [];
    sliderGrid(advancedPanel, followSliders, [
      ['Look',     0, 1.0,  2, function (f) { return f.look; },       'look'],
      ['Pos',      0, 1.0,  2, function (f) { return f.position; },   'position'],
      ['Predict',  0, 0.5,  2, function (f) { return f.prediction; }, 'prediction'],
      ['Deadzone', 0, 10,   1, function (f) { return f.deadzone; },   'deadzone'],
      ['Turn/s',   0, 1440, 0, function (f) { return f.maxTurn; },    'maxturn']
    ]);

    // Toggle options in a 2x2 grid. Each is a pill button with a small leading checkbox
    // indicator square (filled gold when on) -- replaces the old '[x]'/'[ ]' bracket text,
    // which read as debug output rather than a shipped control.
    var optionBtns = {};
    function followToggle(parent, key, label, command) {
      var b = mk('Panel', parent); b.hittest = true;
      b.style.backgroundColor = S.btnBg; b.style.borderRadius = '3px';
      b.style.border = '1px solid ' + S.cardBorder;
      b.style.width = 'fill-parent-flow(1.0)'; b.style.marginRight = '8px';
      b.style.paddingTop = '9px'; b.style.paddingBottom = '9px';
      b.style.paddingLeft = '6px'; b.style.paddingRight = '6px';
      var inner = mk('Panel', b); inner.hittest = false;
      inner.style.flowChildren = 'right'; inner.style.horizontalAlign = 'center';
      var ind = mk('Panel', inner); ind.hittest = false;
      ind.style.width = '10px'; ind.style.height = '10px'; ind.style.borderRadius = '2px';
      ind.style.border = '1px solid #ffffff55'; ind.style.backgroundColor = S.clear;
      ind.style.verticalAlign = 'center'; ind.style.marginRight = '6px';
      var l = lbl(inner, label, S.value, 11); l.style.fontWeight = 'bold';
      l.style.whiteSpace = 'nowrap'; l.style.verticalAlign = 'center';
      b.SetPanelEvent('onactivate', function () {
        var f = st && st.follow; cmd('mirv_filmmaker follow ' + command + ' ' + ((f && f[key]) ? 0 : 1));
      });
      b.__lbl = l; b.__ind = ind; b.__base = label; optionBtns[key] = b; return b;
    }
    var TOGGLES = [
      ['autoDead', 'Auto-disable on death', 'autodead'], ['switchWeapon', 'Retarget dropped weapon', 'switchweapon'],
      ['switchBomb', 'Retarget dropped C4', 'switchbomb'], ['holdLast', 'Hold last known position', 'hold']
    ];
    for (var ti = 0; ti < TOGGLES.length; ti += 2) {
      var tr = row(advancedPanel); tr.style.marginTop = '6px';
      followToggle(tr, TOGGLES[ti][0], TOGGLES[ti][1], TOGGLES[ti][2]);
      var lastTog = followToggle(tr, TOGGLES[ti + 1][0], TOGGLES[ti + 1][1], TOGGLES[ti + 1][2]);
      lastTog.style.marginRight = '0px';
    }

    // (Attach-point selector + framing sliders moved up into the FOLLOW / ATTACH section.)

)EDJS"
