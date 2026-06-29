#include "CosmeticOverrideSystem.h"

#include "CosmeticCatalog.h"

#include "../Demo/PlayingDemoPath.h"

#include "../../ClientEntitySystem.h" // CEntityInstance, entity-list globals, CBaseHandle, AfxGetLocalObserverState
#include "../../SchemaSystem.h"        // g_clientDllOffsets, g_cosmeticsOffsetsOk

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/icvar.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Filmmaker {

namespace {

// Resolves an entity by index the same way MirvCosmetics.cpp / CameraEditorHud.cpp do: bounds +
// null-list checks before touching the (possibly stale) global pointers.
CEntityInstance* EntFromIndex(int index) {
	if (index < 0 || index > GetHighestEntityIndex() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

// Same heuristic as CameraEditorHud.cpp's LooksLikeWeaponEntity: matches both server-style
// "weapon_*" class names and the client "CWeaponX" / "C_WeaponX" schema class names, so it is
// resilient to either naming scheme being present at runtime.
bool LooksLikeWeaponEntity(CEntityInstance* entity) {
	const char* className = entity ? entity->GetClassName() : nullptr;
	const char* clientClass = entity ? entity->GetClientClassName() : nullptr;
	return (className && std::strstr(className, "weapon_"))
		|| (className && std::strstr(className, "Weapon"))
		|| (clientClass && std::strstr(clientClass, "Weapon"));
}

// POD result of the SEH-guarded write -- no constructors, safe to return out of __try/__except.
struct ApplyResult {
	bool patched = false;
	bool reverted = false;
	bool needComposite = false; // a (re)composite is warranted this frame (drives the optional vcall)
};

// SEH-isolated vtable call for the OPTIONAL forced re-composite (see CosmeticOverrideSystem.h /
// SetRecompose). POD-only body (no C++ objects) so __try/__except is permitted; a wrong index
// access-violates HERE and is reported via the return value instead of crashing. Signature matches
// CBasePlayerWeapon::UpdateComposite(bool) -- on x64 __thiscall == __fastcall. (1 ==
// EXCEPTION_EXECUTE_HANDLER; the literal avoids pulling in <windows.h> and its macro clashes.)
typedef void* (__fastcall* UpdateComposite_t)(void* thisptr, int arg);
bool SafeVCall(void* obj, int idx, int arg) {
	__try {
		void** vt = *(void***)obj;
		void* fn = vt[idx];
		if (!fn) return false;
		((UpdateComposite_t)fn)(obj, arg);
		return true;
	} __except (1) {
		return false;
	}
}

SOURCESDK::CS2::Cvar_s* FindPaintkitOverrideCvar() {
	if (!SOURCESDK::CS2::g_pCVar)
		return nullptr;
	SOURCESDK::CS2::ConVarHandle handle = SOURCESDK::CS2::g_pCVar->FindConVar("cl_paintkit_override", false);
	if (!handle.IsValid())
		return nullptr;
	return SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get());
}

bool ReadCvarInt(SOURCESDK::CS2::Cvar_s* cvar, int* out) {
	if (!cvar || !out)
		return false;
	__try {
		switch (cvar->m_eVarType) {
		case SOURCESDK::CS2::EConVarType_Bool:
			*out = cvar->m_Value.m_bValue ? 1 : 0;
			return true;
		case SOURCESDK::CS2::EConVarType_Int16:
			*out = (int)cvar->m_Value.m_i16Value;
			return true;
		case SOURCESDK::CS2::EConVarType_UInt16:
			*out = (int)cvar->m_Value.m_u16Value;
			return true;
		case SOURCESDK::CS2::EConVarType_Int32:
			*out = cvar->m_Value.m_i32Value;
			return true;
		case SOURCESDK::CS2::EConVarType_UInt32:
			*out = (int)cvar->m_Value.m_u32Value;
			return true;
		case SOURCESDK::CS2::EConVarType_Int64:
			*out = (int)cvar->m_Value.m_i64Value;
			return true;
		case SOURCESDK::CS2::EConVarType_UInt64:
			*out = (int)cvar->m_Value.m_u64Value;
			return true;
		case SOURCESDK::CS2::EConVarType_Float32:
			*out = (int)cvar->m_Value.m_flValue;
			return true;
		case SOURCESDK::CS2::EConVarType_Float64:
			*out = (int)cvar->m_Value.m_dbValue;
			return true;
		case SOURCESDK::CS2::EConVarType_String:
			*out = atoi(cvar->m_Value.m_szValue.Get());
			return true;
		default:
			return false;
		}
	} __except (1) {
		return false;
	}
}

bool WriteCvarInt(SOURCESDK::CS2::Cvar_s* cvar, int value) {
	if (!cvar)
		return false;
	__try {
		switch (cvar->m_eVarType) {
		case SOURCESDK::CS2::EConVarType_Bool:
			cvar->m_Value.m_bValue = value != 0;
			break;
		case SOURCESDK::CS2::EConVarType_Int16:
			cvar->m_Value.m_i16Value = (short)value;
			break;
		case SOURCESDK::CS2::EConVarType_UInt16:
			cvar->m_Value.m_u16Value = (uint16_t)value;
			break;
		case SOURCESDK::CS2::EConVarType_Int32:
			cvar->m_Value.m_i32Value = value;
			break;
		case SOURCESDK::CS2::EConVarType_UInt32:
			cvar->m_Value.m_u32Value = (uint32_t)value;
			break;
		case SOURCESDK::CS2::EConVarType_Int64:
			cvar->m_Value.m_i64Value = (int64_t)value;
			break;
		case SOURCESDK::CS2::EConVarType_UInt64:
			cvar->m_Value.m_u64Value = (uint64_t)value;
			break;
		case SOURCESDK::CS2::EConVarType_Float32:
			cvar->m_Value.m_flValue = (float)value;
			break;
		case SOURCESDK::CS2::EConVarType_Float64:
			cvar->m_Value.m_dbValue = (double)value;
			break;
		case SOURCESDK::CS2::EConVarType_String:
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%d", value);
			cvar->m_Value.m_szValue.Set(buf);
			break;
		}
		default:
			return false;
		}
		++cvar->m_iTimesChanged;
		return true;
	} __except (1) {
		return false;
	}
}

// SEH-guarded read of a weapon entity's econ identity (owner XUID + embedded C_EconItemView pointer
// + live item-definition index). LooksLikeWeaponEntity is only a class-name heuristic, so a
// "weapon_" entity that is not actually a C_EconEntity would access-violate if read directly --
// guarding here means such an entity is skipped, never crashing the game. POD-only body: `out` is
// constructed by the caller; inside __try we only assign primitives.
struct WeaponEconRead {
	uint64_t xuid = 0;
	unsigned char* itemView = nullptr;
	int liveDef = 0;
};
bool TryReadWeaponEconInfo(unsigned char* w, ptrdiff_t offXuidLow, ptrdiff_t offXuidHigh,
	ptrdiff_t offAttrMgr, ptrdiff_t offItem, ptrdiff_t offDef, WeaponEconRead* out) {
	__try {
		uint32_t xLow = *(uint32_t*)(w + offXuidLow);
		uint32_t xHigh = *(uint32_t*)(w + offXuidHigh);
		out->xuid = ((uint64_t)xHigh << 32) | (uint64_t)xLow;
		out->itemView = w + offAttrMgr + offItem;
		out->liveDef = (int)*(uint16_t*)(out->itemView + offDef);
		return true;
	} __except (1) {
		return false;
	}
}

// POD result of the networked-attribute overwrite. `changed` is true when at least one value was
// actually different from our target (drives the re-composite), `written` counts matched attributes.
struct AttrWriteResult {
	bool ok = false;
	int written = 0;
	bool changed = false;
};

// THE real skin write (Phase 2 breakthrough). A networked demo weapon stores its actual skin in
// m_NetworkedDynamicAttributes (def 6 = paint kit, 7 = seed, 8 = wear, 81 = StatTrak) -- the
// m_nFallback* fields are only consulted when the item has NO networked attributes, so on a demo
// item they are ignored. This OVERWRITES the matching attribute VALUES in place (it cannot add new
// ones -- a missing def, e.g. StatTrak on a non-ST gun, is skipped). Walks the same vector layout as
// CosmeticDebug.cpp::ReadAttrList. POD-only body so __try/__except is permitted. vectorField =
// itemView + networkedOff + attrSubOff (the CAttributeList::m_Attributes vector).
void WriteNetworkedSkinAttributes(unsigned char* itemView, ptrdiff_t networkedOff, ptrdiff_t attrSubOff,
	int stride, ptrdiff_t defOff, ptrdiff_t valOff, int paintKit, int seed, float wear, int statTrak,
	AttrWriteResult* out) {
	*out = AttrWriteResult{};
	if (networkedOff == 0 || attrSubOff == 0 || defOff == 0 || valOff == 0 || stride <= 0)
		return;
	unsigned char* vectorField = itemView + networkedOff + attrSubOff;
	__try {
		// Accept both observed vector layouts (count+ptr at +0/+8, or ptr+count at +0/+16), the same
		// tolerance as ReadAttrList.
		int count = *(int*)vectorField;
		unsigned char* data = *(unsigned char**)(vectorField + 8);
		if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
			data = *(unsigned char**)vectorField;
			count = *(int*)(vectorField + 16);
		}
		if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
			out->ok = true; // valid but empty -- nothing to overwrite
			return;
		}
		for (int i = 0; i < count; ++i) {
			unsigned char* attr = data + (ptrdiff_t)i * stride;
			int def = (int)*(uint16_t*)(attr + defOff);
			float nv;
			if (def == 6) nv = (float)paintKit;
			else if (def == 7) nv = (float)seed;
			else if (def == 8) nv = wear;
			else if (def == 81 && statTrak >= 0) nv = (float)statTrak;
			else continue;
			float* pv = (float*)(attr + valOff);
			if (*pv != nv) { *pv = nv; out->changed = true; }
			++out->written;
		}
		out->ok = true;
	} __except (1) {
		out->ok = false;
	}
}

// The proven write recipe from MirvCosmetics::Cosmetics_RunFrame, generalized to an arbitrary
// econ entity + arbitrary fallback offsets. Every pointer/offset is resolved by the caller BEFORE
// entering this function so the __try body only ever touches POD pointers/primitives -- required
// because __try/__except may not contain C++ objects that need unwinding.
//
// w        = entity base pointer (unsigned char*)
// itemView = w + m_AttributeManager + m_Item (the embedded C_EconItemView)
// offItemIdHigh/offPaint/offWear/offSeed/offStat = C_EconEntity fallback field offsets (from w)
// offDef                                          = C_EconItemView::m_iItemDefinitionIndex (from itemView)
// offItemIdLow/offAccountId                       = optional (0 = skip), from itemView
// offInit/offInitTags                             = optional (0 = skip), from itemView
// offRestoreMaterial/offImageReq/offImageTried/offCachedFile = optional item-view cache hints
// offAttrParity                                  = optional CAttributeManager cache hint
// offVisualsData/offClearUgc/offReloadEvent      = optional C_CSWeaponBase visuals-cache hints
// knifeDefOverride > 0 requests a def/model swap (best-effort; see header comment on agent/gloves
// for what is and is not safe to do here -- this is just numeric def swap, not a model load).
// markStaleEnabled = do the visuals-stale field writes when a value changed (per-frame auto-rebuild).
// forceStale       = do them unconditionally (the manual "rebuild once" path), even when unchanged.
ApplyResult ApplyCosmeticWrite(
	unsigned char* w,
	unsigned char* itemView,
	unsigned char* attrManager,
	ptrdiff_t offItemIdHigh,
	ptrdiff_t offPaint,
	ptrdiff_t offWear,
	ptrdiff_t offSeed,
	ptrdiff_t offStat,
	ptrdiff_t offDef,
	ptrdiff_t offItemIdLow,
	ptrdiff_t offAccountId,
	ptrdiff_t offInit,
	ptrdiff_t offInitTags,
	ptrdiff_t offAttrInit,
	ptrdiff_t offRestoreMaterial,
	ptrdiff_t offImageReq,
	ptrdiff_t offImageTried,
	ptrdiff_t offCachedFile,
	ptrdiff_t offAttrParity,
	ptrdiff_t offVisualsData,
	ptrdiff_t offClearUgc,
	ptrdiff_t offReloadEvent,
	int32_t paintKit,
	float wear,
	int32_t seed,
	int32_t statTrak,
	uint32_t accountId,
	int knifeDefOverride,
	bool writeFallbackId,
	bool markStaleEnabled,
	bool forceStale,
	CosmeticRebuildFlags flags) {
	__try {
		int32_t* pItemIdHigh = (int32_t*)(itemView + offItemIdHigh);
		int32_t* pPaint = (int32_t*)(w + offPaint);
		float* pWear = (float*)(w + offWear);
		int32_t* pSeed = (int32_t*)(w + offSeed);
		int32_t* pStat = (int32_t*)(w + offStat);
		uint16_t* pDef = (uint16_t*)(itemView + offDef);

		bool restoreItemId = !writeFallbackId && (*pItemIdHigh == -1);
		bool reverted = writeFallbackId && (*pItemIdHigh != -1);
		bool need = reverted
			|| restoreItemId
			|| (*pPaint != paintKit)
			|| (*pWear != wear)
			|| (*pSeed != seed)
			|| (*pStat != statTrak)
			|| (knifeDefOverride > 0 && *pDef != (uint16_t)knifeDefOverride);

		// itemIDHigh=-1 forces the legacy fallback-field path. Opt-in only: it INVALIDATES the item
		// id (breaks the UI inventory read -- "no AK") and is unnecessary now that we overwrite the
		// networked attributes directly. Default OFF (see m_fallbackId / "cosmetics fallback").
		if (writeFallbackId) {
			*pItemIdHigh = -1;
			if (offAccountId) *(uint32_t*)(itemView + offAccountId) = accountId; // best-effort
			if (offItemIdLow) *(uint32_t*)(itemView + offItemIdLow) = 0;         // best-effort
		} else if (restoreItemId) {
			// Older builds / live diagnostics could leave demo weapons in fallback mode, which makes
			// CS2's loadout HUD lose the real weapon icon. In normal mode keep the demo item id valid.
			*pItemIdHigh = 0;
		}
		*pPaint = paintKit;
		*pWear = wear;
		*pSeed = seed;
		*pStat = statTrak;
		if (knifeDefOverride > 0) *pDef = (uint16_t)knifeDefOverride; // best-effort knife DEF swap

		// The visuals-stale field writes. Gated on a value changing AND per-frame auto-rebuild being on
		// (markStaleEnabled), OR forceStale (the "rebuild once" path) which re-asserts unconditionally.
		// EACH individual write is ALSO gated on its rebuild flag (all default OFF) so the harmful ones
		// (clearUgc/initialized blank the HUD weapon icon) are not written unless explicitly enabled for
		// research -- see CosmeticRebuildFlags / "cosmetics rebuildflags". Default = none -> HUD-safe.
		bool doStaleWrites = (need && markStaleEnabled) || forceStale;
		if (doStaleWrites) {
			if (flags.initialized) {
				if (offInit) *(bool*)(itemView + offInit) = false;
				if (offInitTags) *(bool*)(itemView + offInitTags) = false;
			}
			if (flags.imageCache) {
				if (offRestoreMaterial) *(bool*)(itemView + offRestoreMaterial) = true;
				if (offImageReq) *(bool*)(itemView + offImageReq) = false;
				if (offImageTried) *(bool*)(itemView + offImageTried) = false;
				if (offCachedFile) *(char*)(itemView + offCachedFile) = 0;
			}
			if (flags.attrParity && attrManager && offAttrParity) {
				int32_t* parity = (int32_t*)(attrManager + offAttrParity);
				*parity = *parity + 1;
			}
			if (flags.visualsDataSet && offVisualsData) *(bool*)(w + offVisualsData) = false;
			if (flags.clearUgc && offClearUgc) *(bool*)(w + offClearUgc) = true;
			if (flags.reloadEvent && offReloadEvent) {
				// Set m_nCustomEconReloadEventId NEGATIVE (not ++). Binary analysis of the live client.dll
				// (docs/cosmetics-recompose-research.md, 2026-06-28) shows C_CSWeaponBase vtable slot 11
				// (the econ OnDataChanged path) only requests a "clientside_reload_custom_econ" composite
				// rebuild when this field is < 0 ("cmp dword [weapon+0x18bc],0 / jge skip"); a positive value
				// (the old ++ bump) is the skip case, which is why it never rebuilt. -1 == "reload me".
				int32_t* reloadEvent = (int32_t*)(w + offReloadEvent);
				*reloadEvent = -1;
			}
			// Clear C_EconEntity::m_bAttributesInitialized on the WEAPON (offset from w, NOT itemView)
			// to try to force an econ attribute / composite re-init. See cosmetics-recompose-research.md.
			if (flags.attrInit && offAttrInit) *(bool*)(w + offAttrInit) = false;
		}

		ApplyResult r;
		r.patched = true;
		r.reverted = (reverted && need);
		r.needComposite = need;
		return r;
	} __except (1) {
		// A misclassified entity / bad offset access-violates HERE and is swallowed instead of
		// crashing the game (1 == EXCEPTION_EXECUTE_HANDLER; the literal avoids <windows.h> macro
		// clashes the same way MirvCosmetics::SafeVCall does).
		return ApplyResult{};
	}
}

} // namespace

CosmeticOverrideSystem& CosmeticsRef() {
	static CosmeticOverrideSystem s;
	return s;
}

void Cosmetics_RunFrame() {
	CosmeticsRef().RunFrame();
}

void CosmeticOverrideSystem::Init() {
	if (m_initialized)
		return;
	m_store.Load();
	m_initialized = true;
}

void CosmeticOverrideSystem::Shutdown() {
	RestorePaintkitBridgeCvar();
	m_store.Save(); // best-effort; Save() itself swallows IO failures
}

bool CosmeticOverrideSystem::OffsetsAvailable() const {
	return g_cosmeticsOffsetsOk;
}

bool CosmeticOverrideSystem::InDemoContext() const {
	return !ResolvePlayingDemoPath().empty();
}

void CosmeticOverrideSystem::SetEnabled(bool e) {
	m_store.SetEnabled(e);
}

void CosmeticOverrideSystem::Save() {
	m_store.Save();
}

void CosmeticOverrideSystem::SetRecompose(bool e) {
	m_recompose = e;
	if (e) m_recomposeFaulted = false;
}

void CosmeticOverrideSystem::GetVtIdx(int* comp, int* sec) const {
	if (comp) *comp = m_vtComposite;
	if (sec) *sec = m_vtCompositeSec;
}

void CosmeticOverrideSystem::SetVtIdx(int comp, int sec) {
	m_vtComposite = comp;
	m_vtCompositeSec = sec;
	m_recompose = true;        // re-arm so the new indices get tried next frame
	m_recomposeFaulted = false;
}

void CosmeticOverrideSystem::SetWeapon(uint64_t steamId, int defIndex, int paintKit, float wear, int seed, int statTrak) {
	CosmeticProfile& profile = m_store.GetOrCreate(steamId);
	CosmeticSlot slot = CosmeticCatalog::SlotForDefIndex(defIndex);

	CosmeticItem* item = nullptr;
	if (slot == CosmeticSlot::Knife) {
		item = &profile.knife;
	} else if (slot == CosmeticSlot::Secondary) {
		item = &profile.secondary;
	} else {
		// Primary or None (defIndex 0 = "keep the held weapon's own def") -> primary slot.
		item = &profile.primary;
	}

	item->set = true;
	item->defIndex = defIndex;
	item->paintKit = paintKit;
	item->wear = wear;
	item->seed = seed;
	item->statTrak = statTrak;
	// statTrak is left as-supplied for weapons (unlike SetKnife/SetGloves which force -1).

	std::string name = NameForSteamId(steamId);
	if (!name.empty())
		profile.name = name;
}

void CosmeticOverrideSystem::SetKnife(uint64_t steamId, int defIndex, int paintKit, float wear, int seed) {
	CosmeticProfile& profile = m_store.GetOrCreate(steamId);
	profile.knife.set = true;
	profile.knife.defIndex = defIndex;
	profile.knife.paintKit = paintKit;
	profile.knife.wear = wear;
	profile.knife.seed = seed;
	profile.knife.statTrak = -1;

	std::string name = NameForSteamId(steamId);
	if (!name.empty())
		profile.name = name;
}

void CosmeticOverrideSystem::SetGloves(uint64_t steamId, int defIndex, int paintKit, float wear, int seed) {
	CosmeticProfile& profile = m_store.GetOrCreate(steamId);
	profile.gloves.set = true;
	profile.gloves.defIndex = defIndex;
	profile.gloves.paintKit = paintKit;
	profile.gloves.wear = wear;
	profile.gloves.seed = seed;
	profile.gloves.statTrak = -1;

	std::string name = NameForSteamId(steamId);
	if (!name.empty())
		profile.name = name;
}

void CosmeticOverrideSystem::SetAgent(uint64_t steamId, const char* model, int defIndex) {
	CosmeticProfile& profile = m_store.GetOrCreate(steamId);
	if (!model || !*model || 0 == _stricmp(model, "default")) {
		profile.agent.set = false;
		profile.agent.model.clear();
	} else {
		profile.agent.set = true;
		profile.agent.model = model;
		profile.agent.defIndex = defIndex;
	}

	std::string name = NameForSteamId(steamId);
	if (!name.empty())
		profile.name = name;
}

void CosmeticOverrideSystem::ClearPlayer(uint64_t steamId) {
	m_store.ClearPlayer(steamId);
}

void CosmeticOverrideSystem::ClearAll() {
	m_store.ClearAll();
}

int CosmeticOverrideSystem::CurrentSpectatedPawnIndex() const {
	// Robust spectated-pawn resolution: engine observer-services first, then a render-eye match
	// against the game camera. The observer-services path alone reads target -1 in POV/GOTV demos
	// (so 'current' could never resolve there); the eye-match fallback fixes that. See
	// AfxGetSpectatedPawnIndex in ClientEntitySystem.cpp.
	return AfxGetSpectatedPawnIndex();
}

uint8_t CosmeticOverrideSystem::CurrentObserverMode() const {
	return AfxGetLocalObserverState(nullptr);
}

uint64_t CosmeticOverrideSystem::CurrentSpectatedSteamId() const {
	int pawnIndex = CurrentSpectatedPawnIndex();
	if (pawnIndex < 0)
		return 0;

	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = EntFromIndex(i);
		if (!ent || !ent->IsPlayerController())
			continue;
		if (ent->GetPlayerPawnHandle().GetEntryIndex() == pawnIndex)
			return ent->GetSteamId();
	}
	return 0;
}

std::string CosmeticOverrideSystem::NameForSteamId(uint64_t steamId) const {
	if (steamId == 0)
		return std::string();

	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = EntFromIndex(i);
		if (!ent || !ent->IsPlayerController())
			continue;
		if (ent->GetSteamId() != steamId)
			continue;
		const char* sanitized = ent->GetSanitizedPlayerName();
		if (sanitized && *sanitized)
			return std::string(sanitized);
		const char* raw = ent->GetPlayerName();
		if (raw && *raw)
			return std::string(raw);
		return std::string();
	}
	return std::string();
}

void CosmeticOverrideSystem::RunFrame() {
	// Cheap no-op gate: disabled, offsets unresolved, no profiles, or no demo playing.
	if (!m_store.Enabled() || !g_cosmeticsOffsetsOk || m_store.Empty() || !InDemoContext()) {
		UpdatePaintkitBridge();
		return;
	}

	++m_lastStats.frame;
	m_lastStats.entitiesScanned = 0;
	m_lastStats.entitiesMatched = 0;
	m_lastStats.entitiesPatched = 0;
	m_lastStats.entitiesReverted = 0;
	m_lastStats.attrListsRead = 0;
	m_lastStats.attrValuesWritten = 0;
	m_lastStats.attrValuesChanged = 0;
	m_lastStats.attrListsEmpty = 0;
	m_lastStats.paintkitBridgeValue = 0;

	// Per-frame: mark visuals stale only on change (gated by m_rebuildAuto inside ApplyCosmeticWrite),
	// and fire the recompose vcall only if the user armed it via "cosmetics recompose 1".
	ApplyMatchedWeapons(/*forceStale=*/false, /*fireRebuildCall=*/m_recompose);
	UpdatePaintkitBridge();
}

// Manual, on-demand rebuild: re-asserts the visuals-stale field writes (forceStale) on every matched
// weapon entity even when nothing changed, and fires the recompose vcall once if armed. Decoupled
// from the per-frame change detection so the user can retrigger a refresh attempt for a screenshot.
// Does NOT require the apply loop to be enabled -- it is an explicit action -- but still hard-gated on
// offsets resolving, a demo playing, and at least one stored profile. Returns matched entity count.
int CosmeticOverrideSystem::RebuildOnce() {
	if (!g_cosmeticsOffsetsOk || !InDemoContext() || m_store.Empty())
		return 0;
	m_lastStats.entitiesScanned = 0;
	m_lastStats.entitiesMatched = 0;
	m_lastStats.entitiesPatched = 0;
	m_lastStats.entitiesReverted = 0;
	m_lastStats.attrListsRead = 0;
	m_lastStats.attrValuesWritten = 0;
	m_lastStats.attrValuesChanged = 0;
	m_lastStats.attrListsEmpty = 0;
	return ApplyMatchedWeapons(/*forceStale=*/true, /*fireRebuildCall=*/m_recompose);
}

bool CosmeticOverrideSystem::SetRebuildFlag(const char* name, bool on) {
	if (!name) return false;
	if (0 == _stricmp(name, "visualsdata") || 0 == _stricmp(name, "visualsdataset")) m_rebuildFlags.visualsDataSet = on;
	else if (0 == _stricmp(name, "clearugc")) m_rebuildFlags.clearUgc = on;
	else if (0 == _stricmp(name, "reloadevent")) m_rebuildFlags.reloadEvent = on;
	else if (0 == _stricmp(name, "initialized") || 0 == _stricmp(name, "init")) m_rebuildFlags.initialized = on;
	else if (0 == _stricmp(name, "attrinit")) m_rebuildFlags.attrInit = on;
	else if (0 == _stricmp(name, "imagecache")) m_rebuildFlags.imageCache = on;
	else if (0 == _stricmp(name, "attrparity") || 0 == _stricmp(name, "parity")) m_rebuildFlags.attrParity = on;
	else return false;
	return true;
}

void CosmeticOverrideSystem::SetAllRebuildFlags(bool on) {
	m_rebuildFlags.visualsDataSet = on;
	m_rebuildFlags.clearUgc = on;
	m_rebuildFlags.reloadEvent = on;
	m_rebuildFlags.initialized = on;
	m_rebuildFlags.attrInit = on;
	m_rebuildFlags.imageCache = on;
	m_rebuildFlags.attrParity = on;
}

void CosmeticOverrideSystem::SetPaintkitBridge(bool e) {
	if (m_paintkitBridge == e) {
		if (m_paintkitBridge)
			UpdatePaintkitBridge();
		return;
	}
	m_paintkitBridge = e;
	if (!m_paintkitBridge)
		RestorePaintkitBridgeCvar();
	else
		UpdatePaintkitBridge();
}

bool CosmeticOverrideSystem::SetPaintkitBridgeCvar(int value) {
	SOURCESDK::CS2::Cvar_s* cvar = FindPaintkitOverrideCvar();
	m_paintkitBridgeCvarFound = cvar != nullptr;
	if (!cvar)
		return false;

	int current = 0;
	if (!ReadCvarInt(cvar, &current))
		return false;

	if (!m_paintkitBridgeHaveOriginal) {
		m_paintkitBridgeOriginalValue = current;
		m_paintkitBridgeHaveOriginal = true;
	}

	if (current != value && !WriteCvarInt(cvar, value))
		return false;

	m_paintkitBridgeLastValue = value;
	return true;
}

void CosmeticOverrideSystem::RestorePaintkitBridgeCvar() {
	if (!m_paintkitBridgeHaveOriginal) {
		m_paintkitBridgeLastValue = 0;
		return;
	}

	SOURCESDK::CS2::Cvar_s* cvar = FindPaintkitOverrideCvar();
	m_paintkitBridgeCvarFound = cvar != nullptr;
	if (cvar)
		WriteCvarInt(cvar, m_paintkitBridgeOriginalValue);
	m_paintkitBridgeHaveOriginal = false;
	m_paintkitBridgeLastValue = 0;
	m_lastStats.paintkitBridgeValue = 0;
}

int CosmeticOverrideSystem::ResolveSpectatedPaintkitOverride() const {
	if (m_paintkitBridgeForcedValue > 0)
		return m_paintkitBridgeForcedValue;

	if (!m_store.Enabled() || !g_cosmeticsOffsetsOk || m_store.Empty() || !InDemoContext())
		return 0;

	const int pawnIndex = CurrentSpectatedPawnIndex();
	if (pawnIndex < 0)
		return 0;

	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return 0;

	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	CEntityInstance* weapon = wh.IsValid() ? EntFromIndex(wh.GetEntryIndex()) : nullptr;
	if (!weapon || !LooksLikeWeaponEntity(weapon))
		return 0;

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	WeaponEconRead econ;
	if (!TryReadWeaponEconInfo((unsigned char*)weapon,
			o.C_EconEntity.m_OriginalOwnerXuidLow, o.C_EconEntity.m_OriginalOwnerXuidHigh,
			o.C_EconEntity.m_AttributeManager, o.C_AttributeContainer.m_Item,
			o.C_EconItemView.m_iItemDefinitionIndex, &econ))
		return 0;

	if (econ.xuid == 0)
		return 0;

	const CosmeticProfile* prof = m_store.Find(econ.xuid);
	if (!prof)
		return 0;

	const CosmeticItem* item = nullptr;
	if (CosmeticCatalog::IsKnifeDef(econ.liveDef)) {
		if (prof->knife.set)
			item = &prof->knife;
	} else {
		CosmeticSlot slot = CosmeticCatalog::SlotForDefIndex(econ.liveDef);
		const CosmeticItem* cand = (slot == CosmeticSlot::Secondary) ? &prof->secondary
			: (slot == CosmeticSlot::Primary) ? &prof->primary
			: nullptr;
		if (cand && cand->set && (cand->defIndex == 0 || cand->defIndex == econ.liveDef))
			item = cand;
	}

	if (!item || item->paintKit <= 0)
		return 0;
	return item->paintKit;
}

void CosmeticOverrideSystem::UpdatePaintkitBridge() {
	if (!m_paintkitBridge) {
		m_lastStats.paintkitBridgeValue = 0;
		return;
	}

	const int paintKit = ResolveSpectatedPaintkitOverride();
	if (paintKit <= 0) {
		RestorePaintkitBridgeCvar();
		return;
	}

	if (SetPaintkitBridgeCvar(paintKit))
		m_lastStats.paintkitBridgeValue = paintKit;
	else
		m_lastStats.paintkitBridgeValue = 0;
}

int CosmeticOverrideSystem::ApplyMatchedWeapons(bool forceStale, bool fireRebuildCall) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;

	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = EntFromIndex(i);
		if (!ent)
			continue;
		if (!LooksLikeWeaponEntity(ent))
			continue; // gloves/agent are not econ weapon entities; handled separately below

		unsigned char* w = (unsigned char*)ent;

		// SEH-guarded read of owner XUID + embedded C_EconItemView pointer + live item-definition
		// index. A "weapon_" entity that is not actually a C_EconEntity is skipped here instead of
		// crashing. Dropped weapons keep OriginalOwnerXuid, so a dropped gun still carries its
		// owner's override.
		WeaponEconRead econ;
		if (!TryReadWeaponEconInfo(w,
				o.C_EconEntity.m_OriginalOwnerXuidLow, o.C_EconEntity.m_OriginalOwnerXuidHigh,
				o.C_EconEntity.m_AttributeManager, o.C_AttributeContainer.m_Item,
				o.C_EconItemView.m_iItemDefinitionIndex, &econ))
			continue;

		uint64_t xuid = econ.xuid;
		if (xuid == 0)
			continue;

		CosmeticProfile* prof = m_store.Find(xuid);
		if (!prof)
			continue;

		++m_lastStats.entitiesScanned;

		unsigned char* itemView = econ.itemView;
		int liveDef = econ.liveDef;

		CosmeticItem* item = nullptr;
		int knifeDefOverride = 0;

		if (CosmeticCatalog::IsKnifeDef(liveDef)) {
			if (prof->knife.set) {
				item = &prof->knife;
				if (item->defIndex > 0 && item->defIndex != liveDef)
					knifeDefOverride = item->defIndex;
			}
		} else {
			CosmeticSlot slot = CosmeticCatalog::SlotForDefIndex(liveDef);
			CosmeticItem* cand = (slot == CosmeticSlot::Secondary) ? &prof->secondary
				: (slot == CosmeticSlot::Primary) ? &prof->primary
				: nullptr;
			// A paint kit is weapon-specific: only apply when the override is "any weapon in this
			// slot" (defIndex 0) or it explicitly matches the currently-held weapon def.
			if (cand && cand->set && (cand->defIndex == 0 || cand->defIndex == liveDef))
				item = cand;
		}

		if (!item)
			continue;

		++m_lastStats.entitiesMatched;

		// THE real skin write (Phase 2): overwrite the networked dynamic attributes (def 6/7/8/81),
		// where a demo weapon's actual skin lives. The fallback-field ApplyCosmeticWrite below is now
		// a harmless backup (those fields are ignored while networked attributes are present).
		int attrStride = (int)o.CEconItemAttribute.m_size;
		int attrMinStride = (int)o.CEconItemAttribute.m_flValue + (int)sizeof(float);
		if (attrStride < attrMinStride) attrStride = attrMinStride;
		AttrWriteResult attr;
		WriteNetworkedSkinAttributes(itemView,
			o.C_EconItemView.m_NetworkedDynamicAttributes, o.C_AttributeList.m_Attributes, attrStride,
			o.CEconItemAttribute.m_iAttributeDefinitionIndex, o.CEconItemAttribute.m_flValue,
			item->paintKit, item->seed, item->wear, item->statTrak, &attr);
		if (attr.ok) {
			++m_lastStats.attrListsRead;
			m_lastStats.attrValuesWritten += attr.written;
			if (attr.changed)
				++m_lastStats.attrValuesChanged;
			if (attr.written == 0)
				++m_lastStats.attrListsEmpty;
		}

		ApplyResult result = ApplyCosmeticWrite(
			w,
			itemView,
			w + o.C_EconEntity.m_AttributeManager,
			o.C_EconItemView.m_iItemIDHigh,
			o.C_EconEntity.m_nFallbackPaintKit,
			o.C_EconEntity.m_flFallbackWear,
			o.C_EconEntity.m_nFallbackSeed,
			o.C_EconEntity.m_nFallbackStatTrak,
			o.C_EconItemView.m_iItemDefinitionIndex,
			o.C_EconItemView.m_iItemIDLow,
			o.C_EconItemView.m_iAccountID,
			o.C_EconItemView.m_bInitialized,
			o.C_EconItemView.m_bInitializedTags,
			o.C_EconEntity.m_bAttributesInitialized,
			o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache,
			o.C_EconItemView.m_bInventoryImageRgbaRequested,
			o.C_EconItemView.m_bInventoryImageTriedCache,
			o.C_EconItemView.m_szCurrentLoadCachedFileName,
			o.CAttributeManager.m_iReapplyProvisionParity,
			o.C_CSWeaponBase.m_bVisualsDataSet,
			o.C_CSWeaponBase.m_bClearWeaponIdentifyingUGC,
			o.C_CSWeaponBase.m_nCustomEconReloadEventId,
			(int32_t)item->paintKit,
			item->wear,
			(int32_t)item->seed,
			(int32_t)item->statTrak,
			(uint32_t)xuid,
			knifeDefOverride,
			m_fallbackId,
			m_rebuildAuto,
			forceStale,
			m_rebuildFlags);

		if (result.patched)
			++m_lastStats.entitiesPatched;
		if (result.reverted)
			++m_lastStats.entitiesReverted;

			// Optional forced re-composite (OFF by default; see SetRecompose). Writing the fallback
			// fields + invalidating the init flags is not always enough to make a demo entity visually
			// rebuild its skin material -- this calls the weapon's UpdateComposite vtable method when a
			// (re)composite is warranted. SEH-guarded: a wrong index disables recompose, never crashes.
			// fireRebuildCall lets RunFrame keep this opt-in (only when m_recompose is armed) while the
			// manual "rebuild once" path forces a call attempt on every matched entity (forceStale).
			if (fireRebuildCall && (result.needComposite || attr.changed || forceStale)) {
				bool ok = true;
				if (m_vtComposite >= 0) ok = SafeVCall(ent, m_vtComposite, m_vtArg);
				if (ok && m_vtCompositeSec >= 0) ok = SafeVCall(ent, m_vtCompositeSec, m_vtArg);
				if (!ok) { m_recompose = false; m_recomposeFaulted = true; }
			}
	}

	// GLOVES + AGENT (milestones 12-13, research-gated): writing glove/agent model overrides is
	// NOT implemented yet. The model/glove application path (which involves a model swap, not a
	// simple fallback-field composite like weapons/knives) is still being researched -- writing
	// the wrong fields here risks a crash. Profiles with gloves.set / agent.set are stored and
	// persisted (CosmeticProfileStore handles that independently of RunFrame), but intentionally
	// left UN-APPLIED until the model-override research lands (see
	// docs/cosmetics-model-override-research.md). "cosmetics status" should report any such
	// profiles as "stored (not yet applied)" rather than silently doing nothing.
	//
	// No entity writes happen in this block on purpose -- do not add any here without first
	// reading the research doc above.

	return m_lastStats.entitiesMatched;
}

} // namespace Filmmaker
