# CS2 cosmetics forum methodology notes

Date: 2026-06-29

Scope: offline HLAE / filmmaker use only. This note intentionally avoids external/internal cheat
implementation details, anti-cheat bypasses, live-online use, offsets/signatures from forum posts,
or copied code. The useful part for this repo is the rendering methodology: what CS2 actually reads
when it builds a weapon skin composite, and which offline-safe levers are worth testing.

## Scrape status

I tried the provided UnknownCheats links through three paths:

- Browser fetch: direct `web.open` returned no readable page content.
- Search fetch: exact-title and exact-url searches returned no usable snippets or cached content.
- Local `curl.exe` with the supplied cookie file at
  `C:\Users\ayden\Downloads\www.unknowncheats.me_cookies.txt`: the sandbox could see the cookie file,
  but outbound HTTPS to `www.unknowncheats.me:443` failed with `Could not connect to server`.

Result: the forum pages were not scrapeable from this environment. The thread titles are still useful
as a topic map, but the conclusions below come from local repo research and live-verification notes
already present in this checkout, not from hidden forum post contents.

## Provided forum topic map

These are the links that should be revisited manually if network access works later:

- <https://www.unknowncheats.me/forum/counter-strike-2-a/759698-skin-changer.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/745997-cs2-internal-skin-changer-legacy-mesh-issue-weapons.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/758114-help-skin-changer-code.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/758981-skin-changer-newbie.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/759442-sig-skin-changer-cs2-external.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/757200-nerv-style-skin-changer-real-inventory-changer.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/755851-skin-changer.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/754710-weapon-hud-icon-useful-skin-changers.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/754709-skin-changer-rarity.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/751925-skin-changer-doesnt-online-mode.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/751532-advice-skin-changer.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/750524-skin-changer.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/750264-skin-changer-internal.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/749897-cs2-external-skin-changer.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/749893-cs2-external-skin-changer.html>
- <https://www.unknowncheats.me/forum/counter-strike-2-a/>

Expected useful signals by title:

- "legacy mesh issue": likely confirms that changing weapon identity/model and changing paint are
  separate problems. This matches `docs/cosmetics-model-override-research.md`: knife/weapon model
  changes require model/resource handling, not just paint fields.
- "weapon HUD icon" and "rarity": likely concern econ identity/UI metadata. Useful later for polish,
  but not the material-render blocker.
- "external", "internal", and "sig" threads: likely contain operational cheat mechanics. Do not port
  those. The offline-safe lesson is only the research style: locate the build-time consumer, verify in
  a local demo, and prefer engine-owned render paths over late field writes.
- "doesn't online mode": not relevant for HLAE offline work. This repo should stay demo/movie-making
  scoped.
- "real inventory changer": likely reinforces that CS2 distinguishes an authoritative econ item from
  locally edited attributes. That distinction is exactly the root cause found in this repo.

## Repo changes from the last two months

Relevant git history since 2026-04-29:

- `c049662f` / 2026-06-28: `Write cosmetics via networked attributes`
- `5036e3c8` / 2026-06-27: `Refactor cosmetics into dedicated module`
- `73f31ec7` / 2026-06-27: `Add cam editor cosmetics customizer and tidy repo layout`
- `da900850` / 2026-05-19: `fix: adjust to CS2 update (1.41.6.2)`

Current uncommitted cosmetics work is concentrated in:

- `AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp`
- `AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.h`
- `AfxHookSource2/Filmmaker/Cosmetics/CosmeticCommands.cpp`
- `AfxHookSource2/Filmmaker/Cosmetics/CosmeticDebug.cpp`
- `docs/cosmetics-recompose-research.md`

Current untracked verification helpers:

- `automation/tools/cosmetics_static_probe.py`
- `automation/tools/image_diff.py`
- `automation/verify/verify-cosmetics-paintkit-bridge.ps1`
- `automation/verify/verify-ondatachanged-bisect.ps1`

## What the local research already tried

The repo has already tested the common skin-changer field-write path:

- overwrite weapon paint/wear/seed/StatTrak in `m_NetworkedDynamicAttributes`;
- also write the fallback fields on `C_EconEntity`;
- optionally force fallback identity through `m_iItemIDHigh = -1`;
- mark visual/cache flags dirty;
- call the weapon data-changed path after setting the custom econ reload event negative;
- try material reload commands and seek/recreate flows;
- compare screenshots with `automation/tools/image_diff.py`.

Observed result: the data path works, but the render path does not. The HUD/name/econ state can
change, but the already-built 3D weapon composite stays on the original demo skin.

The strongest local conclusion is in `docs/cosmetics-recompose-research.md`: CS2 builds the rendered
skin composite at weapon create/deploy time from the authoritative econ item, then caches the material
on the renderable. Late local edits to networked attributes or fallback fields do not invalidate that
cached composite.

## What is actually promising

The only path verified to affect the rendered weapon material is CS2's own dev lever,
`cl_paintkit_override`, and only when it is set before the weapon composite is built. The current repo
therefore added:

- `mirv_filmmaker cosmetics paintkitbridge [0|1|auto|force <paint>]`
- `automation/verify/verify-cosmetics-paintkit-bridge.ps1`

This is not a complete per-player fix. It is global and deploy-time only. But it proves the target:
the skin must be selected inside the composite-build path, not after the fact.

Recommended direction for offline HLAE:

1. Keep the current `paintkitbridge` as a proof-of-life and testing tool.
2. Stop spending time on late write-then-refresh variants unless they identify a new composite-cache
   invalidation call. The local tests already ruled out the obvious ones.
3. Locate the paint-kit read site in the weapon composite builder, especially the site that consults
   `cl_paintkit_override`.
4. Hook that offline-only read path so it returns the selected per-player paint kit while the engine
   is naturally building the weapon composite.
5. Apply/refresh by forcing a demo seek, weapon redeploy, round transition, or other natural rebuild
   event after the per-player paint decision is available.

## Offline test plan

Use this order to avoid chasing UI-only success:

1. Build with the current cosmetics WIP.
2. Load a demo through the existing netcon automation.
3. Run `mirv_filmmaker cosmetics visualdiag` on the spectated weapon and save the output.
4. Use `mirv_filmmaker cosmetics paintkitbridge force <paint>` before a seek/redeploy.
5. Capture baseline and bridge screenshots with
   `automation/verify/verify-cosmetics-paintkit-bridge.ps1`.
6. Confirm the diff is in the weapon material region, not only HUD text.
7. Only after that, replace the global cvar bridge with a per-player build-time read hook.

Avoid using forum external/internal code as-is. For this project, the acceptable transfer is the
methodology: identify which engine-owned path produces the renderable, then feed that path before it
caches the composite.

## Bottom line

The thing to find is not another fallback-field write. The thing to find is the build-time paint-kit
consumer. UnknownCheats threads may have recent clues about signatures or inventory-style approaches,
but the repo's own experiments already show the core rule for offline filmmaking: write before
composite build, or hook the build's paint decision. Late writes can make diagnostics and HUD state
look correct while the rendered weapon remains unchanged.
