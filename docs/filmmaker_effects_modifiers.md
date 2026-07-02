# Effects modifiers (Config panel EFFECTS section)

Runtime particle-effect control for demo playback: per-category **On / More / Less / Off**
over the demo's visual effects, toggled live from the Config panel
(`mirv_filmmaker config`) or the console (`mirv_filmmaker fx ...`), persisted to
`%APPDATA%\HLAE\filmmaker_fx.json`.

This is the CS2 replacement for CS:GO-era Better Particles file mods. The reference copies
live in `panorama ref/csgo effect mod/`. The runtime does not invent fake sprites or swap
grenades to unrelated stock CS2 particles. **On**, **More**, and **Less** point at Source 2
resources converted from the real Source 1 `.pcf` assets with `kristiker/source1import`.

## Asset conversion

Everything the converter needs is repo-local, so a plain run converts with no
machine-specific setup:

- `misc/source1import/` — clone of <https://github.com/kristiker/source1import> (gitignored;
  re-clone if missing).
- `misc/Source2Converter/` — clone of <https://github.com/REDxEYE/Source2Converter> with its
  `SourceIO` submodule initialized (gitignored; re-clone if missing).
- `panorama ref/csgo effect mod/legacy_csgo_deps/` — the two stock CS:GO models the mod's
  particles reference (`models/gibs/wood_gib01b`, `models/props_debris/concrete_chunk07a`,
  each as `.mdl/.vvd/.dx90.vtx/.phy`) plus their VMT/VTF materials, extracted once from a
  legacy CS:GO `pak01_dir.vpk`. Bundled so conversion has no dependency on a CS:GO install.
- Python deps: `pip install Pillow numpy vdf dataclassy==0.10.4 parsimonious==0.10.0
  srctools==2.3.4 vtf2img vpk`.

Convert the referenced Source 1 assets before expecting the modes to look like the old mod:

```powershell
powershell -File automation/tools/convert-better-particles-source1.ps1 -Compile
```

(`-Source1ImportDir` / `-Source2ConverterDir` / `-LegacyCsgoDir` or the matching env vars
override the repo-local defaults.)

The wrapper stages the mod PCFs under a stable namespace before running
`utils/materials_import.py -b cs2` and `utils/particles_import.py -b cs2`, exports the mod
VTF textures (including sprite-sheet reconstruction: embedded `.sht` data becomes per-frame
slices + a `.mks` + a `.vtex` input, without which every atlas draws as a frame GRID),
post-processes the converted VPCFs (`postprocess-better-particles.py`), converts the
bundled MDLs with Source2Converter, compiles everything with CS2's `resourcecompiler.exe`,
and finally validates the full runtime closure (`validate-better-particles-assets.py`).

The post-process pass exists because CS2 renders these conversions differently than
Source 1 did; it never invents placeholder assets, it only repairs/tones the real ones:

- FP muzzle-flash closure: overbright/alpha/radius tone-down (Source 2 lighting blows
  out the Source 1 values).
- Molotov roots: incompatible cinematic children removed, fire toned.
- Dead references (children/fallbacks the mod itself never shipped) stripped so CS2
  stops logging `Failed loading resource` at demo load.
- Sprite renderers without an `m_hTexture` draw SOLID WHITE QUADS (CS2 ignores the
  legacy `m_hMaterial`): the original texture is injected from the VMT's `$basetexture`
  when it exists, otherwise the renderer (Source 1 heat-distortion/warp quads, screen
  overlays) is removed.
- `$DUALSEQUENCE` combine keys are stripped: the rebuilt sheets are single-mode, and the
  dual-sample modes produce background-dependent dark fringes ("black ring" smoke).

A pack that compiles particles but not materials/textures/models renders as white/error
quads in game -- the validator's job is to catch exactly that.

```text
particles/filmmaker/betterparticles/classic/...
particles/filmmaker/betterparticles/classic_updated/...
particles/filmmaker/betterparticles/less_impacts/...
particles/filmmaker/betterparticles/less_smoke/...
```

Those namespaces are what `ParticleFx.cpp` uses in its variant tables. The launcher mounts
the compiled output automatically via `USRLOCALCSGO` when
`automation/output/effects/betterparticles-source1import/source2/game/source_mvm_fx`
exists. If the converted pack is not compiled and mounted into CS2's resource search path,
the swap fails open and the original CS2 effect plays. This is intentional: missing assets
must not fall back to random placeholders.

## Categories

| Key | UI label | Matches (path prefix on the vpcf resource name) |
|---|---|---|
| `impacts` | Bullet Impacts | `particles/impact_fx/`, `particles/water_impact/`, `particles/breakable_fx/` |
| `tracers` | Bullet Tracers | weapon-fx paths whose name contains `tracer` |
| `weaponfx` | Muzzle Flash & Shells | `particles/weapons/cs_weapon_fx/`, `particles/unified_weapon_fx/` |
| `blood` | Blood | any path containing `blood` (blood_impact/, impact_fx/, screen splatter) |
| `explosions` | Explosions (HE / C4) | `particles/explosions_fx/`, `particles/entity/env_explosion/` |
| `molotov` | Molotov Fire | `particles/burning_fx/`, `particles/inferno_fx*` |
| `mapfx` | Map Ambience | `particles/maps/`, `particles/ambient_fx/`, `particles/environment/`, `particles/rain_fx/`, `particles/critters/` |

`particles/ui/` (HUD/MVP effects) is always excluded. CS2 smoke grenades are deliberately
not touched because the gameplay smoke is a volumetric system, not a normal particle swap.

Modes:

- **On**: converted `p_betterparticlesmod_classic_c057b` assets.
- **More**: converted `p_betterparticlesmod_classic updated_c057b` assets.
- **Less**: combined converted less variants. Bullet impact systems use
  `p_betterparticlesmod_lessimpacts`; smoke/muzzle/blood/fire/explosion systems use
  `p_betterparticlesmod_lesssmoke_22ac2`.
- **Off**: default CS2 pass-through for that category.

Blood maps to the mod's cinematic blood impact/headshot systems, so spray, flow, smoke, and
air-trail children come from the asset pack. HE maps to the mod grenade explosion, not a C4
explosion. Muzzle/tracer/molotov/environmental impact mappings likewise target converted
mod assets.

**Money on Headshot** (`mirv_filmmaker fx moneyshot on|off`) is event-gated. It only arms
after a confirmed `player_hurt` headshot/hitgroup-1 event or a `player_death` event with
`headshot=true`, then consumes a small number of matching headshot impact particle
creations inside a short time window. It targets the converted mod asset
`particles/filmmaker/betterparticles/classic/impact_fxmoney/impact_helmet_headshot.vpcf`;
it no longer triggers from unrelated particle creation and it does not use CS2's stock
moneycrate burst.

**Taser/Zeus is untouched by the variant tables**: its `weapon_tracers_taser*` wire
systems are not mapped, so they play vanilla.

**Variant tables must map top-level systems only.** Child systems are created internally by
the engine and bypass the hook. Verify what a demo actually creates with
`mirv_filmmaker fx names <filter>` and then add mappings only for those top-level names.

## How it works

One Detours hook on `particles.dll`'s `CParticleSystemMgr` create-collection body catches
particle instantiation:

1. `CreateInterface("ParticleSystemMgr003")` resolves the particle manager.
2. Slot 15 (`FindParticleSystem`) stays callable for resolving swap target names.
3. Slot 17's shared create body is detoured, including the internal direct-call path.
4. The hook reads the resource name off the handle, classifies it, and either passes it
   through or swaps it to a resolved target. Per-category Off is pass-through; explicit
   custom `fx block` rules still swap to `particles/dev/empty.vpcf`.

Swap targets are resolved on the main thread only. Resolving inside the create hook can
re-enter the resource system during particle creation, so the hook is cache-hit-only: an
unresolved target fails open once and gets queued for later resolution.

Non-precached targets are loaded through the engine's just-in-time manifest path instead
of the plain single-resource blocking load. That keeps dependency handles fixed up and
avoids the crash path seen when a previously unseen target was loaded mid-create.

Toggles apply to the current paused moment automatically. Settings changes request a
debounced one-tick-backward `demo_gototick`, which destroys live particles and replays the
recent event stream under the new rules. While the demo is playing, the automatic reseek is
skipped to avoid hitching; `mirv_filmmaker fx apply` forces it manually.

Limits: surface decals are not particles and never change; effects older than the last
demo full-packet are not replayed; long-lived ambient systems are usually created once at
map/demo load, so `mapfx` changes may need a demo reload.

## Console reference

```text
mirv_filmmaker fx set <category> <on|more|less|off>  per-category control
mirv_filmmaker fx on|off                             master switch
mirv_filmmaker fx state                              status + counters
mirv_filmmaker fx log on|off                         capture every creation
mirv_filmmaker fx recent [n]                         print last n captured/acted creations
mirv_filmmaker fx names [filter]                     aggregated per-name creation counts
mirv_filmmaker fx block <substr>                     custom rule: block names containing substr
mirv_filmmaker fx swap <substr> <target.vpcf>        custom rule: swap matching names
mirv_filmmaker fx unblock|unswap <substr>            remove custom rule(s)
mirv_filmmaker fx rules                              list custom rules
mirv_filmmaker fx test <name>                        dry-run the decision for one name
mirv_filmmaker fx apply                              re-create the current moment's live effects now
mirv_filmmaker fx moneyshot on|off                   event-gated money effect on headshot hits
```

Tuning workflow: `fx log on`, play the moment, inspect `fx names`, then adjust the variant
tables in `ParticleFx.cpp`. Runtime events also mirror into the `mvm_debug` log
(`fx.create`, `fx.install`, `fx.event`, `state.fx`).
