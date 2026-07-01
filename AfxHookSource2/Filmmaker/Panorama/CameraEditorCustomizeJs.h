#pragma once

// CUSTOMIZE (LOADOUT) MODAL: cosmetic selection, wear controls, 3D preview wiring.
// JS fragment concatenated (via #include, as an adjacent raw string literal) into the
// kCameraEditorJs script body assembled in CameraEditorJs.h. Not a standalone script --
// shares scope/closures with the root IIFE. See CameraEditorJs.h for the entry point.

R"EDJS(
    // ===================== CUSTOMIZE (LOADOUT) MODAL ====================
    // A CS2-loadout-style modal for re-skinning the spectated player in the demo view. Only
    // reachable while spectating a player (obsMode 2/3); never in freecam. The dropdowns are
    // driven by a cosmetics dataset (Phase 2 replaces this stub with data generated from the
    // CS2 game files). Selecting an item updates the modal preview now; the per-frame econ/model
    // override that changes the actual demo render is wired in via pickCosmetic() once the native
    // "mirv_filmmaker cosmetics ..." back-end lands (Phases 3-4).

    // Rarity tiers -> colors, matching CS2's csgostyles.css (color-rarity-0..7).
    var RARITY = {
      0: { n: 'Default',    c: '#6a6156ff' },
      1: { n: 'Consumer',   c: '#b0c3d9ff' },
      2: { n: 'Industrial', c: '#5e98d9ff' },
      3: { n: 'Mil-Spec',   c: '#4b69ffff' },
      4: { n: 'Restricted', c: '#8847ffff' },
      5: { n: 'Classified', c: '#d32ce6ff' },
      6: { n: 'Covert',     c: '#eb4b4bff' },
      7: { n: 'Exceedingly Rare', c: '#e4ae39ff' }
    };
    function rarColor(r) { return (RARITY[r] || RARITY[0]).c; }
    function makeFauxItemId(defIndex, paintKitId) {
      try { return (BigInt('0xF000000000000000') | (BigInt(paintKitId || 0) << BigInt(16)) | BigInt(defIndex || 0)); }
      catch (e) { return null; }
    }
    function safeCall(panel, method) {
      if (!panel || !panel.IsValid || !panel.IsValid() || typeof panel[method] !== 'function') return null;
      try {
        var args = [];
        for (var i = 2; i < arguments.length; i++) args.push(arguments[i]);
        return panel[method].apply(panel, args);
      } catch (e) { return null; }
    }
    function fauxFromMeta(meta) {
      if (!meta || !meta.def) return null;
      return makeFauxItemId(meta.def, meta.paint || meta.paintKit || 0);
    }
    // Canonical econ item id (string) for a catalog meta, built by CS2's own econ system so an
    // <ItemImage> panel can render the REAL inventory thumbnail in the in-game HUD context (this is
    // exactly what avatar.js / itemtile.js do). Returns '' when the item has no real def index
    // (e.g. "default" rows) so the caller falls back to a rarity swatch.
    function econItemId(meta) {
      if (!meta) return '';
      var def = parseInt(meta.def || 0, 10) || 0;
      var paint = parseInt(meta.paint || meta.paintKit || 0, 10) || 0;
      if (def <= 0) return '';
      try {
        if (typeof InventoryAPI !== 'undefined' && InventoryAPI.GetFauxItemIDFromDefAndPaintIndex) {
          var id = InventoryAPI.GetFauxItemIDFromDefAndPaintIndex(def, paint);
          if (id && id !== '0') return id;
        }
      } catch (e) {}
      return '';
    }

    // Stub dataset: { key:value, ... } -> [label, value, {color, img}] options. Phase 2 swaps
    // this for a real JSON catalog generated from items_game.txt/csgo_english.txt. The shape is
    // already slot-based so the UI and commands match the final player-targeted API.
    var COSMETICS = {
      agent:  [['Default agent', 'default', { color: '#e6eaef', team: 'ANY' }],
               ['CT | SAS', 'ctm_sas', { color: rarColor(3), team: 'CT', def: 0, model: 'agents/models/ctm_sas/ctm_sas.vmdl' }],
               ['CT | FBI', 'ctm_fbi', { color: rarColor(4), team: 'CT', def: 0, model: 'agents/models/ctm_fbi/ctm_fbi.vmdl' }],
               ['T | Anarchist', 'tm_anarchist', { color: rarColor(3), team: 'T', def: 0, model: 'agents/models/tm_anarchist/tm_anarchist.vmdl' }],
               ['T | Balkan', 'tm_balkan', { color: rarColor(4), team: 'T', def: 0, model: 'agents/models/tm_balkan/tm_balkan_varianta.vmdl' }]],
      primary:[['Default rifle skin', '0', { color: rarColor(0), def: 7, paint: 0 }],
               ['AK-47 | Redline', '282', { color: rarColor(4), def: 7, paint: 282, wearMin: 0.10, wearMax: 0.70 }],
               ['AK-47 | Asiimov', '801', { color: rarColor(5), def: 7, paint: 801, wearMin: 0.05, wearMax: 0.70 }],
               ['AWP | Asiimov', '279', { color: rarColor(6), def: 9, paint: 279, wearMin: 0.18, wearMax: 1.00 }],
               ['AWP | Dragon Lore', '344', { color: rarColor(6), def: 9, paint: 344, wearMin: 0.00, wearMax: 0.70 }],
               ['M4A4 | Howl', '309', { color: rarColor(7), def: 16, paint: 309, wearMin: 0.00, wearMax: 0.40 }]],
      secondary:[['Default pistol skin', '0', { color: rarColor(0), def: 1, paint: 0 }],
               ['Desert Eagle | Blaze', '37', { color: rarColor(5), def: 1, paint: 37, wearMin: 0.00, wearMax: 0.08 }],
               ['USP-S | Kill Confirmed', '504', { color: rarColor(6), def: 61, paint: 504, wearMin: 0.00, wearMax: 1.00 }],
               ['Glock-18 | Fade', '38', { color: rarColor(5), def: 4, paint: 38, wearMin: 0.00, wearMax: 0.08 }]],
      knife:  [['Default CT knife', '42:0', { color: rarColor(0), def: 42, paint: 0, team: 'CT' }],
               ['Default T knife', '59:0', { color: rarColor(0), def: 59, paint: 0, team: 'T' }],
               ['Karambit | Doppler', '507:417', { color: rarColor(7), def: 507, paint: 417, wearMin: 0.00, wearMax: 0.08 }],
               ['Butterfly Knife | Fade', '515:38', { color: rarColor(7), def: 515, paint: 38, wearMin: 0.00, wearMax: 0.08 }],
               ['M9 Bayonet | Marble Fade', '508:413', { color: rarColor(7), def: 508, paint: 413, wearMin: 0.00, wearMax: 0.08 }]],
      gloves: [['Default CT Gloves', '5029:0', { color: rarColor(0), def: 5029, paint: 0, team: 'CT' }],
               ['Default T Gloves', '5028:0', { color: rarColor(0), def: 5028, paint: 0, team: 'T' }],
               ['Sport Gloves | Pandora Box', '5030:10037', { color: rarColor(7), def: 5030, paint: 10037, wearMin: 0.06, wearMax: 0.80 }],
               ['Specialist Gloves | Crimson Kimono', '5034:10033', { color: rarColor(7), def: 5034, paint: 10033, wearMin: 0.06, wearMax: 0.80 }],
               ['Moto Gloves | Spearmint', '5033:10026', { color: rarColor(7), def: 5033, paint: 10026, wearMin: 0.06, wearMax: 0.80 }]]
    };
)EDJS"
#include "CameraEditorCosmeticsCatalog.inc"
R"EDJS(
    // Current selections + the spectated player this loadout belongs to.
    var custSel = { agent: 'default', primary: '0', secondary: '0', knife: '0', gloves: '0' };
    var WEAR_PRESETS = {
      fn: { label: 'Factory New', lo: 0.00, hi: 0.07, value: 0.01 },
      mw: { label: 'Minimal Wear', lo: 0.07, hi: 0.15, value: 0.10 },
      ft: { label: 'Field-Tested', lo: 0.15, hi: 0.38, value: 0.22 },
      ww: { label: 'Well-Worn', lo: 0.38, hi: 0.45, value: 0.40 },
      bs: { label: 'Battle-Scarred', lo: 0.45, hi: 1.00, value: 0.55 },
      custom: { label: 'Custom Float', lo: 0.00, hi: 1.00, value: 0.01 }
    };
    var WEAR_OPTIONS = [['Factory New', 'fn'], ['Minimal Wear', 'mw'], ['Field-Tested', 'ft'], ['Well-Worn', 'ww'], ['Battle-Scarred', 'bs'], ['Custom Float', 'custom']];
    var custWear = {
      primary: { preset: 'fn', custom: 0.01 },
      secondary: { preset: 'fn', custom: 0.01 },
      knife: { preset: 'fn', custom: 0.01 },
      gloves: { preset: 'ft', custom: 0.22 }
    };
    var custItemWear = {};
    var custWearUpdating = false;
    var custPersist = {};
    var custTargetIndex = -1, custTargetName = 'Player', custTargetTeam = '';
    var custTargetKey = 'pawn:-1', custActiveWeaponDef = 0, custActiveWeaponSlot = '', custDisplayedLoadoutSig = '';
    var custActiveWeaponPickup = false, custActiveWeaponOwnerSteam = '', custActiveWeaponOwnerName = '';
    var custLoadout = { primary: null, secondary: null, knife: null, gloves: null };
    // Which weapon def the primary/secondary blocks are CURRENTLY editing. 0 = follow the player's
    // held weapon; > 0 = an explicit weapon the user picked from the weapon selector (so a skin can be
    // assigned to ANY weapon, not just the one the player is holding). Each weapon def stores its own
    // skin in the SteamID-keyed backend, so a player can carry a distinct skin on every weapon at once.
    var custWeaponDef = { primary: 0, secondary: 0 };
    var WEAPON_SLOT_BY_DEF = {
      1:'secondary', 2:'secondary', 3:'secondary', 4:'secondary', 30:'secondary', 32:'secondary', 36:'secondary', 61:'secondary', 63:'secondary', 64:'secondary',
      7:'primary', 8:'primary', 9:'primary', 10:'primary', 11:'primary', 13:'primary', 14:'primary', 16:'primary', 17:'primary', 19:'primary', 23:'primary', 24:'primary', 25:'primary', 26:'primary', 27:'primary', 28:'primary', 29:'primary', 33:'primary', 34:'primary', 35:'primary', 38:'primary', 39:'primary', 60:'primary',
      42:'knife', 59:'knife', 500:'knife', 503:'knife', 505:'knife', 506:'knife', 507:'knife', 508:'knife', 509:'knife', 512:'knife', 514:'knife', 515:'knife', 516:'knife', 517:'knife', 518:'knife', 519:'knife', 520:'knife', 521:'knife', 522:'knife', 523:'knife', 525:'knife', 526:'knife'
    };
)EDJS"
R"EDJS(
    // Rich item dropdown: thumbnail/rarity swatch + rarity-colored name, in both the collapsed
    // field bar and the popup rows. Reuses the shared open/close/positioning machinery
    // (customDrops / openDrop / showDropPopup), so only one popup is open at a time.
    function itemDrop(parent, id, onPick) {
      var field = mk('Panel', parent); field.id = id; field.hittest = true;
      field.style.width = '100%'; field.style.height = '44px'; field.style.flowChildren = 'right';
      field.style.verticalAlign = 'center'; field.style.borderRadius = '4px';
      field.style.backgroundColor = '#1b2230'; field.style.border = '1px solid #ffffff2e';
      field.style.paddingLeft = '6px'; field.style.paddingRight = '8px';
      var swatch = mk('Panel', field); swatch.hittest = false; swatch.style.width = '54px'; swatch.style.height = '34px';
      swatch.style.verticalAlign = 'center'; swatch.style.marginRight = '9px'; swatch.style.borderRadius = '3px';
      swatch.style.backgroundColor = '#0c0f14'; swatch.style.border = '1px solid #ffffff14';
      var img = mk('Image', swatch); img.hittest = false; img.style.width = '100%'; img.style.height = '100%';
      try { img.SetScaling('stretch-to-fit-preserve-aspect'); } catch (e) {}
      var disp = lbl(field, '', '#e6eaef', 14); disp.hittest = false; disp.style.verticalAlign = 'center';
      disp.style.width = 'fill-parent-flow(1.0)'; disp.style.fontWeight = 'bold';
      disp.style.whiteSpace = 'nowrap'; disp.style.textOverflow = 'ellipsis';
      var caret = lbl(field, '▾', '#e6eaef', 12); caret.hittest = false;
      caret.style.verticalAlign = 'center'; caret.style.marginLeft = '6px';
      var pop = mk('Panel', root); pop.visible = false; pop.hittest = true;
      pop.style.position = '0px 0px 0px'; pop.style.zIndex = '420'; pop.style.flowChildren = 'down';
      pop.style.backgroundColor = '#0e1620fa'; pop.style.border = '1px solid ' + S.accent;
      pop.style.borderRadius = '4px'; pop.style.overflow = 'squish scroll'; pop.style.maxHeight = '360px';
      pop.style.boxShadow = '#000000d0 0px 4px 14px 2px';
      var rec = { field: field, disp: disp, caret: caret, pop: pop, onPick: onPick, opts: [], sig: '', img: img, swatch: swatch, query: '' };
      field.SetPanelEvent('onactivate', function () { toggleDrop(rec); });
      customDrops.push(rec);
      function panoramaImageSrc(img) {
        if (!img) return '';
        var rel = String(img).replace(/\\/g, '/');
        if (rel.indexOf('s2r://') === 0 || rel.indexOf('file://') === 0) return rel;
        if (rel.indexOf('panorama/images/') === 0) rel = rel.substring(16);
        if (rel.slice(-11) === '_png.vtex_c') rel = rel.substring(0, rel.length - 2);
        if (rel.slice(-9) === '_png.vtex') return 's2r://panorama/images/' + rel;
        if (rel.slice(-4).toLowerCase() === '.png') rel = rel.substring(0, rel.length - 4);
        return 's2r://panorama/images/' + rel + '_png.vtex';
      }

      function applySwatch(target, meta) {
        // 1) Direct Panorama image resource path. Catalog metadata stores image refs relative to
        //    panorama/images/ and verified against <img>_png.vtex_c in the CS2 VPK. Expand them to
        //    the native CS2 resource URL here so agents, weapons, knives, and gloves all use the
        //    same loading path.
        if (meta && meta.img) {
          if (target.__iimg) target.__iimg.visible = false;
          target.__im = target.__im || mk('Image', target);
          target.__im.style.width = '100%'; target.__im.style.height = '100%';
          try { target.__im.SetScaling('stretch-to-fit-preserve-aspect'); } catch (e0) {}
          try { target.__im.SetImage(panoramaImageSrc(meta.img)); } catch (e1) {}
          target.__im.visible = true;
          target.style.backgroundColor = '#0c0f14';
          target.style.border = '1px solid ' + ((meta && meta.color) ? meta.color : '#ffffff14');
          return;
        }
        var itemId = econItemId(meta);
        if (itemId) {
          // Real CS2 econ thumbnail via a native <ItemImage> panel (renders in the HUD context).
          if (!target.__iimg) {
            try { target.__iimg = $.CreatePanel('ItemImage', target, '', {}); }
            catch (e) { target.__iimg = null; }
            if (target.__iimg) {
              target.__iimg.style.width = '100%'; target.__iimg.style.height = '100%';
              try { target.__iimg.SetScaling('stretch-to-fit-preserve-aspect'); } catch (e2) {}
            }
          }
          if (target.__iimg) {
            try { target.__iimg.itemid = itemId; } catch (e3) {}
            target.__iimg.visible = true;
            if (target.__im) target.__im.visible = false;
            // Keep rarity readable as a thin edge, not a big block.
            target.style.backgroundColor = '#0c0f14';
            target.style.border = '1px solid ' + ((meta && meta.color) ? meta.color : '#ffffff14');
            return;
          }
        }
        if (target.__iimg) target.__iimg.visible = false;
        target.style.backgroundColor = (meta && meta.color) ? meta.color : '#0c0f14';
        target.style.border = '1px solid #ffffff14';
      }
      function rebuild() {
        rec.pop.RemoveAndDeleteChildren();
        var search = $.CreatePanel('TextEntry', rec.pop, id + 'Search', { placeholder: 'Search' });
        search.style.width = '100%'; search.style.height = '34px'; search.style.marginBottom = '4px';
        search.style.backgroundColor = '#101823'; search.style.border = '1px solid #ffffff24';
        search.style.color = S.value; search.style.fontSize = '14px'; search.style.paddingLeft = '8px';
        try { search.text = rec.query || ''; } catch (e) {}
        search.SetPanelEvent('ontextentrychange', function () {
          try { rec.query = (search.text || '').toLowerCase(); } catch (e) { rec.query = ''; }
          rebuild();
          try { search.SetFocus(); } catch (e2) {}
        });
        for (var i = 0; i < rec.opts.length; i++) (function (o) {
          if (rec.query && (o[0] || '').toLowerCase().indexOf(rec.query) < 0) return;
          var meta = o[2] || {};
          var prow = mk('Panel', rec.pop); prow.hittest = true; prow.style.width = '100%'; prow.style.flowChildren = 'right';
          prow.style.paddingTop = '5px'; prow.style.paddingBottom = '5px'; prow.style.paddingLeft = '6px'; prow.style.paddingRight = '10px';
          prow.style.verticalAlign = 'middle'; prow.style.borderLeft = '3px solid ' + (meta.color || '#00000000');
          if (meta.selected) prow.style.backgroundColor = S.btnOn;
          var sw = mk('Panel', prow); sw.hittest = false; sw.style.width = '46px'; sw.style.height = '30px'; sw.style.marginRight = '9px';
          sw.style.verticalAlign = 'center'; sw.style.borderRadius = '3px'; sw.style.border = '1px solid #ffffff14';
          applySwatch(sw, meta);
          var t = lbl(prow, o[0], meta.color || '#e6eaef', 14); t.hittest = false; t.style.verticalAlign = 'center';
          t.style.width = 'fill-parent-flow(1.0)'; t.style.whiteSpace = 'nowrap'; t.style.textOverflow = 'ellipsis'; t.style.fontWeight = 'bold';
          prow.SetPanelEvent('onactivate', function () { closeAllDrops(); rec.onPick(o[1]); });
        })(rec.opts[i]);
      }
      return {
        dd: field,
        setSearch: function (q) { rec.query = (q || '').toLowerCase(); rebuild(); },
        open: function () { if (openDrop !== rec) closeAllDrops(); showDropPopup(rec); },
        update: function (opts, selValue) {
          rec.opts = opts || [];
          var sel = null;
          for (var d = 0; d < rec.opts.length; d++) {
            var m = rec.opts[d][2] || (rec.opts[d][2] = {});
            m.selected = (rec.opts[d][1] === selValue);
            if (m.selected) sel = rec.opts[d];
          }
          if (!sel && rec.opts.length) sel = rec.opts[0];
          var meta = sel ? (sel[2] || {}) : {};
          rec.disp.text = sel ? sel[0] : '—'; rec.disp.style.color = meta.color || '#e6eaef';
          rec.field.style.border = '1px solid ' + (meta.color || '#ffffff2e');
          applySwatch(rec.swatch, meta);
          var sig = ''; for (var k = 0; k < rec.opts.length; k++) sig += rec.opts[k][0] + '|'; sig += '#' + selValue + '#' + rec.query;
          if (sig !== rec.sig) { rec.sig = sig; rebuild(); }
        }
      };
    }

    // ---- Modal chrome --------------------------------------------------
)EDJS"
R"EDJS(
    var custOverlay = mk('Panel', root); custOverlay.visible = false; custOverlay.hittest = true;
    custOverlay.style.width = '100%'; custOverlay.style.height = '100%'; custOverlay.style.zIndex = '230';
    custOverlay.style.backgroundColor = 'rgba(0,0,0,0.78)';
    custOverlay.SetPanelEvent('onactivate', function () { closeCustomize(); }); // click-outside closes
    var custWin = mk('Panel', custOverlay); custWin.hittest = true;
    custWin.style.width = '80%'; custWin.style.height = '82%';
    custWin.style.horizontalAlign = 'center'; custWin.style.verticalAlign = 'center';
    custWin.style.backgroundColor = 'rgba(12,15,20,0.99)'; custWin.style.border = '1px solid ' + S.accent;
    custWin.style.borderRadius = '8px'; custWin.style.boxShadow = '#000000ee 0px 0px 40px 10px';
    custWin.style.flowChildren = 'down';
    custWin.SetPanelEvent('onactivate', function () { /* swallow clicks inside the window */ });

    var custHead = mk('Panel', custWin); custHead.style.flowChildren = 'right'; custHead.style.width = '100%';
    custHead.style.paddingTop = '14px'; custHead.style.paddingBottom = '14px'; custHead.style.paddingLeft = '22px'; custHead.style.paddingRight = '14px';
    custHead.style.backgroundColor = '#00000040'; custHead.style.borderBottom = '1px solid #ffffff14';
    var custTitle = lbl(custHead, 'CUSTOMIZE LOADOUT', S.value, 20); custTitle.style.fontWeight = 'bold';
    custTitle.style.letterSpacing = '2px'; custTitle.style.width = 'fill-parent-flow(1.0)'; custTitle.style.verticalAlign = 'center';
    var custClose = btn(custHead, '✕', function () { closeCustomize(); }, S.value);
    custClose.style.marginRight = '0px'; custClose.style.width = '42px';
    custClose.__lbl.style.fontSize = '18px'; custClose.__lbl.style.horizontalAlign = 'center';

    var custPickupBanner = mk('Panel', custWin); custPickupBanner.visible = false; custPickupBanner.hittest = false;
    custPickupBanner.style.width = '100%'; custPickupBanner.style.flowChildren = 'down';
    custPickupBanner.style.paddingLeft = '22px'; custPickupBanner.style.paddingRight = '22px';
    custPickupBanner.style.paddingTop = '10px'; custPickupBanner.style.paddingBottom = '10px';
    custPickupBanner.style.backgroundColor = '#3a2a10cc'; custPickupBanner.style.borderBottom = '1px solid #ffb34755';
    var custPickupLbl = lbl(custPickupBanner, '', '#ffdba6', 13);
    custPickupLbl.style.fontWeight = 'bold'; custPickupLbl.style.whiteSpace = 'normal';

    var custBody = mk('Panel', custWin); custBody.style.flowChildren = 'right'; custBody.style.width = '100%';
    custBody.style.height = 'fill-parent-flow(1.0)';
    custBody.style.paddingTop = '18px'; custBody.style.paddingBottom = '20px'; custBody.style.paddingLeft = '22px'; custBody.style.paddingRight = '22px';

    // LEFT: preview. This is the SAME native panel the loadout / item-inspect / end-of-round win
    // panel use (MapPlayerPreviewPanel) -- and the win panel proves it renders inside the in-game
    // HUD context. It needs the FULL creation attribute set (a `map` scene + camera + composition
    // layer + character mode); the earlier empty black box was a panel created without a `map`
    // scene, so it had nothing to render. Config mirrors panorama ref vanity-loadout.xml /
    // inspect.js. Created lazily on first open (the scene load is heavy); 2D card if it can't load.
    var prevWrap = mk('Panel', custBody); prevWrap.style.width = '46%'; prevWrap.style.height = '100%'; prevWrap.style.marginRight = '22px';
    prevWrap.style.backgroundColor = '#080a0e'; prevWrap.style.borderRadius = '6px'; prevWrap.style.border = '1px solid #ffffff14';
    prevWrap.style.flowChildren = 'down';
    prevWrap.style.overflow = 'clip';
    var preview3d = null, previewItem3d = null, preview3dTried = false, previewSerial = 0, previewModelKey = '';
    // Native 3D MapPlayerPreviewPanel (vanity loadout scene). The make-or-break detail is TIMING:
    // the panel must be CREATED after the modal overlay is visible AND has a laid-out size, or it
    // instantiates but renders black. So creation is deferred via $.Schedule from openCustomize
    // (recreatePreview), not done inline while the overlay is still 0-sized. The 2D card below is a
    // fallback shown only if the native panel fails to instantiate.
    var USE_3D_PREVIEW = true;

    // 2D loadout card (fallback): player header + one rarity-colored row per slot, so the spectated
    // player's actual weapons/knife/gloves (and any picked skins) stay readable if 3D is unavailable.
    var preview2d = mk('Panel', prevWrap); preview2d.style.width = '100%'; preview2d.style.height = '100%';
    preview2d.style.flowChildren = 'down'; preview2d.style.paddingTop = '30px';
    preview2d.style.paddingLeft = '30px'; preview2d.style.paddingRight = '30px'; preview2d.visible = true;
    var prev2dPlayer = lbl(preview2d, '', S.value, 23); prev2dPlayer.style.fontWeight = 'bold'; prev2dPlayer.style.letterSpacing = '1px';
    var prev2dTeam = lbl(preview2d, '', S.dim, 13); prev2dTeam.style.marginTop = '2px'; prev2dTeam.style.marginBottom = '22px';
    function prev2dRow(label) {
      var row = mk('Panel', preview2d); row.style.width = '100%'; row.style.flowChildren = 'down';
      row.style.marginBottom = '16px'; row.style.paddingLeft = '13px'; row.style.paddingTop = '3px'; row.style.paddingBottom = '3px';
      row.style.borderLeft = '3px solid ' + S.accent;
      var l = lbl(row, label, S.dim, 11); l.style.fontWeight = 'bold'; l.style.letterSpacing = '2px';
      var v = lbl(row, '', S.value, 17); v.style.fontWeight = 'bold'; v.style.marginTop = '3px';
      v.style.whiteSpace = 'nowrap'; v.style.textOverflow = 'ellipsis'; v.style.width = '100%';
      return { row: row, l: l, v: v };
    }
    var prev2dCard = {
      agent: prev2dRow('AGENT'), primary: prev2dRow('PRIMARY'), secondary: prev2dRow('SECONDARY'),
      knife: prev2dRow('MELEE / KNIFE'), gloves: prev2dRow('GLOVES')
    };
    function renderLoadoutCard() {
      prev2dPlayer.text = (custTargetName || 'Player').toUpperCase();
      var tm = normalizeTeamName(custTargetTeam);
      prev2dTeam.text = tm === 'CT' ? 'Counter-Terrorist' : (tm === 'T' ? 'Terrorist' : 'Spectated player');
      var slots = ['agent', 'primary', 'secondary', 'knife', 'gloves'];
      for (var i = 0; i < slots.length; i++) {
        var slot = slots[i], entry = prev2dCard[slot];
        var has = (slot === 'primary' || slot === 'secondary')
          ? !!(custLoadout[slot] && custLoadout[slot].defIndex > 0) : true;
        entry.row.visible = has;
        if (!has) continue;
        var opt = selOpt(slot, custSel[slot]);
        var meta = opt ? (opt[2] || {}) : {};
        var name = opt ? opt[0] : '—';
        if (slot !== 'agent') name = withWearLabel(slot, name);
        entry.v.text = name;
        var col = meta.color || S.value;
        entry.v.style.color = col;
        entry.row.style.borderLeft = '3px solid ' + col;
      }
    }

    // Default agent model for the spectated player's team (until we read their real model in C++).
    function teamDefaultModel() {
      return (normalizeTeamName(custTargetTeam) === 'CT') ? 'agents/models/ctm_sas/ctm_sas.vmdl'
                                                          : 'agents/models/tm_phoenix/tm_phoenix.vmdl';
    }
    function loadoutTeam() {
      return (normalizeTeamName(custTargetTeam) === 'CT') ? 'ct' : 't';
    }
    function loadoutItem(slot) {
      try { return LoadoutAPI.GetItemID(loadoutTeam(), slot) || ''; }
      catch (e) { return ''; }
    }
    function previewState() {
      var playerState = preview3d ? ('player=' + (preview3d.IsValid ? preview3d.IsValid() : '?'))
                                  : (preview3dTried ? 'player-fallback' : 'player-uninit');
      var itemState = previewItem3d ? (' item=' + (previewItem3d.IsValid ? previewItem3d.IsValid() : '?')) : ' item-uninit';
      return playerState + itemState;
    }
    // Scene presets for the MapPlayerPreviewPanel. CRUCIAL: the vanity/loadout scenes
    // (ui/buy_menu + cam_vanityloadout / cam_loadoutmenu_*) only render in the MAIN-MENU Panorama
    // root; in the in-game HUD (CSGOHud, where this editor lives) they instantiate but composite
    // nothing -> a black box. CS2's end-of-match WIN PANEL (hudwinpanel_background_map.js) is the
    // one MapPlayerPreviewPanel that DOES render inside the HUD, using ui/match_mvp + camera +
    // mvp-banner. So we default to that scene; the others are kept for live A/B over `previewTry`.
    var PREVIEW_SCENES = [
      { map: 'ui/buy_menu', camera: 'cam_vanityloadout', mode: 'buy-menu', pname: 'vanity_character', bg: 'true' },
      // bg 'false' (opaque) mirrors the native win panel (hudwinpanel MakeMvpMapPanel) EXACTLY -- a
      // transparent composition layer has nothing behind it in the HUD and renders pure black, which
      // is what made the preview look "black / disappearing". Opaque = the scene composites visibly.
      { map: 'ui/match_mvp', camera: 'camera', mode: 'mvp-banner', pname: 'mvp_char', bg: 'false' },
      { map: 'ui/buy_menu', camera: 'cam_loadoutmenu_ct', mode: 'buy-menu', pname: 'vanity_character', bg: 'true' }
    ];
    // 1 = ui/match_mvp (end-of-match win-panel scene). This is the ONE MapPlayerPreviewPanel scene
    // that actually composites inside the in-game HUD (CSGOHud) -- the vanity/buy_menu scenes (idx 0)
    // instantiate but render a black box outside the main-menu root, which is why the preview used to
    // "appear for a moment then disappear". See PREVIEW_SCENES note above.
    var previewSceneIdx = 1;
    function currentScene() { return PREVIEW_SCENES[previewSceneIdx] || PREVIEW_SCENES[0]; }
    // Create the native MapPlayerPreviewPanel with the current scene's attrs (optionally overridden,
    // for live tuning over netcon). Returns the panel or null.
    function createPreview3d(overrides) {
      var sc = currentScene();
      // EXACT native win-panel (hudwinpanel MakeMvpMapPanel) attribute set -- the extra attrs we used
      // to pass (player/initial_entity/sync_spawn_addons/hide_while_waiting/csm_override) are NOT set
      // by the native panel and appear to stop the composition layer from rendering in our injected
      // HUD subtree. The model is applied afterwards via SetPlayerModel (as the native does), not as
      // a creation attr.
      var attrs = {
        'require-composition-layer': 'true', 'pin-fov': 'vertical', 'transparent-background': sc.bg,
        'class': 'mvp_map', map: sc.map, camera: sc.camera, animgraphcharactermode: sc.mode,
        playername: sc.pname, mouse_rotate: 'false'
      };
      if (overrides && overrides.playermodel) attrs.playermodel = overrides.playermodel;
      var p = null;
      try {
        p = $.CreatePanel('MapPlayerPreviewPanel', prevWrap, 'CustPreview3D' + (++previewSerial), attrs);
        if (!(p && p.IsValid && p.IsValid())) p = null;
      } catch (e) { p = null; }
      if (p) {
        // Full-panel sizing like the native win panel (the old 136%/-17% crop was for the vanity
        // scene's off-centre character and can zero out the composition target).
        p.style.width = '100%';
        p.style.height = '100%';
        p.style.position = '0px 0px 0px';
      }
      return p;
    }
    function ensurePreview3d(model) {
      if (!USE_3D_PREVIEW) { preview2d.visible = true; return null; }
      model = model || teamDefaultModel();
      if (preview3d && previewModelKey !== model) {
        try { if (preview3d.DeleteAsync) preview3d.DeleteAsync(0); } catch (e) {}
        preview3d = null; preview3dTried = false;
      }
      if (preview3d || preview3dTried) return preview3d;
      preview3dTried = true;
      previewModelKey = model;
      preview3d = createPreview3d({ playermodel: model });
      preview2d.visible = !preview3d;
      return preview3d;
    }
    function ensureViewmodelPreview() {
      if (previewItem3d && previewItem3d.IsValid && previewItem3d.IsValid()) return previewItem3d;
      previewItem3d = null;
      try {
        previewItem3d = $.CreatePanel('MapItemPreviewPanel', prevWrap, 'CustViewmodel3D' + (++previewSerial), {
          map: 'ui/xpshop_item', camera: 'camera_weapon_0', 'require-composition-layer': 'true',
          player: 'false', initial_entity: 'item', mouse_rotate: 'true', sync_spawn_addons: 'true',
          'transparent-background': 'true', 'pin-fov': 'vertical', hittest: 'true',
          panzoom_enabled: 'true', hide_while_waiting_for_composite_materials: 'false'
        });
        if (!(previewItem3d && previewItem3d.IsValid && previewItem3d.IsValid())) previewItem3d = null;
      } catch (e) { previewItem3d = null; }
      if (previewItem3d) {
        previewItem3d.style.width = '100%';
        previewItem3d.style.height = '34%';
        previewItem3d.style.marginTop = '8px';
        safeCall(previewItem3d, 'SetHideStaticGeometry', true);
        safeCall(previewItem3d, 'SetReadyForDisplay', true);
      }
      return previewItem3d;
    }
    function setItemPreviewItem(panel, itemId) {
      if (!panel || itemId == null) return;
      safeCall(panel, 'SetActiveItem', 0);
      if (safeCall(panel, 'SetItemItemId', itemId, '') == null) safeCall(panel, 'SetItemItemId', itemId.toString(), '');
      safeCall(panel, 'StartWeaponLookat');
      safeCall(panel, 'SetReadyForDisplay', true);
    }
    function equipPlayerPreviewMeta(panel, meta) {
      if (!panel || !meta) return;
      var itemId = econItemId(meta);
      if (!itemId) return;
      if (safeCall(panel, 'EquipPlayerWithItem', itemId) == null) safeCall(panel, 'EquipPlayerWithItem', itemId.toString());
    }
    // Push the chosen agent (or the spectated player's team default) onto whichever preview exists.
    function applyPreview() {
      var skinSlot = (custSel.primary !== '0') ? 'primary' : 'secondary';
      var skin = selOpt(skinSlot, custSel[skinSlot]), agent = selOpt('agent', custSel.agent);
      var knife = selOpt('knife', custSel.knife), gloves = selOpt('gloves', custSel.gloves);
      var model = (agent && agent[2] && agent[2].model) ? agent[2].model : teamDefaultModel();
      ensurePreview3d(model);
      if (preview3d) {
        var sc = currentScene();
        if (sc.startCamera) safeCall(preview3d, 'TransitionToCamera', sc.startCamera, 0);
        safeCall(preview3d, 'SetActiveCharacter', 0); // char 0 = the single previewed agent
        safeCall(preview3d, 'SetPlayerModel', model);
        equipPlayerPreviewMeta(preview3d, skin ? skin[2] : null);
        equipPlayerPreviewMeta(preview3d, knife ? knife[2] : null);
        equipPlayerPreviewMeta(preview3d, gloves ? gloves[2] : null);
        safeCall(preview3d, 'SetReadyForDisplay', true);
      } else {
        renderLoadoutCard();
      }
    }

    // The native MapPlayerPreviewPanel only composites its scene when its parent is VISIBLE and has
    // a real laid-out size. Creating it while the modal overlay was still hidden (the old open
    // order) left it permanently blank -- it only "worked" when external automation re-poked it
    // after the modal was already up. So we make the overlay visible first, then re-assert the
    // preview for a few frames from render() (layout settles a frame or two after visible flips).
    var previewPokeFrames = 0;
    function pokePreviewSoon() { previewPokeFrames = 16; }
)EDJS"
R"EDJS(
    function maintainPreview() {
      if (!custOverlay.visible) return;
      // Keep the composition-layer panel marked ready EVERY frame while the modal is open. Without
      // this continuous (cheap, idempotent) re-assert the scene composites once and then lapses to
      // black after the initial settle burst -- the "appears then disappears" bug. The heavier
      // re-equip/re-model (applyPreview) only runs during the post-open settle frames.
      if (preview3d && preview3d.IsValid && preview3d.IsValid())
        safeCall(preview3d, 'SetReadyForDisplay', true);
      if (previewPokeFrames > 0) { previewPokeFrames--; applyPreview(); }
    }
    // Destroy + rebuild the 3D preview from scratch. Must run when the modal is already visible and
    // laid out (deferred via $.Schedule from openCustomize) -- creating it on a 0-sized parent makes
    // it render black even though it reports valid.
    function recreatePreview() {
      if (!custOverlay.visible) return;
      if (!USE_3D_PREVIEW) { preview2d.visible = true; renderLoadoutCard(); return; }
      try { if (preview3d && preview3d.DeleteAsync) preview3d.DeleteAsync(0); } catch (e) {}
      preview3d = null; preview3dTried = false; previewModelKey = '';
      applyPreview();      // creates the panel + equips + SetReadyForDisplay
      pokePreviewSoon();   // re-assert SetReadyForDisplay for the frames after the new panel lays out
    }

)EDJS"
R"EDJS(
    function normalizeTeamName(t) {
      t = (t || '').toString().toUpperCase();
      if (t === '3' || t === 'CT' || t.indexOf('COUNTER') >= 0) return 'CT';
      if (t === '2' || t === 'T' || t.indexOf('TERROR') >= 0) return 'T';
      return '';
    }
    function normalizeLoadoutWeapon(w) {
      if (!w || !(parseInt(w.defIndex || 0, 10) > 0)) return null;
      return {
        entityIndex: parseInt(w.entityIndex || -1, 10),
        defIndex: parseInt(w.defIndex || 0, 10) || 0,
        paintKit: parseInt(w.paintKit || 0, 10) || 0,
        wear: num(w.wear)
      };
    }
    function collectPanelText(panel, out, depth) {
      if (!panel || depth > 24) return;
      try {
        if (typeof panel.text === 'string' && panel.text) out.push(panel.text);
      } catch (e) {}
      var count = 0;
      try { count = panel.GetChildCount ? panel.GetChildCount() : 0; } catch (e2) { count = 0; }
      for (var i = 0; i < count; i++) {
        var child = null;
        try { child = panel.GetChild(i); } catch (e3) { child = null; }
        collectPanelText(child, out, depth + 1);
      }
    }
    function hudWeaponText() {
      var roots = ['HudWeaponSelection', 'hud-WPN-main', 'HudAmmo'];
      var text = [];
      for (var i = 0; i < roots.length; i++) {
        var p = null;
        try { p = $.GetContextPanel().FindChildTraverse(roots[i]); } catch (e) { p = null; }
        collectPanelText(p, text, 0);
      }
      return text.join(' | ');
    }
    function inferActivePaintFromHud(slot) {
      if (!slot) return null;
      var activeText = hudWeaponText();
      if (!activeText) return null;
      var arr = COSMETICS[slot] || [];
      var activeDef = parseInt(custActiveWeaponDef || 0, 10) || 0;
      for (var i = 0; i < arr.length; i++) {
        var label = arr[i][0] || '', meta = arr[i][2] || {};
        var def = parseInt(meta.def || 0, 10) || 0;
        var paint = parseInt(meta.paint || meta.paintKit || 0, 10) || 0;
        if (!label || paint <= 0 || def !== activeDef) continue;
        if (activeText.indexOf(label) >= 0) {
          return { value: arr[i][1], defIndex: def, paintKit: paint };
        }
      }
      return null;
    }
    function applyCustomizeTargetLoadout(ct) {
      custLoadout = { primary: null, secondary: null, knife: null, gloves: null, agentModel: '' };
      if (ct && ct.weapons) {
        custLoadout.primary = normalizeLoadoutWeapon(ct.weapons.primary);
        custLoadout.secondary = normalizeLoadoutWeapon(ct.weapons.secondary);
        custLoadout.knife = normalizeLoadoutWeapon(ct.weapons.knife);
        custLoadout.gloves = normalizeLoadoutWeapon(ct.weapons.gloves);
      }
      if (ct && typeof ct.agentModel === 'string') custLoadout.agentModel = ct.agentModel;
      var activeSlot = WEAPON_SLOT_BY_DEF[custActiveWeaponDef] || '';
      if (activeSlot && !custLoadout[activeSlot])
        custLoadout[activeSlot] = { entityIndex: parseInt(ct && ct.activeWeaponIndex || -1, 10), defIndex: custActiveWeaponDef, paintKit: 0, wear: 0.01 };
      if (activeSlot && custLoadout[activeSlot] && !(parseInt(custLoadout[activeSlot].paintKit || 0, 10) > 0)) {
        var inferred = inferActivePaintFromHud(activeSlot);
        if (inferred) custLoadout[activeSlot].paintKit = inferred.paintKit;
      }
    }
    function customizeLoadoutSignature() {
      function part(slot) {
        var w = custLoadout[slot] || {};
        return [parseInt(w.defIndex || 0, 10) || 0, parseInt(w.paintKit || 0, 10) || 0, Number(w.wear || 0).toFixed(4)].join(':');
      }
      return [custTargetKey || '', normalizeTeamName(custTargetTeam), part('primary'), part('secondary'), part('knife'), part('gloves'), custLoadout.agentModel || ''].join('|');
    }
    // Distinct weapons available for a primary/secondary slot (the weapon-selector list). Derives one
    // entry per weapon def from the full skin catalog, preferring a real weapon name ("AK-47") over the
    // generic "Default ... skin" label, and carrying that weapon's default thumbnail for the swatch.
    function weaponsForSlot(slot) {
      var arr = COSMETICS[slot] || [];
      var byDef = {}, order = [];
      for (var i = 0; i < arr.length; i++) {
        var m = arr[i][2] || {};
        var def = parseInt(m.def || 0, 10) || 0;
        if (def <= 0) continue;
        var label = '' + arr[i][0];
        var hasSkinName = label.indexOf('|') >= 0;
        var nm = (hasSkinName ? label.split('|')[0] : label).replace(/^\s+|\s+$/g, '');
        if (!byDef[def]) { byDef[def] = { name: nm, color: m.color, img: m.img }; order.push(def); }
        else if (hasSkinName) { byDef[def].name = nm; } // prefer a real "AK-47"-style name
        if (parseInt(m.paint || 0, 10) === 0 && m.img && !byDef[def].img) byDef[def].img = m.img;
      }
      order.sort(function (a, b) { return (byDef[a].name < byDef[b].name) ? -1 : 1; });
      var out = [];
      for (var j = 0; j < order.length; j++) {
        var d = order[j];
        out.push([byDef[d].name || ('Weapon ' + d), '' + d,
          { color: byDef[d].color || rarColor(0), def: d, paint: 0, img: byDef[d].img }]);
      }
      return out;
    }
    // The weapon def the primary/secondary skin dropdown currently targets: an explicitly-picked weapon
    // if any, else the player's held weapon, else the first weapon in the catalog (so the block is
    // usable even when the player is not holding a weapon in that slot).
    function effectiveSlotDef(slot) {
      if (slot !== 'primary' && slot !== 'secondary') return 0;
      if (custWeaponDef[slot] > 0) return custWeaponDef[slot];
      var held = (custLoadout[slot] && parseInt(custLoadout[slot].defIndex || 0, 10)) || 0;
      if (held > 0) return held;
      var ws = weaponsForSlot(slot);
      return ws.length ? (parseInt(ws[0][1], 10) || 0) : 0;
    }
    function weaponLabelForDef(def) {
      def = parseInt(def || 0, 10) || 0;
      if (def <= 0) return 'weapon';
      var slot = WEAPON_SLOT_BY_DEF[def] || '';
      if (slot === 'primary' || slot === 'secondary') {
        var ws = weaponsForSlot(slot);
        for (var i = 0; i < ws.length; i++) if (parseInt(ws[i][1], 10) === def) return ws[i][0];
      }
      return 'Weapon ' + def;
    }
    function shouldRoutePickupToOwner(slot) {
      if (slot !== 'primary' && slot !== 'secondary') return false;
      if (!custActiveWeaponPickup || !custActiveWeaponOwnerSteam || custActiveWeaponDef <= 0) return false;
      if (custActiveWeaponSlot !== slot) return false;
      return effectiveSlotDef(slot) === custActiveWeaponDef;
    }
    function resolveCosmeticTarget(slot) {
      var target = (custTargetKey && custTargetKey.indexOf('steam:') === 0)
        ? custTargetKey.substring(6) : 'current';
      if (shouldRoutePickupToOwner(slot)) return custActiveWeaponOwnerSteam;
      return target;
    }
    function updatePickupBanner() {
      if (!custActiveWeaponPickup || !custActiveWeaponOwnerSteam) {
        custPickupBanner.visible = false;
        return;
      }
      var wn = weaponLabelForDef(custActiveWeaponDef);
      var on = custActiveWeaponOwnerName || ('Player ' + custActiveWeaponOwnerSteam);
      custPickupLbl.text = 'Holding ' + on + '\'s ' + wn + ' (picked up). Finish changes apply to ' + on + '\'s ' + wn + ' loadout, not ' + (custTargetName || 'this player') + '.';
      custPickupBanner.visible = true;
    }
    function filteredOptions(slot) {
      var arr = COSMETICS[slot] || [];
      if (slot === 'primary' || slot === 'secondary') {
        var slotDef = effectiveSlotDef(slot);
        if (slotDef <= 0) return [];
        var weaponOut = [];
        for (var wi = 0; wi < arr.length; wi++) {
          var wm = arr[wi][2] || {};
          var wPaint = parseInt(wm.paint || wm.paintKit || 0, 10) || 0;
          var wDef = parseInt(wm.def || 0, 10) || 0;
          if ((wPaint === 0 && (wDef === 0 || wDef === slotDef)) || (wPaint !== 0 && wDef === slotDef))
            weaponOut.push(arr[wi]);
        }
        return weaponOut;
      }
      if (slot === 'knife') return knifeOptionsWithCurrentDefault(arr);
      if (slot !== 'agent') return arr;
      var team = normalizeTeamName(custTargetTeam);
      var out = [];
      for (var i = 0; i < arr.length; i++) {
        var meta = arr[i][2] || {};
        if (!team || meta.team === 'ANY' || meta.team === team) out.push(arr[i]);
      }
      return out.length ? out : arr;
    }
    function knifeNameForDef(defIndex, arr) {
      if (defIndex === 42) return 'Default CT knife';
      if (defIndex === 59) return 'Default T knife';
      for (var i = 0; i < arr.length; i++) {
        var meta = arr[i][2] || {};
        if ((parseInt(meta.def || 0, 10) || 0) !== defIndex) continue;
        var label = arr[i][0] || '';
        var split = label.indexOf(' | ');
        return split > 0 ? label.substring(0, split) : label;
      }
      return 'Knife #' + defIndex;
    }
    function knifeOptionsWithCurrentDefault(arr) {
      var w = custLoadout.knife || null;
      var def = w ? (parseInt(w.defIndex || 0, 10) || 0) : 0;
      if (def <= 0) return arr;
      var value = def + ':0';
      for (var i = 0; i < arr.length; i++) if (arr[i][1] === value) return arr;
      var out = [[knifeNameForDef(def, arr), value, { color: rarColor(0), def: def, paint: 0 }]];
      for (var j = 0; j < arr.length; j++) out.push(arr[j]);
      return out;
    }
    function clampWearForMeta(meta, value) {
      var lo = (meta && typeof meta.wearMin === 'number') ? meta.wearMin : 0.0;
      var hi = (meta && typeof meta.wearMax === 'number') ? meta.wearMax : 1.0;
      value = clamp01(value);
      if (value < lo) value = lo;
      if (value > hi) value = hi;
      return value;
    }
    function wearValue(slot) {
      var w = custWear[slot] || (custWear[slot] = { preset: 'fn', custom: 0.01 });
      var preset = WEAR_PRESETS[w.preset] || WEAR_PRESETS.fn;
      var raw = (w.preset === 'custom') ? num(w.custom) : preset.value;
      var opt = selOpt(slot, custSel[slot]);
      return clampWearForMeta(opt ? opt[2] : null, raw);
    }
    function wearPresetLabel(slot) {
      var w = custWear[slot] || {};
      return (WEAR_PRESETS[w.preset] || WEAR_PRESETS.fn).label;
    }
    function withWearLabel(slot, label) {
      if (!label || !custWear[slot]) return label || '';
      var opt = selOpt(slot, custSel[slot]);
      var meta = opt ? (opt[2] || {}) : {};
      if (!meta.paint) return label;
      var suffix = wearPresetLabel(slot);
      if ((custWear[slot] || {}).preset === 'custom') suffix = 'Float ' + wearValue(slot).toFixed(2);
      return label + ' · ' + suffix;
    }
    function defaultWearForSlot(slot) {
      return slot === 'gloves' ? { preset: 'ft', custom: 0.22 } : { preset: 'fn', custom: 0.01 };
    }
    function presetForWearValue(value) {
      value = clamp01(num(value));
      if (value < 0.07) return 'fn';
      if (value < 0.15) return 'mw';
      if (value < 0.38) return 'ft';
      if (value < 0.45) return 'ww';
      return 'bs';
    }
    function copyWearState(w, slot) {
      var d = defaultWearForSlot(slot);
      w = w || d;
      var custom = (typeof w.custom === 'number' && isFinite(w.custom)) ? w.custom : d.custom;
      return { preset: w.preset || d.preset, custom: custom };
    }
    function itemWearKey(slot, value) {
      return slot + ':' + (value === undefined || value === null ? (custSel[slot] || '0') : ('' + value));
    }
    function saveActiveItemWear(slot) {
      if (!wearControls || !wearControls[slot]) return;
      custItemWear[itemWearKey(slot)] = copyWearState(custWear[slot], slot);
    }
    function loadActiveItemWear(slot) {
      if (!wearControls || !wearControls[slot]) return;
      var key = itemWearKey(slot);
      custWear[slot] = copyWearState(custItemWear[key], slot);
    }
)EDJS"
R"EDJS(
    function decoratedOptions(slot) {
      var src = filteredOptions(slot), out = [];
      for (var i = 0; i < src.length; i++) {
        var meta = src[i][2] || {};
        var m = {}; for (var k in meta) m[k] = meta[k];
        out.push([withWearLabel(slot, src[i][0]), src[i][1], m]);
      }
      return out;
    }
)EDJS"
R"EDJS(
    function optionExists(slot, value) {
      var arr = filteredOptions(slot);
      for (var i = 0; i < arr.length; i++) if (arr[i][1] === value) return true;
      return false;
    }
    function loadoutDefaultSelection(slot) {
      var w = custLoadout[slot] || null;
      if (!w || !(parseInt(w.defIndex || 0, 10) > 0)) return '0';
      var def = parseInt(w.defIndex || 0, 10) || 0;
      var paint = parseInt(w.paintKit || 0, 10) || 0;
      if ((slot === 'primary' || slot === 'secondary') && paint > 0 && optionExists(slot, '' + paint)) return '' + paint;
      if ((slot === 'primary' || slot === 'secondary') && optionExists(slot, def + ':0')) return def + ':0';
      if (slot === 'knife' && paint > 0 && optionExists(slot, def + ':' + paint)) return def + ':' + paint;
      if (slot === 'knife' && optionExists(slot, def + ':0')) return def + ':0';
      return '0';
    }
    // Gloves: if the spectated player wears custom gloves, default to that glove type; otherwise
    // fall back to the team's default gloves (CT 5029 / T 5028), per the "no gloves -> default by
    // side" rule. The exact glove finish isn't networked through the fallback fields, so a custom
    // pair resolves to the first finish of that glove model (its type is what matters here).
    function glovesDefaultSelection() {
      var arr = COSMETICS.gloves || [];
      var w = custLoadout.gloves || null;
      var def = w ? (parseInt(w.defIndex || 0, 10) || 0) : 0;
      var paint = w ? (parseInt(w.paintKit || 0, 10) || 0) : 0;
      if (def > 0 && paint > 0) {
        for (var i = 0; i < arr.length; i++) {
          var m = arr[i][2] || {};
          if ((parseInt(m.def || 0, 10) || 0) === def && (parseInt(m.paint || 0, 10) || 0) === paint) return arr[i][1];
        }
      }
      return (normalizeTeamName(custTargetTeam) === 'CT') ? '5029:0' : '5028:0';
    }
    function agentDefaultSelection() {
      // Match the player's live model path (read off the pawn, e.g.
      // 'agents/models/ctm_st6/ctm_st6_variantg.vmdl') to a catalog agent by its meta.model.
      // NOTE: the catalog key is SINGULAR 'agent' (matching the slot name), not 'agents'.
      var model = custLoadout.agentModel || '';
      if (model) {
        var arr = COSMETICS.agent || [];
        for (var i = 0; i < arr.length; i++) {
          var m = arr[i][2] || {};
          if (m.model && m.model === model) return arr[i][1];
        }
      }
      return 'default';
    }
    function resetSelectionsFromLoadout() {
      custWeaponDef = { primary: 0, secondary: 0 }; // follow held weapons for a freshly-opened player
      custSel = {
        agent: agentDefaultSelection(),
        primary: loadoutDefaultSelection('primary'),
        secondary: loadoutDefaultSelection('secondary'),
        knife: loadoutDefaultSelection('knife'),
        gloves: glovesDefaultSelection()
      };
      custWear = {
        primary: defaultWearForSlot('primary'),
        secondary: defaultWearForSlot('secondary'),
        knife: defaultWearForSlot('knife'),
        gloves: defaultWearForSlot('gloves')
      };
      for (var slot in custLoadout) {
        var w = custLoadout[slot];
        if (!w || !(parseInt(w.paintKit || 0, 10) > 0)) continue;
        var wear = clamp01(num(w.wear));
        custWear[slot] = { preset: presetForWearValue(wear), custom: wear };
      }
      custItemWear = {};
      saveActiveItemWear('primary');
      saveActiveItemWear('secondary');
      saveActiveItemWear('knife');
      saveActiveItemWear('gloves');
    }
    function currentPlayerKey() {
      return custTargetKey || ('pawn:' + custTargetIndex);
    }
    function restorePlayerSelections() {
      var key = currentPlayerKey();
      if (custPersist[key] && custPersist[key].baseSig === customizeLoadoutSignature()) {
        custSel = {
          agent: custPersist[key].sel.agent || 'default',
          primary: custPersist[key].sel.primary || '0',
          secondary: custPersist[key].sel.secondary || '0',
          knife: custPersist[key].sel.knife || '0',
          gloves: custPersist[key].sel.gloves || '0'
        };
        custWear = custPersist[key].wear || custWear;
        custItemWear = custPersist[key].itemWear || {};
        custWeaponDef = custPersist[key].weaponDef || { primary: 0, secondary: 0 };
        loadActiveItemWear('primary');
        loadActiveItemWear('secondary');
        loadActiveItemWear('knife');
        loadActiveItemWear('gloves');
      } else {
        resetSelectionsFromLoadout();
      }
    }
    function persistPlayerSelections() {
      if (custTargetIndex < 0) return;
      saveActiveItemWear('primary');
      saveActiveItemWear('secondary');
      saveActiveItemWear('knife');
      saveActiveItemWear('gloves');
      var copyWear = {};
      for (var s in custWear) copyWear[s] = { preset: custWear[s].preset, custom: custWear[s].custom };
      var copyItemWear = {};
      for (var k in custItemWear) copyItemWear[k] = { preset: custItemWear[k].preset, custom: custItemWear[k].custom };
      custPersist[currentPlayerKey()] = { baseSig: customizeLoadoutSignature(), sel: { agent: custSel.agent, primary: custSel.primary, secondary: custSel.secondary, knife: custSel.knife, gloves: custSel.gloves }, wear: copyWear, itemWear: copyItemWear, weaponDef: { primary: custWeaponDef.primary, secondary: custWeaponDef.secondary } };
    }

    // RIGHT: final loadout-style controls. No arms/viewmodel override and no CT/T glove split.
)EDJS"
R"EDJS(
    var ctrlCol = mk('Panel', custBody); ctrlCol.style.width = 'fill-parent-flow(1.0)'; ctrlCol.style.height = '100%';
    ctrlCol.style.flowChildren = 'down'; ctrlCol.style.overflow = 'squish scroll';
    function slotBlock(title) {
      var b = mk('Panel', ctrlCol); b.style.flowChildren = 'down'; b.style.width = '100%'; b.style.marginBottom = '14px';
      var t = lbl(b, title, S.dim, 11); t.style.fontWeight = 'bold'; t.style.letterSpacing = '2px'; t.style.marginBottom = '5px';
      return b;
    }
    function miniLabel(parent, text) {
      var l = lbl(parent, text, S.dim, 10); l.style.marginTop = '4px'; l.style.marginBottom = '2px';
      l.style.letterSpacing = '1px'; return l;
    }
    function wearBlock(parent, slot) {
      var wrap = mk('Panel', parent); wrap.style.width = '100%'; wrap.style.flowChildren = 'down'; wrap.style.marginTop = '6px';
      var top = mk('Panel', wrap); top.style.width = '100%'; top.style.flowChildren = 'right';
      var label = lbl(top, '', S.dim, 12); label.style.width = 'fill-parent-flow(1.0)'; label.style.verticalAlign = 'center';
      var preset = itemDrop(wrap, 'CustWear' + slot, function (v) { setWear(slot, v, null); });
      var custom = $.CreatePanel('TextEntry', wrap, 'CustWearFloat' + slot, { placeholder: '0.00 - 1.00' });
      custom.style.width = '100%'; custom.style.height = '32px'; custom.style.marginTop = '5px';
      custom.style.backgroundColor = '#101823'; custom.style.border = '1px solid #ffffff24';
      custom.style.color = S.value; custom.style.fontSize = '14px'; custom.style.paddingLeft = '8px';
      custom.SetPanelEvent('ontextentrychange', function () {
        if (custWearUpdating) return;
        var v = parseFloat(custom.text || '0');
        if (!isFinite(v)) v = 0;
        setWear(slot, 'custom', v);
      });
      return { label: label, preset: preset, custom: custom };
    }
    var agentBlock = slotBlock('AGENT');
    var agentDrop  = itemDrop(agentBlock, 'CustAgent', function (v) { pickCosmetic('agent', v); });
    var primaryBlock = slotBlock('PRIMARY');
    miniLabel(primaryBlock, 'Weapon');
    var primaryWeaponDrop = itemDrop(primaryBlock, 'CustPrimaryWeapon', function (v) { pickWeapon('primary', v); });
    miniLabel(primaryBlock, 'Finish');
    var primaryDrop = itemDrop(primaryBlock, 'CustPrimary', function (v) { pickCosmetic('primary', v); });
    var primaryWear = wearBlock(primaryBlock, 'primary');
    var secondaryBlock = slotBlock('SECONDARY');
    miniLabel(secondaryBlock, 'Weapon');
    var secondaryWeaponDrop = itemDrop(secondaryBlock, 'CustSecondaryWeapon', function (v) { pickWeapon('secondary', v); });
    miniLabel(secondaryBlock, 'Finish');
    var secondaryDrop = itemDrop(secondaryBlock, 'CustSecondary', function (v) { pickCosmetic('secondary', v); });
    var secondaryWear = wearBlock(secondaryBlock, 'secondary');
    var knifeBlock = slotBlock('MELEE / KNIFE');
    var knifeDrop  = itemDrop(knifeBlock, 'CustKnife', function (v) { pickCosmetic('knife', v); });
    var knifeWear = wearBlock(knifeBlock, 'knife');
    var glovesBlock = slotBlock('GLOVES');
    var glovesDrop = itemDrop(glovesBlock, 'CustGloves', function (v) { pickCosmetic('gloves', v); });
    var glovesWear = wearBlock(glovesBlock, 'gloves');
    var wearControls = { primary: primaryWear, secondary: secondaryWear, knife: knifeWear, gloves: glovesWear };
    var wearDrops = { primary: primaryWear.preset, secondary: secondaryWear.preset, knife: knifeWear.preset, gloves: glovesWear.preset };
    // Selected option's [label, meta] for a slot (for the preview).
    function selOpt(slot, value) {
      var arr = filteredOptions(slot);
      for (var i = 0; i < arr.length; i++) if (arr[i][1] === value) return arr[i];
      return arr.length ? arr[0] : null;
    }
    function coerceSelection(slot) {
      if ((slot === 'primary' || slot === 'secondary') && !(custLoadout[slot] && custLoadout[slot].defIndex > 0)) {
        custSel[slot] = '0';
        return;
      }
      var arr = filteredOptions(slot);
      if (!arr.length) return;
      for (var i = 0; i < arr.length; i++) if (arr[i][1] === custSel[slot]) return;
      saveActiveItemWear(slot);
      custSel[slot] = arr[0][1];
      loadActiveItemWear(slot);
    }
    // Preview refresh -> drive the 3D character (or the 2D fallback). applyPreview() is defined in
    // the preview block above.
    function updatePreview() { applyPreview(); }
    function updateWearControl(slot) {
      var wc = wearControls[slot]; if (!wc) return;
      var w = custWear[slot] || (custWear[slot] = { preset: 'fn', custom: 0.01 });
      wc.preset.update(WEAR_OPTIONS, w.preset);
      wc.custom.visible = w.preset === 'custom';
      custWearUpdating = true;
      wc.custom.text = wearValue(slot).toFixed(2);
      custWearUpdating = false;
      wc.label.text = 'Wear: ' + wearPresetLabel(slot) + ' (' + wearValue(slot).toFixed(2) + ')';
    }
    function setWear(slot, preset, customValue) {
      if (!custWear[slot]) custWear[slot] = { preset: 'fn', custom: 0.01 };
      if (preset) custWear[slot].preset = preset;
      if (customValue !== null && customValue !== undefined) custWear[slot].custom = clamp01(customValue);
      saveActiveItemWear(slot);
      persistPlayerSelections();
      populateCustomize();
      updatePreview();
      applyCosmeticCommand(slot);
    }
    function applyCosmeticCommand(slot) {
      // The agent slot has no wear entry (agents are not skinned), so it must NOT be gated on
      // custWear[slot] -- doing so silently dropped every agent pick (no command was ever sent).
      if (custTargetIndex < 0) return;
      if (slot !== 'agent' && !custWear[slot]) return;
      // Target the player by SteamID -- the backend store is SteamID-keyed. Picked-up weapons keep the
      // original owner's econ id, so route finish commands to that owner when editing the held pickup.
      var target = resolveCosmeticTarget(slot);
      var routedPickup = shouldRoutePickupToOwner(slot);
      var opt = selOpt(slot, custSel[slot]);
      var meta = opt ? (opt[2] || {}) : {};
      var def = parseInt(meta.def || 0, 10) || 0;
      var pk = parseInt(meta.paint || meta.paintKit || 0, 10) || 0;
      var wear = wearValue(slot).toFixed(4);
      // Echo the VERBATIM skin label the user clicked (exactly the dropdown text, e.g. "AK-47 |
      // Redline" / "Skeleton Knife") to the game console + MVM debug log, so the human-readable name
      // appears right next to the native before/after weapon snapshot. Strip characters that would
      // break the console-command tokenizer; the native "uilog" handler joins the tokens back.
      var label = (opt && opt[0]) ? ('' + opt[0]) : '(unknown)';
      var safeLabel = label.replace(/["';\r\n]+/g, ' ').replace(/\s+/g, ' ');
      var routeNote = routedPickup ? (' [owner ' + custActiveWeaponOwnerSteam + ']') : '';
      cmd('mirv_filmmaker cosmetics uilog [' + slot + '] ' + safeLabel + routeNote + ' (def ' + def + ' paint ' + pk + ' wear ' + wear + ')');
      var sent = false;
      var command = '';
      if ((slot === 'primary' || slot === 'secondary') && def > 0) {
        command = 'mirv_filmmaker cosmetics player ' + target + ' weapon ' + def + ' paint ' + pk + ' wear ' + wear + ' seed 0';
        sent = true;
      } else if (slot === 'knife' && def > 0) {
        command = 'mirv_filmmaker cosmetics player ' + target + ' knife ' + def + ' paint ' + pk + ' wear ' + wear + ' seed 0';
        sent = true;
      } else if (slot === 'gloves' && def > 0) {
        command = 'mirv_filmmaker cosmetics player ' + target + ' gloves ' + def + ' paint ' + pk + ' wear ' + wear + ' seed 0';
        sent = true;
      } else if (slot === 'agent') {
        command = 'mirv_filmmaker cosmetics player ' + target + ' agent ' + (meta.model || 'default');
        sent = true;
      }
      // Enable before sending the profile mutation so normal UI use does not emit the temporary
      // "cosmetics are disabled" hint from the native command handler.
      if (sent) {
        cmd('mirv_filmmaker cosmetics enabled 1');
        cmd(command);
      }
    }
    // Apply a selection. Updates UI + preview immediately, persists per current player, and sends
    // the matching offline-demo cosmetic command where the native path exists.
    function pickCosmetic(slot, value) {
      saveActiveItemWear(slot);
      custSel[slot] = value;
      loadActiveItemWear(slot);
      persistPlayerSelections();
      populateCustomize();
      updatePreview();
      applyCosmeticCommand(slot);
    }
    // Re-target which weapon a primary/secondary block edits (the weapon selector). Resets the finish
    // dropdown to that weapon's vanilla default and repopulates; does NOT emit a cosmetic command on its
    // own -- only picking a FINISH applies (so merely browsing weapons changes nothing in the demo).
    function pickWeapon(slot, value) {
      if (slot !== 'primary' && slot !== 'secondary') return;
      var def = parseInt(value, 10) || 0;
      if (def <= 0) return;
      saveActiveItemWear(slot);
      custWeaponDef[slot] = def;
      custSel[slot] = optionExists(slot, def + ':0') ? (def + ':0') : '0';
      loadActiveItemWear(slot);
      persistPlayerSelections();
      populateCustomize();
      updatePreview();
    }
    function populateCustomize() {
      coerceSelection('agent');
      coerceSelection('primary');
      coerceSelection('secondary');
      coerceSelection('knife');
      coerceSelection('gloves');
      // Primary/secondary blocks are ALWAYS shown now: the weapon selector lets the user assign a skin
      // to any weapon, including ones the player is not currently holding.
      primaryBlock.visible = true;
      secondaryBlock.visible = true;
      knifeBlock.visible = true;
      agentDrop.update(decoratedOptions('agent'), custSel.agent);
      primaryWeaponDrop.update(weaponsForSlot('primary'), '' + effectiveSlotDef('primary'));
      secondaryWeaponDrop.update(weaponsForSlot('secondary'), '' + effectiveSlotDef('secondary'));
      primaryDrop.update(decoratedOptions('primary'), custSel.primary);
      secondaryDrop.update(decoratedOptions('secondary'), custSel.secondary);
      knifeDrop.update(decoratedOptions('knife'), custSel.knife);
      glovesDrop.update(decoratedOptions('gloves'), custSel.gloves);
      updateWearControl('primary');
      updateWearControl('secondary');
      updateWearControl('knife');
      updateWearControl('gloves');
      updatePickupBanner();
    }
    function openCustomize() {
      closeAllDrops(); closeSettings();
      restorePlayerSelections();
      custTitle.text = 'CUSTOMIZE PLAYER · ' + (custTargetName || 'PLAYER').toUpperCase();
      populateCustomize();
      custDisplayedLoadoutSig = customizeLoadoutSignature();
      custOverlay.visible = true; // visible BEFORE building the 3D preview so its scene composites
      updatePreview();
      pokePreviewSoon();          // re-assert the scene over the next frames once layout settles
      // CREATE the 3D panel only AFTER the overlay is visible AND laid out (next layout passes).
      try { $.Schedule(0.08, recreatePreview); $.Schedule(0.30, recreatePreview); } catch (e) { recreatePreview(); }
    }
    function closeCustomize() { closeAllDrops(); custOverlay.visible = false; }
    var customizeDrops = { agent: agentDrop, primary: primaryDrop, secondary: secondaryDrop, knife: knifeDrop, gloves: glovesDrop };
)EDJS"
R"EDJS(
    // Persistent "Customize" button pinned to the editor's BOTTOM-RIGHT corner. It is a child of
    // root (NOT the scrolling inspector), so it stays put across Path/Follow/Attach/Lock-on tabs
    // and is always visible while the editor is open (visibility set in render()).
    var customizeBtn = mk('Panel', root); customizeBtn.hittest = true; customizeBtn.visible = false;
    customizeBtn.style.horizontalAlign = 'right'; customizeBtn.style.verticalAlign = 'bottom';
    customizeBtn.style.marginRight = '16px'; customizeBtn.style.marginBottom = '16px';
    customizeBtn.style.zIndex = '120'; customizeBtn.style.backgroundColor = S.accent;
    customizeBtn.style.borderRadius = '5px';
    customizeBtn.style.paddingTop = '11px'; customizeBtn.style.paddingBottom = '11px';
    customizeBtn.style.paddingLeft = '22px'; customizeBtn.style.paddingRight = '22px';
    customizeBtn.style.boxShadow = '#000000aa 0px 3px 12px 2px';
    var customizeBtnLbl = lbl(customizeBtn, '✦  CUSTOMIZE PLAYER', '#10141aff', 15);
    customizeBtnLbl.style.fontWeight = 'bold'; customizeBtnLbl.style.letterSpacing = '1px';
    customizeBtn.SetPanelEvent('onactivate', function () { openCustomize(); });
)EDJS"
