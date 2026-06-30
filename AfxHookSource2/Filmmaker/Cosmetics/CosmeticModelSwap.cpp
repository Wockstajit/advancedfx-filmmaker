#include "CosmeticModelSwap.h"

#include "../../ClientEntitySystem.h"   // entity-list globals, g_GetEntityFromIndex
#include "../../SchemaSystem.h"          // g_clientDllOffsets
#include "../../SceneSystem.h"           // CResourceSystem / g_pCResourceSystem (blocking resource precache)
#include "../../../shared/binutils.h"
#include "CosmeticDebugLog.h"           // MvmDebugLog_Active / MvmDebugLog_Linef (diagnostics)
#include "CosmeticAnimFix.h"            // EnsureAnimCrashFixInstalled (knife-swap anim crash fix detour)

#include <cstring>
#include <cstdio>

namespace Filmmaker {

namespace {

// ---- client.dll pattern resolution (same approach as CosmeticOverrideSystem.cpp) -------------------
// FindPatternString consumes TWO hex chars per wildcard byte, so a wildcard MUST be "??" not "?".

size_t FindClientPattern(const char* pattern) {
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client)
		return 0;
	Afx::BinUtils::ImageSectionsReader sections(client);
	if (sections.Eof())
		return 0;
	Afx::BinUtils::MemRange result = Afx::BinUtils::FindPatternString(sections.GetMemRange(), pattern);
	return result.IsEmpty() ? 0 : result.Start;
}

void* ResolveRelCall(size_t callAddr) {
	if (!callAddr)
		return nullptr;
	int32_t rel = *(int32_t*)(callAddr + 1);
	return (void*)(callAddr + 5 + rel);
}

// ---- resolved function typedefs (x64 __thiscall == __fastcall) --------------------------------------
typedef void (__fastcall* SetModel_t)(void* modelEntity, const char* model);
typedef void (__fastcall* SetMeshGroupMask_t)(void* sceneNode, uint64_t mask);
typedef void (__fastcall* UpdateSubclass_t)(void* weapon);
typedef void (__fastcall* SetBodyGroup_t)(void* pawn, const char* group, unsigned int unk);
typedef void (__fastcall* UpdateBodyGroupChoice_t)(void* entity);
typedef void* (__fastcall* GetStaticData_t)(void* econItemView);
typedef void* (__fastcall* GetEconItemSystem_t)(void* unused);
// C_EconItemView::SetAttributeValueByName(view, name, float) -- the engine named-setter used to ADD a
// paint attribute to an item that has none (the default-glove / default-weapon attrWritten=0 case).
typedef void (__fastcall* SetAttributeValueByName_t)(void* itemView, const char* name, float value);
// CGameSceneNode::PostDataUpdate(this, 0, 0) -- vtable index 22 (Andromeda SDK::VMT_Index, current
// build). Forces the renderable to re-derive its model/mesh/body-group, so a swap shows while paused.
typedef void (__fastcall* PostDataUpdate_t)(void* sceneNode, int a, int b);
constexpr int kPostDataUpdateVtableIndex = 22;

struct Fns {
	SetModel_t setModel = nullptr;
	SetMeshGroupMask_t setMeshGroupMask = nullptr;
	UpdateSubclass_t updateSubclass = nullptr;
	SetBodyGroup_t setBodyGroup = nullptr;
	UpdateBodyGroupChoice_t updateBodyGroupChoice = nullptr;
	GetStaticData_t getStaticData = nullptr;
	GetEconItemSystem_t getEconItemSystem = nullptr;
	SetAttributeValueByName_t setAttributeValueByName = nullptr;
};

Fns g_fns;
ModelSwapResolveStatus g_status;

// --- experimental animgraph reset (approach #1; see CosmeticModelSwap.h) ---------------------------
// DISPROVEN for this build (default OFF). The reference offsets are the overlay-research VIEWMODEL/AG1
// layout; on the demo world weapon the instance read at 0xD08 returns null (logged inst=0x0 wrote=0), and
// current CS2 weapons run AG2 (m_pGraphInstanceAG2) where the null-vars trick does not map. Kept toggleable
// for experiments ("cosmetics animreset 1 [offsets <instHex> <varsHex>]"). The real fix is precache (#2).
bool g_animResetOn = false;
uint32_t g_animInstOff = 0xD08;  // entity -> CAnimationGraphInstance*
uint32_t g_animVarsOff = 0x2E0;  // CAnimationGraphInstance -> pAnimGraphNetworkedVariables

// --- approach #2: blocking model precache before SetModel (the root-cause fix; see header) ----------
// Default ON. Counters are diagnostics-only (surfaced in the precache breadcrumb).
bool g_precacheOn = true;
uint64_t g_precacheCalls = 0;

// POD result of the SEH-guarded reset read/write (no C++ objects in the __try body).
struct AnimResetResult { uintptr_t inst = 0; bool wrote = false; bool faulted = false; };

void DoResetAnimGraph(unsigned char* entity, uint32_t instOff, uint32_t varsOff, AnimResetResult* out) {
	*out = AnimResetResult{};
	__try {
		void* inst = *(void**)(entity + instOff);
		out->inst = (uintptr_t)inst;
		if ((uintptr_t)inst > 0x10000) {
			*(void**)((unsigned char*)inst + varsOff) = nullptr;
			out->wrote = true;
		}
	} __except (1) {
		out->faulted = true;
	}
}

// Reset the entity's animgraph networked-variables pointer after a model swap, then log a breadcrumb
// (kept OUT of the __try so MvmDebugLog's std::string is legal). `tag` distinguishes world vs viewmodel.
void ResetAnimGraph(unsigned char* entity, int entityIndex, const char* tag) {
	if (!g_animResetOn || !entity)
		return;
	AnimResetResult r;
	DoResetAnimGraph(entity, g_animInstOff, g_animVarsOff, &r);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("knife.swap",
			"step=animreset.%s idx=%d instOff=0x%x inst=0x%llx wrote=%d faulted=%d varsOff=0x%x",
			tag, entityIndex, g_animInstOff, (unsigned long long)r.inst, r.wrote ? 1 : 0, r.faulted ? 1 : 0, g_animVarsOff);
}

// CEconItemDefinition::m_pszModelName -- a raw econ-data offset (NOT a schema field), confirmed
// identical in Andromeda (0x148) and nerv (get_model_name @ 0x148). Build-specific; the GetStaticData
// path validates the returned string and falls back to the knife table below if it looks wrong.
constexpr ptrdiff_t kEconItemDefModelNameOffset = 0x148;
// Econ item schema layout (Andromeda g_CEconItemSchema_GetPaintKits / nerv OFFSET_PAINT_KITS).
constexpr ptrdiff_t kEconItemSystem_SchemaOffset = 0x8;
constexpr ptrdiff_t kEconItemSchema_PaintKitsOffset = 0x2F0;
constexpr ptrdiff_t kPaintKit_IsUseLegacyModelOffset = 0xAE;

// ---- built-in knife def -> world model fallback (used if GetStaticData is unavailable / invalid) ----
// Stable Valve asset paths; mirror the CEconItemDefinition::m_pszModelName values (Andromeda live dump
// confirmed def 500 -> weapons/models/knife/knife_bayonet/weapon_knife_bayonet.vmdl).
struct KnifeModel { int def; const char* model; };
const KnifeModel kKnifeModels[] = {
	{ 42,  "weapons/models/knife/knife_default_ct/weapon_knife_default_ct.vmdl" },
	{ 59,  "weapons/models/knife/knife_default_t/weapon_knife_default_t.vmdl" },
	{ 500, "weapons/models/knife/knife_bayonet/weapon_knife_bayonet.vmdl" },
	{ 503, "weapons/models/knife/knife_css/weapon_knife_css.vmdl" },
	{ 505, "weapons/models/knife/knife_flip/weapon_knife_flip.vmdl" },
	{ 506, "weapons/models/knife/knife_gut/weapon_knife_gut.vmdl" },
	{ 507, "weapons/models/knife/knife_karambit/weapon_knife_karambit.vmdl" },
	{ 508, "weapons/models/knife/knife_m9/weapon_knife_m9.vmdl" },
	{ 509, "weapons/models/knife/knife_tactical/weapon_knife_tactical.vmdl" },
	{ 512, "weapons/models/knife/knife_falchion/weapon_knife_falchion.vmdl" },
	{ 514, "weapons/models/knife/knife_bowie/weapon_knife_bowie.vmdl" },
	{ 515, "weapons/models/knife/knife_butterfly/weapon_knife_butterfly.vmdl" },
	{ 516, "weapons/models/knife/knife_push/weapon_knife_push.vmdl" },
	{ 517, "weapons/models/knife/knife_cord/weapon_knife_cord.vmdl" },
	{ 518, "weapons/models/knife/knife_canis/weapon_knife_canis.vmdl" },
	{ 519, "weapons/models/knife/knife_ursus/weapon_knife_ursus.vmdl" },
	{ 520, "weapons/models/knife/knife_navaja/weapon_knife_navaja.vmdl" },
	{ 521, "weapons/models/knife/knife_outdoor/weapon_knife_outdoor.vmdl" },
	{ 522, "weapons/models/knife/knife_stiletto/weapon_knife_stiletto.vmdl" },
	{ 523, "weapons/models/knife/knife_talon/weapon_knife_talon.vmdl" },
	{ 525, "weapons/models/knife/knife_skeleton/weapon_knife_skeleton.vmdl" },
	{ 526, "weapons/models/knife/knife_kukri/weapon_knife_kukri.vmdl" },
};

const char* KnifeModelForDef(int def) {
	for (const KnifeModel& k : kKnifeModels)
		if (k.def == def)
			return k.model;
	return nullptr;
}

// ---- CUtlStringToken hash (CS2 == murmur2, seed 0x31415926, lowercased) -- ported from nerv ----------
uint32_t Murmur2(const void* key, int len, uint32_t seed) {
	const uint32_t m = 0x5bd1e995;
	const int r = 24;
	uint32_t h = seed ^ (uint32_t)len;
	const unsigned char* data = (const unsigned char*)key;
	while (len >= 4) {
		uint32_t k = *(const uint32_t*)data;
		k *= m; k ^= k >> r; k *= m;
		h *= m; h ^= k;
		data += 4; len -= 4;
	}
	switch (len) {
	case 3: h ^= data[2] << 16; // fallthrough
	case 2: h ^= data[1] << 8;  // fallthrough
	case 1: h ^= data[0]; h *= m;
	}
	h ^= h >> 13; h *= m; h ^= h >> 15;
	return h;
}

uint32_t StringTokenHash(const char* str) {
	if (!str) return 0;
	char buf[32];
	int n = 0;
	for (; str[n] && n < (int)sizeof(buf) - 1; ++n) {
		char c = str[n];
		buf[n] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}
	buf[n] = '\0';
	return Murmur2(buf, n, 0x31415926u);
}

// Resolve a CEntityInstance* by entity index (same guard pattern as CosmeticOverrideSystem.cpp).
void* EntFromIndex(int index) {
	if (index < 0 || index > GetHighestEntityIndex() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return g_GetEntityFromIndex(*g_pEntityList, index);
}

bool LooksLikeWeapon(void* ent) {
	if (!ent) return false;
	CEntityInstance* e = (CEntityInstance*)ent;
	const char* cn = e->GetClassName();
	const char* cc = e->GetClientClassName();
	return (cn && std::strstr(cn, "weapon_")) || (cn && std::strstr(cn, "Weapon")) || (cc && std::strstr(cc, "Weapon"));
}

bool IsSupportedModelPath(const char* model) {
	if (!model || !*model || std::strstr(model, ".."))
		return false;
	size_t len = std::strlen(model);
	if (len < 6 || 0 != _stricmp(model + len - 5, ".vmdl"))
		return false;
	return 0 == _strnicmp(model, "weapons/models/", 15)
		|| 0 == _strnicmp(model, "agents/models/", 14)
		|| 0 == _strnicmp(model, "characters/models/", 18);
}

bool IsSupportedPlayerModelPath(const char* model) {
	return IsSupportedModelPath(model)
		&& (0 == _strnicmp(model, "agents/models/", 14)
			|| 0 == _strnicmp(model, "characters/models/", 18));
}

unsigned char* HudArmsForPawn(unsigned char* pawn) {
	const ptrdiff_t offArms = g_clientDllOffsets.C_CSPlayerPawn.m_hHudModelArms;
	if (!pawn || offArms == 0)
		return nullptr;
	__try {
		uint32_t handle = *(uint32_t*)(pawn + offArms);
		if (handle == 0xFFFFFFFF)
			return nullptr;
		return (unsigned char*)EntFromIndex((int)(handle & 0x7FFF));
	} __except (1) {
		return nullptr;
	}
}

// SEH-guarded: read the target model path from a (def-already-set) econ item view via GetStaticData,
// validating the returned pointer/string. Returns true and fills out[] on success.
bool TryGetModelFromStaticData(unsigned char* itemView, char* out, size_t outSize) {
	if (!g_fns.getStaticData || !itemView)
		return false;
	__try {
		void* def = g_fns.getStaticData(itemView);
		if (!def)
			return false;
		const char* model = *(const char**)((unsigned char*)def + kEconItemDefModelNameOffset);
		if (!model || (uintptr_t)model < 0x10000)
			return false;
		// Validate the raw econ-data pointer before trusting it. Glove definitions legitimately use
		// agents/models/shared/arms/... resources, while weapons/knives use weapons/models/....
		if (!IsSupportedModelPath(model))
			return false;
		size_t i = 0;
		for (; model[i] && i < outSize - 1; ++i) out[i] = model[i];
		out[i] = '\0';
		return i > 0;
	} __except (1) {
		return false;
	}
}

// SEH-guarded blocking precache of a model resource (approach #2). Runs on the swap thread (main) so the
// .vmdl + its per-model animation data are resident before SetModel and before the worker-thread anim pass
// poses the new model -- which is the null-table crash this fixes. No-op if the resource system is missing.
bool SafePrecacheModel(const char* model) {
	if (!g_precacheOn || !model || !*model || !g_pCResourceSystem)
		return false;
	__try {
		g_pCResourceSystem->PreCache(model);
		++g_precacheCalls;
		return true;
	} __except (1) {
		return false;
	}
}

// SEH-guarded SetModel on an entity (weapon world model, viewmodel, or pawn).
bool SafeSetModel(unsigned char* entity, const char* model) {
	if (!g_fns.setModel || !entity || !model || !*model)
		return false;
	__try {
		g_fns.setModel(entity, model);
		return true;
	} __except (1) {
		return false;
	}
}

// SEH-guarded SetMeshGroupMask on an entity's scene node.
bool SafeSetMeshMask(unsigned char* entity, uint64_t mask) {
	const ptrdiff_t offNode = g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode;
	if (!g_fns.setMeshGroupMask || !entity || offNode == 0 || mask == 0)
		return false;
	__try {
		void* node = *(void**)(entity + offNode);
		if (!node)
			return false;
		g_fns.setMeshGroupMask(node, mask);
		return true;
	} __except (1) {
		return false;
	}
}

// SEH-guarded CGameSceneNode::PostDataUpdate (vtable index 22) on an entity's scene node. This is the
// renderable-refresh Andromeda fires after SetModel/SetMeshGroupMask/UpdateSkin; without it a model or
// mesh swap written while a demo is PAUSED sticks in memory but is not re-evaluated by the renderer.
bool SafePostDataUpdate(unsigned char* entity) {
	const ptrdiff_t offNode = g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode;
	if (!entity || offNode == 0)
		return false;
	__try {
		void* node = *(void**)(entity + offNode);
		if (!node || (uintptr_t)node < 0x10000)
			return false;
		void** vt = *(void***)node;
		if (!vt)
			return false;
		void* fn = vt[kPostDataUpdateVtableIndex];
		if (!fn)
			return false;
		((PostDataUpdate_t)fn)(node, 0, 0);
		return true;
	} __except (1) {
		return false;
	}
}

// SEH-guarded subclass re-derive: write m_nSubclassID = token(defStr), then UpdateSubclass(weapon).
bool SafeUpdateSubclass(unsigned char* weapon, int targetDef) {
	const ptrdiff_t offSub = g_clientDllOffsets.C_BaseEntity.m_nSubclassID;
	if (!g_fns.updateSubclass || !weapon)
		return false;
	__try {
		if (offSub) {
			char buf[16];
			std::snprintf(buf, sizeof(buf), "%d", targetDef);
			*(uint32_t*)(weapon + offSub) = StringTokenHash(buf);
		}
		g_fns.updateSubclass(weapon);
		return true;
	} __except (1) {
		return false;
	}
}

// SEH-guarded glove field write + pawn/HUD-arms rebuild.
bool SafeApplyGloves(unsigned char* pawn, int gloveDef, int paintKit, float wear, int seed, uint32_t accountId) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	const ptrdiff_t offGloves = o.C_CSPlayerPawn.m_EconGloves;
	if (!pawn || offGloves == 0 || o.C_EconItemView.m_iItemDefinitionIndex == 0) {
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("cosmetics.glove", "ABORT pawn=%d offGloves=0x%llx defIdxOff=0x%llx (offsets missing)",
				pawn ? 1 : 0, (unsigned long long)offGloves,
				(unsigned long long)o.C_EconItemView.m_iItemDefinitionIndex);
		return false;
	}
	unsigned char* glove = pawn + offGloves; // embedded C_EconItemView
	char gloveModel[260] = {};
	// Diagnostics: POD counters filled inside the SEH block, logged after it (see "cosmetics.glove").
	int dAttrVec = 0, dAttrCount = 0, dPaintW = 0, dSeedW = 0, dWearW = 0, dFaulted = 0, dNamedSetter = 0;
	__try {
		*(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex) = (uint16_t)gloveDef;
		if (o.C_EconItemView.m_iEntityQuality)
			*(int32_t*)(glove + o.C_EconItemView.m_iEntityQuality) = 3; // QUALITY_UNUSUAL
		if (o.C_EconItemView.m_iAccountID)
			*(uint32_t*)(glove + o.C_EconItemView.m_iAccountID) = accountId;
		// Overwrite networked paint attributes (def 6=paint, 7=seed, 8=wear) IN PLACE if present.
		if (o.C_EconItemView.m_NetworkedDynamicAttributes && o.C_AttributeList.m_Attributes &&
			o.CEconItemAttribute.m_iAttributeDefinitionIndex && o.CEconItemAttribute.m_flValue &&
			o.CEconItemAttribute.m_size) {
			unsigned char* vec = glove + o.C_EconItemView.m_NetworkedDynamicAttributes + o.C_AttributeList.m_Attributes;
			int count = *(int*)vec;
			unsigned char* data = *(unsigned char**)(vec + 8);
			if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
				data = *(unsigned char**)vec;
				count = *(int*)(vec + 16);
			}
			if (count > 0 && count <= 128 && (uintptr_t)data > 0x10000) {
				dAttrVec = 1; dAttrCount = count;
				int stride = (int)o.CEconItemAttribute.m_size;
				for (int i = 0; i < count; ++i) {
					unsigned char* attr = data + (ptrdiff_t)i * stride;
					int def = (int)*(uint16_t*)(attr + o.CEconItemAttribute.m_iAttributeDefinitionIndex);
					float* pv = (float*)(attr + o.CEconItemAttribute.m_flValue);
					if (def == 6) { *pv = (float)paintKit; dPaintW = 1; }
					else if (def == 7) { *pv = (float)seed; dSeedW = 1; }
					else if (def == 8) { *pv = wear; dWearW = 1; }
				}
			}
		}
		// Missing-attribute fallback: a DEFAULT glove has no networked paint attribute to overwrite
		// (dPaintW stays 0 above), so paint can never be written by the vector path -- apply it through
		// the engine named-setter instead (the same path the weapon loop uses), which adds the attribute
		// to the item view if absent. Without this, default gloves could not be painted at all.
		if (paintKit > 0 && !dPaintW && g_fns.setAttributeValueByName) {
			g_fns.setAttributeValueByName(glove, "set item texture prefab", (float)paintKit);
			g_fns.setAttributeValueByName(glove, "set item texture wear", wear);
			g_fns.setAttributeValueByName(glove, "set item texture seed", (float)seed);
			dNamedSetter = 1;
		}
		if (o.C_EconItemView.m_bInitialized)
			*(bool*)(glove + o.C_EconItemView.m_bInitialized) = true;
		// Let the pawn choose the correct team/agent body groups from its econ glove. Forcing the
		// unrelated `first_or_third_person` group to choice 1 removed the third-person hands entirely.
		if (g_fns.updateBodyGroupChoice)
			g_fns.updateBodyGroupChoice(pawn);
		if (o.C_CSPlayerPawn.m_bNeedToReApplyGloves)
			*(bool*)(pawn + o.C_CSPlayerPawn.m_bNeedToReApplyGloves) = true;
	} __except (1) {
		dFaulted = 1;
	}
	bool haveGloveModel = TryGetModelFromStaticData(glove, gloveModel, sizeof(gloveModel));
	// Refresh the pawn renderable so the new glove body group renders without waiting for a live sim
	// frame (essential during a paused demo). Separate SEH scope from the field writes above.
	SafePostDataUpdate(pawn);
	unsigned char* arms = HudArmsForPawn(pawn);
	bool armsResolved = arms != nullptr;
	if (arms) {
		if (haveGloveModel)
			SafeSetModel(arms, gloveModel);
		SafePostDataUpdate(arms);
	}
	// Diagnostic line (deduped per unique payload). Tells which sub-step broke: dAttrVec=0 -> the demo
	// glove has NO networked attribute vector (paint can't be written -> default skin); bodyGroupFn=0
	// -> UpdateBodyGroupChoice unresolved; arms=0 -> first-person glove model not refreshed; haveModel=0
	// -> glove def's model path didn't resolve.
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("cosmetics.glove",
			"def=%d paint=%d seed=%d wear=%.3f faulted=%d attrVec=%d attrCount=%d wrote(p=%d s=%d w=%d) "
			"namedSetter=%d bodyGroupFn=%d needReApplyOff=%d initOff=%d haveModel=%d arms=%d model='%s'",
			gloveDef, paintKit, seed, wear, dFaulted, dAttrVec, dAttrCount, dPaintW, dSeedW, dWearW,
			dNamedSetter, g_fns.updateBodyGroupChoice ? 1 : 0,
			o.C_CSPlayerPawn.m_bNeedToReApplyGloves ? 1 : 0,
			o.C_EconItemView.m_bInitialized ? 1 : 0,
			haveGloveModel ? 1 : 0, armsResolved ? 1 : 0, haveGloveModel ? gloveModel : "");
	return dFaulted ? false : true;
}

// SEH-guarded econ-schema paint-kit legacy lookup. Returns 1 legacy / 0 modern / -1 unknown.
int SafePaintKitLegacy(int paintKitId) {
	if (!g_fns.getEconItemSystem || paintKitId <= 0)
		return -1;
	__try {
		void* sys = g_fns.getEconItemSystem(nullptr); // static accessor; ignores `this`
		if (!sys || (uintptr_t)sys < 0x10000)
			return -1;
		void* schema = *(void**)((unsigned char*)sys + kEconItemSystem_SchemaOffset);
		if (!schema || (uintptr_t)schema < 0x10000)
			return -1;
		unsigned char* map = (unsigned char*)schema + kEconItemSchema_PaintKitsOffset;
		int count = *(int*)map;                         // CUtlMap::m_count
		unsigned char* elems = *(unsigned char**)(map + 8); // CUtlMap::m_elements
		if (count <= 0 || count > 100000 || (uintptr_t)elems < 0x10000)
			return -1;
		// node_t (32 bytes): {int left, int right, pad, pad, int key@16, pad, void* value@24}
		for (int i = 0; i < count; ++i) {
			unsigned char* node = elems + (ptrdiff_t)i * 32;
			int key = *(int*)(node + 16);
			if (key != paintKitId)
				continue;
			void* pk = *(void**)(node + 24);
			if (!pk || (uintptr_t)pk < 0x10000)
				return -1;
			return *(uint8_t*)((unsigned char*)pk + kPaintKit_IsUseLegacyModelOffset) ? 1 : 0;
		}
		return -1;
	} __except (1) {
		return -1;
	}
}

// Walk the pawn's HUD-model-arms scene-node children and mirror a model/mesh write onto the
// FIRST-PERSON viewmodel of the weapon we just changed. CRITICAL: the children under the arms are
// EVERY viewmodel weapon parented to this pawn (knife, pistol, rifle, ...), so a model/mesh write may
// ONLY touch a child whose weapon CLASS matches worldWeapon's class. Blindly SetModel-ing all of them
// to (e.g.) the knife model corrupts the other weapons' viewmodels and CRASHES the moment one of them
// is next deployed -- exactly the "switch to the AK after a knife-type swap" crash: the AK viewmodel
// got the knife model, then AK deploy animations ran on a knife model. worldWeapon == null means "no
// model/mesh change, just refresh the renderables" (PostDataUpdate). Class equality (not the old
// m_hOwnerEntity owner-match, which is wrong on some builds) is the safe discriminator: the world
// weapon and its viewmodel share the same entity class (weapon_knife, weapon_ak47, ...). SEH-guarded.
void RefreshViewmodelWeapons(unsigned char* pawn, const char* model, uint64_t meshMask,
	unsigned char* worldWeapon, int entityIndex = -1) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	const bool trace = MvmDebugLog_Active();
	if (!pawn) return;
	if (o.C_CSPlayerPawn.m_hHudModelArms == 0 || o.C_BaseEntity.m_pGameSceneNode == 0 ||
		o.CGameSceneNode.m_pChild == 0 || o.CGameSceneNode.m_pNextSibling == 0 ||
		o.CGameSceneNode.m_pOwner == 0)
		return;

	unsigned char* arms = HudArmsForPawn(pawn);
	if (!arms) return;

	// Class of the weapon we are mirroring onto its own first-person viewmodel. Only same-class
	// children receive the model/mesh write; every other weapon's viewmodel is left untouched.
	const char* wantClass = nullptr;
	if (worldWeapon) {
		__try { wantClass = ((CEntityInstance*)worldWeapon)->GetClassName(); }
		__except (1) { wantClass = nullptr; }
	}
	if (trace)
		MvmDebugLog_LinefAlways("knife.vm", "walk.begin idx=%d arms=%p wantClass='%s' model='%s' mask=%llu",
			entityIndex, (void*)arms, wantClass ? wantClass : "?", model ? model : "", (unsigned long long)meshMask);

	// Per-child breadcrumbs are written + flushed BEFORE each renderable is touched, so if the crash is
	// in a half-built viewmodel during a switch, the LAST "knife.vm child" line names the exact child
	// (its owner ptr + class + whether it matched and got the model write) we touched before dying.
	__try {
		void* armsNode = *(void**)(arms + o.C_BaseEntity.m_pGameSceneNode);
		if (!armsNode) return;
		void* child = *(void**)((unsigned char*)armsNode + o.CGameSceneNode.m_pChild);
		for (int guard = 0; child && guard < 64; ++guard) {
			void* owner = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pOwner);
			if (owner && LooksLikeWeapon(owner)) {
				bool sameWeapon = false;
				const char* oc = nullptr;
				if (wantClass) {
					oc = ((CEntityInstance*)owner)->GetClassName();
					sameWeapon = oc && 0 == std::strcmp(oc, wantClass);
				}
				if (trace)
					MvmDebugLog_LinefAlways("knife.vm", "child idx=%d n=%d owner=%p cls='%s' same=%d willWriteModel=%d",
						entityIndex, guard, owner, oc ? oc : "?", sameWeapon ? 1 : 0,
						(sameWeapon && model && *model) ? 1 : 0);
				if (sameWeapon) {
					if (model && *model)
						SafeSetModel((unsigned char*)owner, model);
					if (meshMask)
						SafeSetMeshMask((unsigned char*)owner, meshMask);
					// Approach #1 on the first-person viewmodel child we just re-modeled (the exact entity
					// the overlay-research reference applies it to).
					ResetAnimGraph((unsigned char*)owner, entityIndex, "vm");
				}
				SafePostDataUpdate((unsigned char*)owner);
			}
			child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
		}
	} __except (1) {
		if (trace) MvmDebugLog_LinefAlways("knife.vm", "walk.FAULT idx=%d (SEH caught during child walk)", entityIndex);
		return;
	}
	SafePostDataUpdate(arms);
	if (trace) MvmDebugLog_LinefAlways("knife.vm", "walk.end idx=%d", entityIndex);
}

} // namespace

const ModelSwapResolveStatus& ResolveModelSwapFns() {
	if (g_status.attempted)
		return g_status;
	g_status.attempted = true;

	g_fns.setModel = (SetModel_t)FindClientPattern(
		"40 53 48 83 EC ?? 48 8B D9 4C 8B C2 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 40");
	g_fns.setMeshGroupMask = (SetMeshGroupMask_t)FindClientPattern(
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8D 99 ?? ?? ?? ?? 48 8B 71");
	g_fns.updateSubclass = (UpdateSubclass_t)FindClientPattern(
		"4C 8B DC 53 48 81 EC ?? ?? ?? ?? 48 8B 41");
	g_fns.setBodyGroup = (SetBodyGroup_t)ResolveRelCall(FindClientPattern(
		"E8 ?? ?? ?? ?? EB 0C 48 8B CF"));
	g_fns.updateBodyGroupChoice = (UpdateBodyGroupChoice_t)ResolveRelCall(FindClientPattern(
		"E8 ?? ?? ?? ?? 4C 8B AC 24 ?? ?? ?? ?? 48 8B BC 24"));
	g_fns.getStaticData = (GetStaticData_t)FindClientPattern(
		"40 56 48 83 EC ?? 48 89 5C 24 ?? 48 8B F1 48 8B 1D ?? ?? ?? ?? 48 85 DB 75");
	g_fns.getEconItemSystem = (GetEconItemSystem_t)FindClientPattern(
		"48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 85 81");
	g_fns.setAttributeValueByName = (SetAttributeValueByName_t)ResolveRelCall(FindClientPattern(
		"E8 ?? ?? ?? ?? 66 41 0F 6E D4"));

	g_status.setModel = g_fns.setModel != nullptr;
	g_status.setMeshGroupMask = g_fns.setMeshGroupMask != nullptr;
	g_status.updateSubclass = g_fns.updateSubclass != nullptr;
	g_status.setBodyGroup = g_fns.setBodyGroup != nullptr;
	g_status.updateBodyGroupChoice = g_fns.updateBodyGroupChoice != nullptr;
	g_status.getStaticData = g_fns.getStaticData != nullptr;
	g_status.econItemSystem = g_fns.getEconItemSystem != nullptr;
	return g_status;
}

bool IsValidAgentModelPath(const char* modelPath) {
	return IsSupportedPlayerModelPath(modelPath);
}

void SetAnimGraphReset(bool enabled) { g_animResetOn = enabled; }
bool AnimGraphReset() { return g_animResetOn; }
void SetAnimGraphResetOffsets(uint32_t instOff, uint32_t varsOff) { g_animInstOff = instOff; g_animVarsOff = varsOff; }
void GetAnimGraphResetOffsets(uint32_t* instOff, uint32_t* varsOff) {
	if (instOff) *instOff = g_animInstOff;
	if (varsOff) *varsOff = g_animVarsOff;
}

bool PrecacheModelResource(const char* modelPath) { ResolveModelSwapFns(); return SafePrecacheModel(modelPath); }
void SetPrecacheModels(bool enabled) { g_precacheOn = enabled; }
bool PrecacheModels() { return g_precacheOn; }

int PaintKitLegacyModel(int paintKitId) {
	ResolveModelSwapFns();
	return SafePaintKitLegacy(paintKitId);
}

uint64_t ResolveMeshMask(int paintKitId, bool knife, int legacyOverride,
	uint64_t maskModern, uint64_t maskLegacy) {
	int legacy;
	if (legacyOverride == -1) legacy = 0;
	else if (legacyOverride == 1) legacy = 1;
	else legacy = PaintKitLegacyModel(paintKitId); // -2 auto

	if (legacy < 0)
		return 0; // unknown -> leave the mesh untouched

	// Knife polarity differs between the two reference cheats (Andromeda: legacy=2/modern=1; nerv:
	// legacy=1/modern=2). We follow Andromeda's polarity for both weapons and knives (legacy ->
	// maskLegacy, modern -> maskModern) and expose maskModern/maskLegacy as tunables for A/B.
	(void)knife;
	return legacy ? maskLegacy : maskModern;
}

bool ApplyKnifeModelSwap(unsigned char* weapon, unsigned char* itemView,
	unsigned char* pawnForViewmodel, int targetDef, uint64_t meshMask, int entityIndex) {
	ResolveModelSwapFns();
	// Approach #3 (the working fix): make sure the anim-builder detour is installed before we SetModel the
	// world weapon to a possibly-unloaded knife model, so the engine's later anim pass can't null-deref.
	EnsureAnimCrashFixInstalled();
	// Always-flushed step breadcrumbs (only when mvm_debug is running). The point: the knife type swap
	// rebuilds the weapon's animation set, and re-firing it onto an entity the engine recreated during a
	// quick weapon switch faults -- often inside one of these native calls, or in the engine's NEXT
	// animation frame. Each step is written + flushed BEFORE the call, so the LAST "knife.swap" line in
	// the log names exactly which call/state preceded the crash (an unmatched "...begin" = it faulted in
	// that call; a clean "END" followed by the crash = it faulted later in the engine's deploy/anim pass).
	const bool trace = MvmDebugLog_Active();
	if (!g_status.CoreOk() || !weapon || targetDef <= 0) {
		if (trace)
			MvmDebugLog_LinefAlways("knife.swap", "ABORT idx=%d coreOk=%d weapon=%d targetDef=%d (functions unresolved or bad args -> no swap)",
				entityIndex, g_status.CoreOk() ? 1 : 0, weapon ? 1 : 0, targetDef);
		return false;
	}

	// Resolve the target model: prefer the live econ definition (GetStaticData on the def-already-set
	// item view), fall back to the built-in knife table.
	char model[260];
	bool haveModel = TryGetModelFromStaticData(itemView, model, sizeof(model));
	if (!haveModel) {
		const char* tbl = KnifeModelForDef(targetDef);
		if (!tbl) {
			if (trace)
				MvmDebugLog_LinefAlways("knife.swap", "ABORT idx=%d targetDef=%d (no model from econ and no table entry -> no swap)",
					entityIndex, targetDef);
			return false;
		}
		std::snprintf(model, sizeof(model), "%s", tbl);
	}

	// Open the crash-watch window so the vectored handler records any access violation in the next few
	// seconds (the post-swap animation/render frames where these crashes land) with its module+offset.
	if (trace) {
		MvmCrashWatch_Arm(entityIndex, model);
		MvmDebugLog_LinefAlways("knife.swap", "BEGIN idx=%d weapon=%p hasViewmodelPawn=%d targetDef=%d meshMask=%llu haveModel=%d model='%s'",
			entityIndex, (void*)weapon, pawnForViewmodel ? 1 : 0, targetDef, (unsigned long long)meshMask, haveModel ? 1 : 0, model);
	}

	// Approach #2 (root-cause fix): blocking-load the target model + its anim data BEFORE SetModel, so the
	// engine's later async anim pass finds a non-null per-model table instead of crashing (see header / §11).
	if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=precache.begin idx=%d on=%d resSys=%d model='%s'",
		entityIndex, g_precacheOn ? 1 : 0, g_pCResourceSystem ? 1 : 0, model);
	bool precached = SafePrecacheModel(model);
	if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=precache.end idx=%d fired=%d totalCalls=%llu",
		entityIndex, precached ? 1 : 0, (unsigned long long)g_precacheCalls);

	if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=setModel.begin idx=%d model='%s'", entityIndex, model);
	bool any = SafeSetModel(weapon, model);
	if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=setModel.end idx=%d ok=%d", entityIndex, any ? 1 : 0);

	if (meshMask) {
		if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=meshMask.begin idx=%d mask=%llu", entityIndex, (unsigned long long)meshMask);
		SafeSetMeshMask(weapon, meshMask);
		if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=meshMask.end idx=%d", entityIndex);
	}

	if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=subclass.begin idx=%d def=%d", entityIndex, targetDef);
	SafeUpdateSubclass(weapon, targetDef);
	if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=subclass.end idx=%d", entityIndex);

	// Approach #1: reset the WORLD weapon's animgraph so the engine rebuilds it for the new model rather
	// than posing it with stale per-model data (the worker-thread null-deref this whole investigation chased).
	ResetAnimGraph(weapon, entityIndex, "world");

	if (pawnForViewmodel) {
		// PRIME crash suspect: this walks the pawn's HUD-arms children (every weapon's first-person
		// viewmodel) and writes the knife model onto the matching one. During a knife->AK switch the AK's
		// viewmodel is mid-deploy; see RefreshViewmodelWeapons for the per-child breadcrumbs.
		if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=viewmodel.begin idx=%d (mirror onto first-person viewmodel)", entityIndex);
		RefreshViewmodelWeapons(pawnForViewmodel, model, meshMask, weapon, entityIndex);
		if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=viewmodel.end idx=%d", entityIndex);
	}

	// Force the renderable to adopt the new model + mesh group now (shows while paused).
	if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=postData.begin idx=%d", entityIndex);
	SafePostDataUpdate(weapon);
	if (trace) MvmDebugLog_LinefAlways("knife.swap", "END idx=%d ok=%d", entityIndex, any ? 1 : 0);
	return any;
}

void ApplyWeaponMeshMask(unsigned char* weaponEntity, uint64_t meshMask, unsigned char* pawnForViewmodel) {
	ResolveModelSwapFns();
	if (meshMask == 0)
		return;
	SafeSetMeshMask(weaponEntity, meshMask);
	// Force the renderable to re-evaluate the mesh group now (legacy<->CS2 switch shows paused).
	SafePostDataUpdate(weaponEntity);
	if (pawnForViewmodel)
		RefreshViewmodelWeapons(pawnForViewmodel, nullptr, meshMask, weaponEntity);
}

void RefreshWeaponViewmodel(unsigned char* pawn) {
	ResolveModelSwapFns();
	RefreshViewmodelWeapons(pawn, nullptr, 0, nullptr);
}

bool ApplyAgentModel(unsigned char* pawn, const char* modelPath) {
	ResolveModelSwapFns();
	bool setModelFn = g_status.setModel;
	bool pathValid = IsSupportedPlayerModelPath(modelPath);
	bool ok = false;
	if (setModelFn && pawn && pathValid) {
		SafePrecacheModel(modelPath); // approach #2: blocking-load the agent model before SetModel (same risk as knives)
		ok = SafeSetModel(pawn, modelPath);
		if (ok) SafePostDataUpdate(pawn); // refresh the pawn renderable so the agent model shows now
	}
	// Diagnostic line (deduped). setModelFn=0 -> SetModel signature unresolved; pathValid=0 -> the model
	// path was rejected (not an agents//characters/ player model); setModelOk=0 -> SetModel faulted.
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("cosmetics.agent", "setModelFn=%d pawn=%d pathValid=%d setModelOk=%d model='%s'",
			setModelFn ? 1 : 0, pawn ? 1 : 0, pathValid ? 1 : 0, ok ? 1 : 0, modelPath ? modelPath : "");
	return ok;
}

bool PostDataUpdate(unsigned char* entity) {
	return SafePostDataUpdate(entity);
}

bool ApplyGloveModel(unsigned char* pawn, int gloveDef, int paintKit, float wear, int seed,
	uint32_t accountId) {
	ResolveModelSwapFns();
	if (!g_status.GlovesOk() || !pawn || gloveDef <= 0)
		return false;
	return SafeApplyGloves(pawn, gloveDef, paintKit, wear, seed, accountId);
}

} // namespace Filmmaker
