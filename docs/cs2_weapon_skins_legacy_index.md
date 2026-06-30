# CS2 Weapon Skins — Legacy vs CS2 Index

Full list of every released weapon-skin combination in CS2, sorted by weapon and
classified as **Legacy** or **CS2**.

Files:
- [`cs2_weapon_skins_legacy_index.csv`](cs2_weapon_skins_legacy_index.csv) — flat table, one row per weapon×skin.
- [`cs2_weapon_skins_legacy_index.json`](cs2_weapon_skins_legacy_index.json) — same data grouped under each weapon, with a `_meta` summary.

## What "Legacy" vs "CS2" means here

This uses the **game's own** distinction, not a third-party guess. In
`items_game.txt` each paint kit carries an optional `use_legacy_model "1"` flag:

- **Legacy** — the paint kit still renders on the old CS:GO weapon model. Valve did
  not remaster it for CS2's new model. (`use_legacy_model = 1`)
- **CS2** — the paint kit uses the modern CS2 weapon model (made for CS2, or a
  CS:GO skin that was remastered onto the new model). (no `use_legacy_model`)

This is the exact same flag the cosmetics renderer in this project reads to pick the
mesh group (`SetMeshGroupMask`) — see the `weapon-legacy-vs-cs2-mesh` notes. So a
skin marked "Legacy" here is one our tool must render on the legacy mesh, and "CS2"
on the modern mesh.

> Note: classification can surprise you (e.g. AWP Dragon Lore is **Legacy** because
> Valve kept it on the old model). That's intentional — it reflects how the skin
> actually renders in-game, which is what matters for filmmaking.

## Columns (CSV)

| column | meaning |
|---|---|
| `weapon` | display name, e.g. `AK-47` |
| `weapon_class` | engine class, e.g. `weapon_ak47` (`weapon_m4a1` = M4A4, `weapon_m4a1_silencer` = M4A1-S) |
| `skin_name` | display name, e.g. `Asiimov` |
| `paint_kit` | internal paint-kit name, e.g. `cu_ak47_asiimov` |
| `paint_kit_id` | numeric paint-kit id used by the econ system |
| `classification` | `Legacy` or `CS2` |

## Scope

Covers all paintable weapons (35 weapon classes incl. Zeus x27). Knives, gloves,
agents, stickers, and other cosmetics are out of scope. StatTrak / Souvenir are not
separate rows — those are item qualities, not different skins, so each paint+weapon
appears once.

## Regenerating

Counts (as of generation): **1404 skins — 521 CS2, 883 Legacy, across 35 weapons.**

The data is derived straight from the installed game files, so it stays current:

```bash
# 1. Extract the two source files from the CS2 VPK with Source2Viewer-CLI:
Source2Viewer-CLI.exe -i ".../csgo/pak01_dir.vpk" -f "scripts/items/items_game.txt" -o <tmp>
Source2Viewer-CLI.exe -i ".../csgo/pak01_dir.vpk" -f "resource/csgo_english.txt"   -o <tmp>

# 2. Rebuild the index:
python automation/tools/build_skins_legacy_index.py \
    <tmp>/scripts/items/items_game.txt \
    <tmp>/resource/csgo_english.txt \
    docs
```

Builder script: [`automation/tools/build_skins_legacy_index.py`](../automation/tools/build_skins_legacy_index.py).
Re-run it after a CS2 update to pick up newly added skins.
