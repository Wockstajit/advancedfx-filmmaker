#!/usr/bin/env python3
"""Shared weapon skin classification for cosmetics mesh automation.

Loads docs/cs2_weapon_skins_legacy_index.csv (or .json) plus cosmetics.json for
def-index / slot / display-name mapping. Classification rule matches the game:
  legacy_model true  -> Legacy CS weapon model
  legacy_model false -> CS2 weapon model
  paint 0 / default  -> CS2 model
"""
from __future__ import annotations

import csv
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parent.parent.parent
CSV_PATH = ROOT / "docs" / "cs2_weapon_skins_legacy_index.csv"
JSON_PATH = ROOT / "docs" / "cs2_weapon_skins_legacy_index.json"
COSMETICS_JSON = ROOT / "AfxHookSource2" / "Filmmaker" / "Data" / "cosmetics.json"

# Mirrors CosmeticCatalog.cpp slot tables.
SECONDARY_DEFS = {1, 2, 3, 4, 30, 32, 36, 61, 63, 64}
PRIMARY_DEFS = {
    7, 8, 9, 10, 11, 13, 14, 16, 17, 19, 23, 24, 25, 26, 27, 28, 29, 33, 34, 35,
    38, 39, 40, 60,
}
KNIFE_DEFS = {
    42, 59, 500, 503, 505, 506, 507, 508, 509, 512, 514, 515, 516, 517, 518, 519,
    520, 521, 522, 523, 525, 526,
}
GLOVE_DEF_MIN, GLOVE_DEF_MAX = 5027, 5040


@dataclass(frozen=True)
class SkinRecord:
    weapon_name: str
    weapon_class: str
    skin_name: str
    paint_kit: str
    paint_kit_id: int
    legacy_model: bool

    @property
    def full_skin_name(self) -> str:
        return f"{self.weapon_name} | {self.skin_name}"

    @property
    def expected_model_type(self) -> str:
        return "legacy" if self.legacy_model else "cs2"


def slot_for_def(def_index: int) -> str:
    if GLOVE_DEF_MIN <= def_index <= GLOVE_DEF_MAX:
        return "gloves"
    if def_index in KNIFE_DEFS:
        return "knife"
    if def_index in SECONDARY_DEFS:
        return "secondary"
    if def_index in PRIMARY_DEFS:
        return "primary"
    if def_index == 43:
        return "grenade"
    if def_index == 49:
        return "c4"
    return "default"


def is_verifiable_gun_def(def_index: int) -> bool:
    """Primary or secondary only — what must be in-hand for mesh verification."""
    return def_index in PRIMARY_DEFS or def_index in SECONDARY_DEFS


def skip_reason_for_def(def_index: int) -> Optional[str]:
    """Why an active weapon should be skipped during scanning."""
    if def_index <= 0:
        return "no active weapon"
    if def_index in KNIFE_DEFS:
        return "knife out"
    if def_index == 43:
        return "grenade out"
    if def_index == 49:
        return "c4 out"
    if def_index == 31:
        return "zeus out"
    if def_index in (44, 45, 46, 47, 48):
        return "grenade out"
    if GLOVE_DEF_MIN <= def_index <= GLOVE_DEF_MAX:
        return "gloves"
    if not is_verifiable_gun_def(def_index):
        return "not a primary/secondary gun"
    return None


def classify_paint(paint_id: int, db: "WeaponSkinDatabase") -> Tuple[str, bool, bool, bool]:
    """Return (expected_model_type, legacy, cs2, is_default)."""
    if paint_id <= 0:
        return "cs2", False, True, True
    rec = db.by_paint_id.get(paint_id)
    if not rec:
        return "cs2", False, True, False
    if rec.legacy_model:
        return "legacy", True, False, False
    return "cs2", False, True, False


class WeaponSkinDatabase:
    def __init__(self) -> None:
        self.by_paint_id: Dict[int, SkinRecord] = {}
        self.by_weapon_class: Dict[str, List[SkinRecord]] = {}
        self.by_weapon_name: Dict[str, List[SkinRecord]] = {}
        self.def_by_class: Dict[str, int] = {}
        self.def_by_weapon_name: Dict[str, int] = {}
        self.weapon_display: Dict[int, str] = {}
        self._load_skins()
        self._load_cosmetics()

    def _load_skins(self) -> None:
        if CSV_PATH.exists():
            with CSV_PATH.open(encoding="utf-8-sig", newline="") as f:
                for row in csv.DictReader(f):
                    rec = SkinRecord(
                        weapon_name=row["weapon"].strip(),
                        weapon_class=row["weapon_class"].strip(),
                        skin_name=row["skin_name"].strip(),
                        paint_kit=row["paint_kit"].strip(),
                        paint_kit_id=int(row["paint_kit_id"]),
                        legacy_model=row["classification"].strip().lower() == "legacy",
                    )
                    self._index_skin(rec)
            return
        if not JSON_PATH.exists():
            raise FileNotFoundError(f"Missing skin index: {CSV_PATH} or {JSON_PATH}")
        doc = json.loads(JSON_PATH.read_text(encoding="utf-8"))
        for weapon_name, skins in doc.get("weapons", {}).items():
            for s in skins:
                wc = self._guess_weapon_class(weapon_name)
                rec = SkinRecord(
                    weapon_name=weapon_name,
                    weapon_class=wc,
                    skin_name=s["skin_name"],
                    paint_kit=s["paint_kit"],
                    paint_kit_id=int(s["paint_kit_id"]),
                    legacy_model=s["classification"].lower() == "legacy",
                )
                self._index_skin(rec)

    @staticmethod
    def _guess_weapon_class(weapon_name: str) -> str:
        slug = re.sub(r"[^a-z0-9]+", "_", weapon_name.lower()).strip("_")
        return f"weapon_{slug}"

    def _index_skin(self, rec: SkinRecord) -> None:
        self.by_paint_id[rec.paint_kit_id] = rec
        self.by_weapon_class.setdefault(rec.weapon_class, []).append(rec)
        self.by_weapon_name.setdefault(rec.weapon_name, []).append(rec)

    def _load_cosmetics(self) -> None:
        if not COSMETICS_JSON.exists():
            return
        doc = json.loads(COSMETICS_JSON.read_text(encoding="utf-8"))
        for w in doc.get("weapons", []):
            def_idx = int(w["defIndex"])
            name = w["name"]
            self.def_by_weapon_name[name] = def_idx
            self.weapon_display[def_idx] = name
            slug = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
            self.def_by_class[f"weapon_{slug}"] = def_idx
        for k in doc.get("knives", []):
            def_idx = int(k["defIndex"])
            self.weapon_display[def_idx] = k["name"]
        # CSV weapon_class is authoritative where present.
        for wc, skins in self.by_weapon_class.items():
            if skins:
                wn = skins[0].weapon_name
                if wn in self.def_by_weapon_name:
                    self.def_by_class[wc] = self.def_by_weapon_name[wn]

    def def_for_class(self, weapon_class: str) -> Optional[int]:
        if not weapon_class:
            return None
        wc = weapon_class if weapon_class.startswith("weapon_") else f"weapon_{weapon_class}"
        return self.def_by_class.get(wc)

    def def_for_item(self, item: str) -> Optional[int]:
        """Map fmjson item_pickup labels (USP-S, AK-47) or weapon_class strings to def index."""
        if not item:
            return None
        label = item.strip()
        if label in self.def_by_weapon_name:
            return self.def_by_weapon_name[label]
        if label.startswith("weapon_"):
            return self.def_for_class(label)
        slug = re.sub(r"[^a-z0-9]+", "_", label.lower()).strip("_")
        return self.def_by_class.get(f"weapon_{slug}")

    def weapon_name_for_def(self, def_index: int) -> str:
        return self.weapon_display.get(def_index, f"def_{def_index}")

    def skin_label(self, def_index: int, paint_id: int) -> str:
        if paint_id <= 0:
            return f"{self.weapon_name_for_def(def_index)} (default)"
        rec = self.by_paint_id.get(paint_id)
        if rec:
            return rec.full_skin_name
        return f"{self.weapon_name_for_def(def_index)} | paint {paint_id}"

    def pick_test_paints(self, def_index: int) -> Optional[Dict[str, object]]:
        """Legacy + modern paints for override tests on this weapon def."""
        weapon_name = self.weapon_name_for_def(def_index)
        skins = self.by_weapon_name.get(weapon_name, [])
        if not skins:
            # Try match any skin whose weapon maps to this def via class.
            for wc, did in self.def_by_class.items():
                if did == def_index:
                    skins = self.by_weapon_class.get(wc, [])
                    break
        legacy = next((s for s in skins if s.legacy_model), None)
        modern = next((s for s in skins if not s.legacy_model), None)
        if not legacy or not modern:
            return None
        return {
            "legacyPaint": {"id": legacy.paint_kit_id, "name": legacy.full_skin_name, "useLegacyModel": True},
            "modernPaint": {"id": modern.paint_kit_id, "name": modern.full_skin_name, "useLegacyModel": False},
            "meshTogglePaint": legacy.paint_kit_id,
        }

    def has_both_model_types(self, weapon_key: str) -> bool:
        skins = self.by_weapon_class.get(weapon_key) or self.by_weapon_name.get(weapon_key, [])
        has_l = any(s.legacy_model for s in skins)
        has_m = any(not s.legacy_model for s in skins)
        return has_l and has_m


_db: Optional[WeaponSkinDatabase] = None


def get_database() -> WeaponSkinDatabase:
    global _db
    if _db is None:
        _db = WeaponSkinDatabase()
    return _db


if __name__ == "__main__":
    db = get_database()
    print(f"paints={len(db.by_paint_id)} weapons={len(db.by_weapon_name)} defs={len(db.weapon_display)}")
