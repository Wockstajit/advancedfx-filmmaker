#!/usr/bin/env python3
"""Offline demo pre-scan: build probe ticks + fmjson anchors for weapon-moment discovery.

Phase 1 of the weapon-model verification pipeline. Reads <demo>.fmjson (or runs
FilmmakerDemoInfo.exe) and emits probe ticks + pickup/death anchors. Live paint/skin
data is filled in by scan-cosmetics-weapon-moments.ps1 at playback time.

Usage:
  python scan_demo_weapon_moments.py <demo.dem> [--out plan.json] [--helper path]
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Set

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
STEAM64_OFFSET = 76561197960265728
sys.path.insert(0, str(HERE))

from weapon_skin_database import get_database, is_verifiable_gun_def, slot_for_def  # noqa: E402


def find_demo_helper() -> Optional[Path]:
    candidates = [
        ROOT / "build" / "staging-release" / "bin" / "x64" / "FilmmakerDemoInfo" / "FilmmakerDemoInfo.exe",
        ROOT / "build" / "staging-release" / "bin" / "FilmmakerDemoInfo" / "FilmmakerDemoInfo.exe",
        ROOT / "FilmmakerDemoInfoGo" / "FilmmakerDemoInfo.exe",
    ]
    for p in candidates:
        if p.exists():
            return p
    return None


def load_fmjson(demo_path: Path, helper: Optional[Path]) -> Dict[str, Any]:
    cache = Path(str(demo_path) + ".fmjson")
    if cache.exists():
        return json.loads(cache.read_text(encoding="utf-8"))
    if helper and helper.exists():
        out = subprocess.check_output([str(helper), str(demo_path)], text=True, errors="replace")
        cache.write_text(out, encoding="utf-8")
        return json.loads(out)
    raise FileNotFoundError(f"No fmjson cache for {demo_path} and no FilmmakerDemoInfo helper")


def player_lookup(fm: Dict[str, Any]) -> Dict[int, Dict[str, Any]]:
    out: Dict[int, Dict[str, Any]] = {}
    for p in fm.get("players", []):
        aid = int(p.get("accountId", 0))
        if aid:
            out[aid] = p
    return out


def build_probe_ticks(fm: Dict[str, Any], max_ticks: int = 24) -> List[int]:
    """Prefer gun pickup events; avoid flooding with uniform spacing."""
    db = get_database()

    pickup_ticks: List[int] = []
    for ev in fm.get("events", []):
        if ev.get("type") != "item_pickup":
            continue
        item = (ev.get("item") or "").strip()
        def_idx = db.def_for_item(item)
        if not def_idx or not is_verifiable_gun_def(def_idx):
            continue
        tick = int(ev.get("tick", 0))
        if tick > 0:
            pickup_ticks.append(tick + 16)  # a few ticks after pickup — gun usually out

    pickup_ticks = sorted(set(pickup_ticks))

    # Dedupe ticks within 2500 of each other (one seek covers nearby action).
    merged: List[int] = []
    for t in pickup_ticks:
        if merged and (t - merged[-1]) < 2500:
            continue
        merged.append(t)

    # Sparse fallbacks if demo has few pickup events.
    if len(merged) < 3:
        duration = int(fm.get("durationSeconds", 0) or 0)
        est_end = max(32000, duration * 64)
        for t in (4000, 12000, 24000, est_end // 2):
            if t > 0 and (not merged or min(abs(t - m) for m in merged) >= 2500):
                merged.append(t)
        merged.sort()

    if len(merged) > max_ticks:
        step = max(1, len(merged) // max_ticks)
        merged = merged[::step][:max_ticks]
    return merged[:max_ticks] if merged else [4000, 8000, 12000][:max_ticks]


def account_to_steam64(account_id: int) -> int:
    return int(account_id) + STEAM64_OFFSET


def is_alive_at_tick(account_id: int, tick: int, events: List[Dict[str, Any]]) -> bool:
    """False when fmjson shows the player died this round before `tick` (weapon_drop, no respawn)."""
    if account_id <= 0 or tick <= 0:
        return False
    last_death = -1
    respawn_pickup = -1
    for ev in sorted(events, key=lambda e: int(e.get("tick", 0))):
        if int(ev.get("accountId", 0)) != account_id:
            continue
        t = int(ev.get("tick", 0))
        if t > tick:
            break
        et = ev.get("type", "")
        if et == "weapon_drop":
            last_death = t
            respawn_pickup = -1
        elif et == "item_pickup" and last_death >= 0 and t > last_death:
            respawn_pickup = t
    if last_death < 0:
        return True
    if respawn_pickup > last_death:
        return True
    return False


def build_scan_targets(
    fm: Dict[str, Any],
    db,
    probe_ticks: List[int],
    max_per_tick: int = 8,
) -> List[Dict[str, Any]]:
    """Known players per probe tick from fmjson gun pickups (no spec_next needed)."""
    players = player_lookup(fm)
    pickups: List[Dict[str, Any]] = []
    for ev in fm.get("events", []):
        if ev.get("type") != "item_pickup":
            continue
        tick = int(ev.get("tick", 0))
        aid = int(ev.get("accountId", 0))
        item = (ev.get("item") or "").strip()
        if tick <= 0 or aid <= 0 or not item:
            continue
        def_idx = db.def_for_item(item)
        if not def_idx or not is_verifiable_gun_def(def_idx):
            continue
        pinfo = players.get(aid, {})
        pickups.append({
            "pickupTick": tick,
            "accountId": aid,
            "steamId": account_to_steam64(aid),
            "playerName": pinfo.get("name", ""),
            "weaponClass": item,
            "weaponDefIndex": def_idx,
        })

    events = fm.get("events", [])
    targets: List[Dict[str, Any]] = []
    seen: set = set()
    for pt in probe_ticks:
        cands = [p for p in pickups if (pt - 384) <= p["pickupTick"] <= (pt + 64)]
        cands.sort(key=lambda p: (abs(p["pickupTick"] - pt), -p["pickupTick"]))
        added = 0
        for c in cands:
            key = (pt, c["accountId"])
            if key in seen:
                continue
            seen.add(key)
            alive = is_alive_at_tick(int(c["accountId"]), pt, events)
            targets.append({
                "probeTick": pt,
                "seekTick": pt,
                "accountId": c["accountId"],
                "steamId": str(c["steamId"]),
                "playerName": c["playerName"],
                "weaponClass": c["weaponClass"],
                "expectedDefIndex": c["weaponDefIndex"],
                "aliveAtProbe": alive,
                "source": "fmjson_gun_pickup",
            })
            if alive:
                added += 1
            if added >= max_per_tick:
                break
    return targets


def build_anchors(fm: Dict[str, Any], db) -> List[Dict[str, Any]]:
    players = player_lookup(fm)
    anchors: List[Dict[str, Any]] = []
    for ev in fm.get("events", []):
        et = ev.get("type", "")
        tick = int(ev.get("tick", 0))
        aid = int(ev.get("accountId", 0))
        item = (ev.get("item") or "").strip()
        if tick <= 0 or aid <= 0:
            continue
        pinfo = players.get(aid, {})
        def_idx = db.def_for_item(item) if item else None
        anchors.append({
            "tick": tick,
            "eventType": et,
            "accountId": aid,
            "steamId": str(account_to_steam64(aid)),
            "playerName": pinfo.get("name", ""),
            "team": pinfo.get("team"),
            "weaponClass": item or None,
            "weaponDefIndex": def_idx,
            "weaponCategory": slot_for_def(def_idx) if def_idx else None,
            "source": "fmjson_event",
        })
    return anchors


def main() -> int:
    ap = argparse.ArgumentParser(description="Offline demo probe plan for weapon moment scanner")
    ap.add_argument("demo", type=Path, help="Path to .dem file")
    ap.add_argument("--out", type=Path, default=None, help="Output JSON plan path")
    ap.add_argument("--helper", type=Path, default=None, help="FilmmakerDemoInfo.exe")
    ap.add_argument("--max-probe-ticks", type=int, default=20)
    args = ap.parse_args()

    demo = args.demo.resolve()
    if not demo.exists():
        print(f"Demo not found: {demo}", file=sys.stderr)
        return 1

    helper = args.helper or find_demo_helper()
    db = get_database()
    fm = load_fmjson(demo, helper)
    if not fm.get("ok"):
        print("fmjson parse failed", file=sys.stderr)
        return 1

    demo_name = demo.stem
    rel_demo = f"replays/{demo_name}"
    probe_ticks = build_probe_ticks(fm, args.max_probe_ticks)
    plan = {
        "schemaVersion": 1,
        "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "demo": rel_demo,
        "demoPath": str(demo),
        "demoName": demo_name,
        "map": fm.get("map"),
        "durationSeconds": fm.get("durationSeconds"),
        "rounds": fm.get("rounds"),
        "players": fm.get("players", []),
        "probeTicks": probe_ticks,
        "scanTargets": build_scan_targets(fm, db, probe_ticks, max_per_tick=8),
        "anchors": build_anchors(fm, db),
        "moments": [],
        "note": "moments[] filled by scan-cosmetics-weapon-moments.ps1 live enumeration",
    }

    out = args.out
    if out is None:
        out = ROOT / "automation" / "output" / "cosmetics_weapon_moments" / demo_name / "plan.json"
    out = out.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(plan, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(f"Wrote {out}")
    print(f"  probeTicks={len(plan['probeTicks'])} scanTargets={len(plan['scanTargets'])} anchors={len(plan['anchors'])} map={plan['map']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
