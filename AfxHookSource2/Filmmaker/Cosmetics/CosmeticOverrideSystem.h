#pragma once

// Offline demo-only cosmetic OVERRIDE SYSTEM.
//
// Once per MAIN-thread frame (Filmmaker::RunMainThreadFrame -> Cosmetics_RunFrame), this walks the
// client entity list, resolves the ORIGINAL OWNER of every econ entity by XUID, and -- if that
// SteamID has a profile -- rewrites the entity's fallback paint-kit/wear/seed/StatTrak so the
// client composites the chosen finish (the nSkinz/Osiris mechanism: C_EconItemView::m_iItemIDHigh
// = -1 makes the client ignore the networked item and use C_EconEntity::m_nFallback* instead).
//
// Keying on owner XUID (not pawn/entity index) is what makes overrides survive weapon switches,
// player/observer switches, round restarts, death/respawn, demo seeks (forward AND backward) and
// entity recreation: every frame we re-discover the entities and re-assert the override. Dropped
// weapons keep their OriginalOwnerXuid, so a dropped gun owned by a profiled player still shows the
// override.
//
// GATING: all application is hard-gated on (a) a demo actually playing (InDemoContext) and (b) the
// econ offsets having resolved (OffsetsAvailable / g_cosmeticsOffsetsOk). A missing offset disables
// cosmetics only -- it never aborts the tool. This is for OFFLINE demo/movie work; never live play.

#include <cstdint>
#include "CosmeticProfile.h"

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Per-frame application stats, surfaced by "cosmetics status" / debug overlay.
struct CosmeticFrameStats {
	int entitiesScanned = 0;   // econ entities walked this frame
	int entitiesMatched = 0;   // owned by a profiled SteamID
	int entitiesPatched = 0;   // had an override written this frame
	int entitiesReverted = 0;  // the engine had clobbered our write and we re-applied it
	int attrListsRead = 0;     // networked dynamic attribute vectors successfully read
	int attrValuesWritten = 0; // def 6/7/8/81 values found and overwritten
	int attrValuesChanged = 0; // values that differed from the requested override
	int attrListsEmpty = 0;    // valid vectors with no matching writable skin attrs
	uint64_t frame = 0;        // monotonically increasing apply-frame counter
	int paintkitBridgeValue = 0; // current cl_paintkit_override bridge value, 0 = inactive/default
};

// Which speculative "visuals are stale" field writes the rebuild step is allowed to perform. These
// were added experimentally to try to force a demo weapon to re-composite its skin; live testing
// (2026-06-28) showed NONE of them trigger the 3D rebuild AND at least one (clearUgc / initialized)
// makes the weapon VANISH from the HUD weapon-switch bar by blanking its econ identity. So every flag
// now DEFAULTS OFF -- normal apply writes only the real skin data (networked attrs + fallback fields),
// which is HUD-safe -- and each is independently toggleable ("cosmetics rebuildflags <name> 0|1") so
// the harmful-vs-useful one can be bisected without writing all of them at once. POD (all bool), so it
// is safe to pass by value into the SEH-guarded write. See docs/cosmetics-recompose-research.md.
struct CosmeticRebuildFlags {
	bool visualsDataSet = false; // C_CSWeaponBase::m_bVisualsDataSet = false (most plausible gate)
	bool clearUgc = false;       // C_CSWeaponBase::m_bClearWeaponIdentifyingUGC = true (HUD-vanish suspect)
	bool reloadEvent = false;    // C_CSWeaponBase::m_nCustomEconReloadEventId = -1 (networked reload token)
	bool initialized = false;    // C_EconItemView::m_bInitialized/m_bInitializedTags = false (desc cache)
	bool attrInit = false;       // C_EconEntity::m_bAttributesInitialized = false (entity attr re-init)
	bool imageCache = false;     // m_bRestoreCustomMaterialAfterPrecache + inventory-image cache hints
	bool attrParity = false;     // CAttributeManager::m_iReapplyProvisionParity ++ (attr cache bump)
};

class CosmeticOverrideSystem {
public:
	// Loads persisted profiles from %APPDATA%\HLAE\cosmetic_profiles.json. Safe to call repeatedly.
	void Init();
	void Shutdown();

	// Apply loop. Call once per MAIN/UI-thread frame. Cheap no-op when disabled, no profiles, no
	// demo playing, or offsets unresolved.
	void RunFrame();

	// --- global state ---
	void SetEnabled(bool e);
	bool Enabled() const { return m_store.Enabled(); }
	void SetDebug(bool e) { m_debug = e; }
	bool Debug() const { return m_debug; }
	bool OffsetsAvailable() const;   // g_cosmeticsOffsetsOk
	bool InDemoContext() const;      // true only while a demo is actively playing

	// --- profile editing (keyed by SteamID64) ---
	// defIndex 0 on SetWeapon = "keep the held weapon's own def, just restyle it"; the slot
	// (primary/secondary) is then resolved from the live weapon at apply time. A non-zero defIndex
	// classifies into primary/secondary via the catalog.
	void SetWeapon(uint64_t steamId, int defIndex, int paintKit, float wear, int seed, int statTrak);
	void SetKnife(uint64_t steamId, int defIndex, int paintKit, float wear, int seed);
	void SetGloves(uint64_t steamId, int defIndex, int paintKit, float wear, int seed);
	void SetAgent(uint64_t steamId, const char* model, int defIndex);
	void ClearPlayer(uint64_t steamId);
	void ClearAll();
	void Save();  // persist profiles + enabled flag

	CosmeticProfileStore& Store() { return m_store; }
	const CosmeticProfileStore& Store() const { return m_store; }
	const CosmeticFrameStats& LastFrameStats() const { return m_lastStats; }

	// --- spectate resolution (for "target current" and debug) ---
	// SteamID64 of the player the local viewer is currently spectating, or 0 if none/not in-eye.
	uint64_t CurrentSpectatedSteamId() const;
	// Entity index of the spectated pawn, or -1.
	int CurrentSpectatedPawnIndex() const;
	// Local observer mode (OBS_MODE_*). 0 = none.
	uint8_t CurrentObserverMode() const;
	// Best-effort current display name for a SteamID (empty if no live controller). Used to stamp
	// the profile's `name` field when an override is set.
	std::string NameForSteamId(uint64_t steamId) const;

	// --- forced material re-composite (refresh path; research/tuning) ---
	// Writing the fallback fields + invalidating the C_EconItemView init flags is sometimes NOT
	// enough to make a demo entity VISUALLY re-composite its skin -- a CBasePlayerWeapon
	// UpdateComposite / UpdateCompositeSec vtable call may be required after the write. The correct
	// CS2 vtable index is build-specific and currently unconfirmed (the historical index 7 faulted on
	// recent builds), so this is OFF by default and the indices are live-tunable via
	// "cosmetics recompose 0|1" / "cosmetics vtidx <comp> <sec>". Every call is SEH-guarded, so a
	// wrong index auto-disables recompose instead of crashing the game.
	void SetRecompose(bool e);
	bool Recompose() const { return m_recompose; }
	bool RecomposeFaulted() const { return m_recomposeFaulted; }
	void GetVtIdx(int* comp, int* sec) const;
	void SetVtIdx(int comp, int sec);
	// Argument passed to the SEH-guarded vtable call (default 1). Set to 0 to test
	// OnDataChanged(DATA_UPDATE_CREATED) -- the candidate skin-rebuild trigger. "cosmetics vtarg".
	void SetVtArg(int a) { m_vtArg = a; }
	int VtArg() const { return m_vtArg; }

	// Opt-in legacy fallback-id mode: when ON, the apply loop also forces C_EconItemView::m_iItemIDHigh
	// = -1 (the m_nFallback* path). Default OFF -- it invalidates the item id (UI shows "no weapon") and
	// is unnecessary now that the networked dynamic attributes are overwritten directly. "cosmetics fallback".
	void SetFallbackId(bool e) { m_fallbackId = e; }
	bool FallbackId() const { return m_fallbackId; }

	// --- visual-cache rebuild (the refresh path; "cosmetics rebuild ...") ---
	// Writing the econ fields/attributes is confirmed to STICK but does not, by itself, make a demo
	// weapon re-composite its skin material (the consuming step is entity-lifecycle driven, not a
	// per-frame poll -- see docs/cosmetics-recompose-research.md). These give a manual, on-demand way
	// to (re)assert the "visuals are stale" field writes (m_bVisualsDataSet=false,
	// m_bClearWeaponIdentifyingUGC=true, bump m_nCustomEconReloadEventId, clear init flags, bump attr
	// parity) on every matched weapon entity, decoupled from the per-frame change detection, plus fire
	// the OPT-IN recompose vcall once (only if armed via "cosmetics recompose 1"). All writes are
	// SEH-guarded. Returns the number of matched weapon entities the rebuild touched.
	int RebuildOnce();
	// Per-frame auto-rebuild: when ON, the apply loop writes the ENABLED stale-mark flags (see
	// RebuildFlags) whenever a value changes; when OFF, the per-frame loop writes only the raw skin
	// values and the stale-mark flags fire only on an explicit "rebuild once". Default ON, but since
	// every rebuild flag now defaults OFF this writes nothing harmful until a flag is enabled.
	// "cosmetics rebuild auto 0|1".
	void SetRebuildAuto(bool e) { m_rebuildAuto = e; }
	bool RebuildAuto() const { return m_rebuildAuto; }

	// Which stale-mark field writes the rebuild step performs (all default OFF -> HUD-safe). Toggle a
	// single flag by name (returns false on an unknown name) or all at once, for bisecting which write
	// breaks the HUD weapon panel vs. which triggers a visual rebuild. "cosmetics rebuildflags ...".
	const CosmeticRebuildFlags& GetRebuildFlags() const { return m_rebuildFlags; }
	bool SetRebuildFlag(const char* name, bool on);
	void SetAllRebuildFlags(bool on);

	// Experimental deploy-time bridge for the one skin path CS2 is known to visually honor:
	// cl_paintkit_override is read while a weapon composite is being BUILT, not after it is cached.
	// When enabled, the main-thread pump sets that global cvar to the currently spectated profiled
	// weapon's paint kit and restores the previous value when there is no matching target or the
	// bridge is disabled. Limitations are intentional and surfaced in the command text: this is global
	// and only affects the next weapon deploy/create, so it is a proof-of-life/workaround while the
	// true per-entity composite read-site hook is still unknown.
	void SetPaintkitBridge(bool e);
	bool PaintkitBridge() const { return m_paintkitBridge; }
	void SetPaintkitBridgeForcedValue(int paintKit) { m_paintkitBridgeForcedValue = paintKit; }
	int PaintkitBridgeForcedValue() const { return m_paintkitBridgeForcedValue; }
	int PaintkitBridgeLastValue() const { return m_paintkitBridgeLastValue; }
	bool PaintkitBridgeCvarFound() const { return m_paintkitBridgeCvarFound; }

private:
	// Shared matched-weapon apply loop, used by both the per-frame pump (RunFrame) and the on-demand
	// rebuild (RebuildOnce). forceStale forces the visuals-stale field writes even when no value
	// changed; fireRebuildCall fires the SEH-guarded recompose vcall when armed. Returns matched count.
	int ApplyMatchedWeapons(bool forceStale, bool fireRebuildCall);
	void UpdatePaintkitBridge();
	int ResolveSpectatedPaintkitOverride() const;
	bool SetPaintkitBridgeCvar(int value);
	void RestorePaintkitBridgeCvar();

	CosmeticProfileStore m_store;
	bool m_initialized = false;
	bool m_debug = false;
	CosmeticFrameStats m_lastStats;

	// Forced-recompose state (see SetRecompose). Defaults match the retired MirvCosmetics path:
	// OFF, primary vtable index 7 (historical), secondary skipped (-1).
	bool m_recompose = false;
	bool m_recomposeFaulted = false;
	int m_vtComposite = 7;
	int m_vtCompositeSec = -1;
	int m_vtArg = 1;           // argument for the recompose vtable call; see SetVtArg
	bool m_fallbackId = false; // opt-in itemIDHigh=-1 path; see SetFallbackId
	bool m_rebuildAuto = true; // per-frame stale-mark write of the ENABLED flags; see SetRebuildAuto
	CosmeticRebuildFlags m_rebuildFlags; // which stale-mark writes are allowed (all default OFF)
	bool m_paintkitBridge = false;
	bool m_paintkitBridgeCvarFound = false;
	bool m_paintkitBridgeHaveOriginal = false;
	int m_paintkitBridgeOriginalValue = 0;
	int m_paintkitBridgeForcedValue = -1; // -1 = auto from spectated profiled weapon, >0 = forced global paint
	int m_paintkitBridgeLastValue = 0;
};

// Process-wide singleton (matches the ...Ref() pattern used by MovieMode/CameraPath/etc.).
CosmeticOverrideSystem& CosmeticsRef();

// Thin free function for the main-thread frame pump (keeps Filmmaker.cpp's include surface small).
void Cosmetics_RunFrame();

// Console command entry: handles "mirv_filmmaker cosmetics ...". argc/args/cmd are forwarded from
// FilmmakerCommand.cpp's dispatcher (args->ArgV(0) == "mirv_filmmaker", ArgV(2) == "cosmetics").
void Cosmetics_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Diagnostics (implemented in CosmeticDebug.cpp). Print to the console via advancedfx::Message.
void Cosmetics_PrintStatus(const char* cmd);
void Cosmetics_PrintSpectatedDebug(const char* cmd);
// Read-only visual-cache dump for the spectated weapon: the full econ identity, the fallback fields,
// the networked dynamic attribute values (def 6/7/8/81), the C_CSWeaponBase visuals-cache flags
// (m_bVisualsDataSet / m_bClearWeaponIdentifyingUGC / m_nCustomEconReloadEventId) AND their resolved
// schema offsets (for an IDA/Ghidra xref pass), the active weapon handle, and the weapon's world
// model path. Never writes. "cosmetics visualdiag".
void Cosmetics_PrintVisualDiag(const char* cmd);

} // namespace Filmmaker
