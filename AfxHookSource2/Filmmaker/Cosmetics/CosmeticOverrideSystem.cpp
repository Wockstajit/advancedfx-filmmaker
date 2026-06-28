#include "CosmeticOverrideSystem.h"

#include "CosmeticCatalog.h"

#include "../Demo/PlayingDemoPath.h"

#include "../../ClientEntitySystem.h" // CEntityInstance, entity-list globals, CBaseHandle, AfxGetLocalObserverState
#include "../../SchemaSystem.h"        // g_clientDllOffsets, g_cosmeticsOffsetsOk

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"

#include <cstdint>
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
typedef void* (__fastcall* UpdateComposite_t)(void* thisptr, bool bRegenerate);
bool SafeVCall(void* obj, int idx, bool arg) {
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
// knifeDefOverride > 0 requests a def/model swap (best-effort; see header comment on agent/gloves
// for what is and is not safe to do here -- this is just numeric def swap, not a model load).
ApplyResult ApplyCosmeticWrite(
	unsigned char* w,
	unsigned char* itemView,
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
	int32_t paintKit,
	float wear,
	int32_t seed,
	int32_t statTrak,
	uint32_t accountId,
	int knifeDefOverride) {
	__try {
		int32_t* pItemIdHigh = (int32_t*)(itemView + offItemIdHigh);
		int32_t* pPaint = (int32_t*)(w + offPaint);
		float* pWear = (float*)(w + offWear);
		int32_t* pSeed = (int32_t*)(w + offSeed);
		int32_t* pStat = (int32_t*)(w + offStat);
		uint16_t* pDef = (uint16_t*)(itemView + offDef);

		bool reverted = (*pItemIdHigh != -1);
		bool need = reverted
			|| (*pPaint != paintKit)
			|| (*pWear != wear)
			|| (*pSeed != seed)
			|| (*pStat != statTrak)
			|| (knifeDefOverride > 0 && *pDef != (uint16_t)knifeDefOverride);

		*pItemIdHigh = -1;
		if (offAccountId) *(uint32_t*)(itemView + offAccountId) = accountId; // best-effort
		if (offItemIdLow) *(uint32_t*)(itemView + offItemIdLow) = 0;         // best-effort
		*pPaint = paintKit;
		*pWear = wear;
		*pSeed = seed;
		*pStat = statTrak;
		if (knifeDefOverride > 0) *pDef = (uint16_t)knifeDefOverride; // best-effort knife DEF swap

		if (need) {
			if (offInit) *(bool*)(itemView + offInit) = false;
			if (offInitTags) *(bool*)(itemView + offInitTags) = false;
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
	if (!m_store.Enabled() || !g_cosmeticsOffsetsOk || m_store.Empty() || !InDemoContext())
		return;

	const ClientDllOffsets_t& o = g_clientDllOffsets;

	++m_lastStats.frame;
	m_lastStats.entitiesScanned = 0;
	m_lastStats.entitiesMatched = 0;
	m_lastStats.entitiesPatched = 0;
	m_lastStats.entitiesReverted = 0;

	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = EntFromIndex(i);
		if (!ent)
			continue;
		if (!LooksLikeWeaponEntity(ent))
			continue; // gloves/agent are not econ weapon entities; handled separately below

		unsigned char* w = (unsigned char*)ent;

		// Resolve the ORIGINAL owner's XUID -> SteamID64. Dropped weapons keep this set, so a
		// dropped gun still carries its owner's override.
		uint32_t xLow = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidLow);
		uint32_t xHigh = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidHigh);
		uint64_t xuid = ((uint64_t)xHigh << 32) | (uint64_t)xLow;
		if (xuid == 0)
			continue;

		CosmeticProfile* prof = m_store.Find(xuid);
		if (!prof)
			continue;

		++m_lastStats.entitiesScanned;

		unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;
		int liveDef = (int)*(uint16_t*)(itemView + o.C_EconItemView.m_iItemDefinitionIndex);

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

		ApplyResult result = ApplyCosmeticWrite(
			w,
			itemView,
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
			(int32_t)item->paintKit,
			item->wear,
			(int32_t)item->seed,
			(int32_t)item->statTrak,
			xLow,
			knifeDefOverride);

		if (result.patched)
			++m_lastStats.entitiesPatched;
		if (result.reverted)
			++m_lastStats.entitiesReverted;

			// Optional forced re-composite (OFF by default; see SetRecompose). Writing the fallback
			// fields + invalidating the init flags is not always enough to make a demo entity visually
			// rebuild its skin material -- this calls the weapon's UpdateComposite vtable method when a
			// (re)composite is warranted. SEH-guarded: a wrong index disables recompose, never crashes.
			if (m_recompose && result.needComposite) {
				bool ok = true;
				if (m_vtComposite >= 0) ok = SafeVCall(ent, m_vtComposite, true);
				if (ok && m_vtCompositeSec >= 0) ok = SafeVCall(ent, m_vtCompositeSec, true);
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
}

} // namespace Filmmaker
