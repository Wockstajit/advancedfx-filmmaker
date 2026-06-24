# Demo Event Pre-scan — `.fmjson` v5

The Follow camera's **Events** list (weapon/C4 drops + pickups) comes from a one-time demo
pre-scan, cached next to the demo as `<demo>.dem.fmjson`. CS2 demos have no random access
to game events, so the tool parses the whole demo once (in the C# helper) and reads the
cached JSON thereafter.

## Schema version

`SchemaVersion` / `kSchemaVersion` = **5**. The writer and reader must agree:

- C# writer: [Program.cs:70](../FilmmakerDemoInfo/Program.cs:70) (`const int SchemaVersion = 5;`)
- C++ reader: [DemoInfoHelper.cpp:16](../AfxHookSource2/Filmmaker/Demo/DemoInfoHelper.cpp:16) (`constexpr int kSchemaVersion = 5;`)

A cached file with a different `v` is treated as stale and re-parsed
([DemoInfoHelper.cpp:224](../AfxHookSource2/Filmmaker/Demo/DemoInfoHelper.cpp:224)). **If you
change the schema, bump both constants together.**

> v4 → v5 added the top-level `"events"` array. Existing v4 caches auto-regenerate on next
> read (no manual migration).

## Event JSON shape

The `.fmjson` gains a top-level array
([Program.cs:312](../FilmmakerDemoInfo/Program.cs:312)):

```json
"events": [
  { "tick": 1234, "type": "weapon_drop",  "item": "",          "accountId": 123456 },
  { "tick": 1250, "type": "bomb_dropped",  "item": "c4",        "accountId": 234567 },
  { "tick": 1300, "type": "bomb_pickup",   "item": "c4",        "accountId": 345678 },
  { "tick": 1400, "type": "item_pickup",   "item": "weapon_ak47","accountId": 123456 }
]
```

| Field | Meaning |
|---|---|
| `tick` | `demo.CurrentDemoTick` when the event fired — the seek target. |
| `type` | `weapon_drop` · `bomb_dropped` · `bomb_pickup` · `bomb_planted` · `item_pickup`. |
| `item` | e.g. `weapon_ak47`, `c4`, or `""`. |
| `accountId` | Steam account id (32-bit) of the player involved; `0` = unknown. |

### Event sources (C#)

Subscribed on `demo.Source1GameEvents` inside the full-parse block (skipped in
names-fast mode), [Program.cs:247–251](../FilmmakerDemoInfo/Program.cs:247):

| Recorded type | demofile-net event | Notes |
|---|---|---|
| `weapon_drop` | `PlayerDeath` | CS2 has **no** `weapon_drop` game event, so the victim's death tick is used as the dominant drop moment. |
| `item_pickup` | `ItemPickup` | Only non-`Silent` pickups. |
| `bomb_dropped` | `BombDropped` | Player via `.Player`. |
| `bomb_pickup` | `BombPickup` | `Source1BombPickupEvent` exposes no `.Player`; the controller is reached via `e.PlayerPawn?.Controller`. |
| `bomb_planted` | `BombPlanted` | — |

`AddEvent` ([Program.cs:104](../FilmmakerDemoInfo/Program.cs:104)) stamps each with
`demo.CurrentDemoTick` and the account id.

## C++ side

- `DemoEvent { int tick; std::string type; std::string item; uint32_t accountId; }`
  ([DemoEntry.h:38](../AfxHookSource2/Filmmaker/Demo/DemoEntry.h:38)); `DemoEntry` /
  `DemoHelperResult` carry a `std::vector<DemoEvent> events`.
- Parsed from the `"events"` array in
  [DemoInfoHelper.cpp:184](../AfxHookSource2/Filmmaker/Demo/DemoInfoHelper.cpp:184).
- The Follow camera loads them lazily on a background thread keyed on the engine's
  `GetDemoFilePath()`
  ([FollowCamera.cpp:1290 `EnsureEventsLoaded`](../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:1290)),
  resolving `accountId → ownerName` from the scoreboard. The state JSON ships the events
  nearest the playhead (capped at 60) so a long demo doesn't flood every frame
  ([FollowCamera.cpp:1232](../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:1232)).

## Regenerating a cache

Delete the cache file and re-open the demo (or select a Weapon target, which triggers the
background load):

```bat
del "<path-to-demo>.dem.fmjson"
```

The helper exe must be the freshly published one — it ships to
`build\staging-release\bin\x64\FilmmakerDemoInfo\FilmmakerDemoInfo.exe` via `build.bat`
(the `dotnet publish` step). A names-fast run (scoreboard names only) deliberately skips
events, so the event list requires the full parse.
