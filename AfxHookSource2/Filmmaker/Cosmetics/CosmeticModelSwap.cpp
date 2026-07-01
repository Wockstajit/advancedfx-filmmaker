#include "CosmeticModelSwap.h"

#include "../../ClientEntitySystem.h"   // entity-list globals, g_GetEntityFromIndex
#include "../../SchemaSystem.h"          // g_clientDllOffsets
#include "../../SceneSystem.h"           // CResourceSystem / g_pCResourceSystem (blocking resource precache)
#include "../../../deps/release/prop/cs2/sdk_src/public/entityhandle.h"
#include "../../../shared/binutils.h"
#include "CosmeticDebugLog.h"           // MvmDebugLog_Active / MvmDebugLog_Linef (diagnostics)
#include "CosmeticDirectComposite.h"    // FireDirectCompositeRefresh / FireNamedSkinAttributes
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
typedef void (__fastcall* SetBodyGroupNumeric_t)(void* entity, uint64_t group, uint64_t value);
typedef void (__fastcall* RegenerateSkins_t)(void);
typedef void (__fastcall* UpdateBodyGroupChoice_t)(void* entity);
typedef void* (__fastcall* GetStaticData_t)(void* econItemView);
typedef void* (__fastcall* GetEconItemSystem_t)(void* unused);
// C_EconItemView::SetAttributeValueByName(view, name, float) -- the engine named-setter used to ADD a
// paint attribute to an item that has none (the default-glove / default-weapon attrWritten=0 case).
typedef void (__fastcall* SetAttributeValueByName_t)(void* itemView, const char* name, float value);
typedef void (__fastcall* ConstructPaintKit_t)(void* itemView);
typedef void* (__fastcall* SchemaGetItemDefByIndex_t)(void* schema, int defIndex);
// CGameSceneNode::PostDataUpdate(this, 0, 0) -- vtable index 22 (Andromeda SDK::VMT_Index, current
// build). Forces the renderable to re-derive its model/mesh/body-group, so a swap shows while paused.
typedef void (__fastcall* PostDataUpdate_t)(void* sceneNode, int a, int b);
constexpr int kPostDataUpdateVtableIndex = 22;

struct Fns {
	SetModel_t setModel = nullptr;
	SetMeshGroupMask_t setMeshGroupMask = nullptr;
	UpdateSubclass_t updateSubclass = nullptr;
	SetBodyGroup_t setBodyGroup = nullptr;
	SetBodyGroupNumeric_t setBodyGroupNumeric = nullptr;
	RegenerateSkins_t regenerateSkins = nullptr;
	UpdateBodyGroupChoice_t updateBodyGroupChoice = nullptr;
	GetStaticData_t getStaticData = nullptr;
	GetEconItemSystem_t getEconItemSystem = nullptr;
	SetAttributeValueByName_t setAttributeValueByName = nullptr;
	ConstructPaintKit_t constructPaintKit = nullptr;
	SchemaGetItemDefByIndex_t getItemDefByIndex = nullptr;
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
// CEconItemSchema item-definition CUtlMap offsets to try (build varies; first hit is cached).
static const ptrdiff_t kItemDefMapOffsets[] = { 0x128, 0x120, 0x130, 0x248, 0x250, 0x258 };
static ptrdiff_t g_itemDefMapOffset = 0;

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

void ReadEntityModelPath(CEntityInstance* ent, char* out, size_t outSize) {
	if (out && outSize) out[0] = '\0';
	if (!ent || !out || outSize == 0)
		return;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.ModelChain.m_CBodyComponent == 0 || o.ModelChain.m_skeletonInstance == 0
		|| o.ModelChain.m_modelState == 0 || o.ModelChain.m_ModelName == 0)
		return;
	unsigned char* p = (unsigned char*)ent;
	__try {
		unsigned char* bodyComp = *(unsigned char**)(p + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)bodyComp <= 0x10000) return;
		unsigned char* modelState = bodyComp + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
		const char* name = *(const char**)(modelState + o.ModelChain.m_ModelName);
		if ((uintptr_t)name <= 0x10000) return;
		size_t i = 0;
		for (; name[i] && i + 1 < outSize; ++i) out[i] = name[i];
		out[i] = '\0';
	} __except (1) {
		if (outSize) out[0] = '\0';
	}
}

bool SameModelPath(const char* a, const char* b) {
	return a && *a && b && *b && 0 == _stricmp(a, b);
}

void ResolveWeaponMatchModelPath(unsigned char* worldWeapon, const char* explicitModel, char* out, size_t outSize) {
	if (out && outSize) out[0] = '\0';
	if (!out || outSize == 0)
		return;
	if (explicitModel && *explicitModel) {
		size_t i = 0;
		for (; explicitModel[i] && i + 1 < outSize; ++i) out[i] = explicitModel[i];
		out[i] = '\0';
		return;
	}
	if (!worldWeapon)
		return;
	ReadEntityModelPath((CEntityInstance*)worldWeapon, out, outSize);
}

bool ViewmodelChildMatchesWeapon(const char* wantClass, const char* wantModel, const char* childClass, const char* childModel) {
	if (wantClass && childClass && 0 == std::strcmp(childClass, wantClass))
		return true;
	return SameModelPath(wantModel, childModel);
}

static bool HudArmsOwnedByPawnIndex(unsigned char* arms, int pawnIdx) {
	if (!arms || pawnIdx <= 0)
		return false;
	__try {
		CEntityInstance* ent = (CEntityInstance*)arms;
		const char* cls = ent->GetClassName();
		if (!cls || std::strstr(cls, "HudModelArms") == nullptr)
			return false;
		SOURCESDK::CS2::CBaseHandle owner = ent->GetOwnerEntityHandle();
		return owner.IsValid() && owner.GetEntryIndex() == pawnIdx;
	} __except (1) {
		return false;
	}
}

unsigned char* HudArmsForPawn(unsigned char* pawn) {
	const ptrdiff_t offArms = g_clientDllOffsets.C_CSPlayerPawn.m_hHudModelArms;
	if (!pawn || offArms == 0)
		return nullptr;
	CEntityInstance* pawnInst = (CEntityInstance*)pawn;
	SOURCESDK::CS2::CBaseHandle pawnHandle = pawnInst->GetHandle();
	const int pawnIdx = pawnHandle.IsValid() ? pawnHandle.GetEntryIndex() : -1;
	if (pawnIdx <= 0)
		return nullptr;
	__try {
		uint32_t raw = *(uint32_t*)(pawn + offArms);
		SOURCESDK::CS2::CBaseHandle h(raw);
		if (h.IsValid()) {
			const int armsIdx = h.GetEntryIndex();
			if (armsIdx > 0) {
				unsigned char* ent = (unsigned char*)EntFromIndex(armsIdx);
				if (ent && HudArmsOwnedByPawnIndex(ent, pawnIdx))
					return ent;
			}
		}
	} __except (1) {
	}
	// Demo/spectate: m_hHudModelArms can be stale while the arms entity still exists in the list.
	const int highest = GetHighestEntityIndex();
	for (int i = 1; i <= highest; ++i) {
		CEntityInstance* ent = (CEntityInstance*)EntFromIndex(i);
		if (!ent)
			continue;
		const char* cls = ent->GetClassName();
		if (!cls || std::strstr(cls, "HudModelArms") == nullptr)
			continue;
		if (HudArmsOwnedByPawnIndex((unsigned char*)ent, pawnIdx))
			return (unsigned char*)ent;
	}
	return nullptr;
}

bool HudArmsContainsWeaponClass(unsigned char* arms, const char* wantClass) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!arms || !wantClass || !*wantClass)
		return false;
	if (o.C_BaseEntity.m_pGameSceneNode == 0 || o.CGameSceneNode.m_pChild == 0
		|| o.CGameSceneNode.m_pNextSibling == 0 || o.CGameSceneNode.m_pOwner == 0)
		return false;
	__try {
		void* armsNode = *(void**)(arms + o.C_BaseEntity.m_pGameSceneNode);
		if (!armsNode)
			return false;
		void* child = *(void**)((unsigned char*)armsNode + o.CGameSceneNode.m_pChild);
		for (int guard = 0; child && guard < 64; ++guard) {
			void* owner = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pOwner);
			if (owner && LooksLikeWeapon(owner)) {
				const char* oc = ((CEntityInstance*)owner)->GetClassName();
				if (oc && 0 == std::strcmp(oc, wantClass))
					return true;
			}
			child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
		}
	} __except (1) {
	}
	return false;
}

unsigned char* HudArmsFromPawnSceneChildren(unsigned char* pawn) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!pawn || o.C_BaseEntity.m_pGameSceneNode == 0 || o.CGameSceneNode.m_pChild == 0
		|| o.CGameSceneNode.m_pNextSibling == 0 || o.CGameSceneNode.m_pOwner == 0)
		return nullptr;
	__try {
		void* pawnNode = *(void**)(pawn + o.C_BaseEntity.m_pGameSceneNode);
		if (!pawnNode)
			return nullptr;
		void* child = *(void**)((unsigned char*)pawnNode + o.CGameSceneNode.m_pChild);
		for (int guard = 0; child && guard < 64; ++guard) {
			void* owner = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pOwner);
			if (owner) {
				const char* cls = ((CEntityInstance*)owner)->GetClassName();
				const char* ccls = ((CEntityInstance*)owner)->GetClientClassName();
				if ((cls && std::strstr(cls, "hudmodel_arms") != nullptr)
					|| (cls && std::strstr(cls, "HudModelArms") != nullptr)
					|| (ccls && std::strstr(ccls, "HudModelArms") != nullptr)) {
					return (unsigned char*)owner;
				}
			}
			child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
		}
	} __except (1) {
	}
	return nullptr;
}

struct HudArmsResolve {
	unsigned char* arms = nullptr;
	const char* source = "none";
	int armsIdx = -1;
	int hudArmsEntities = 0;
};

HudArmsResolve ResolveHudArmsForViewmodel(unsigned char* ownerPawn, const char* wantWeaponClass) {
	HudArmsResolve r;
	auto finish = [&](unsigned char* arms, const char* src) {
		if (!arms)
			return;
		r.arms = arms;
		r.source = src;
		__try {
			SOURCESDK::CS2::CBaseHandle ah = ((CEntityInstance*)arms)->GetHandle();
			if (ah.IsValid())
				r.armsIdx = ah.GetEntryIndex();
		} __except (1) {
			r.armsIdx = -1;
		}
	};

	if (ownerPawn) {
		unsigned char* arms = HudArmsForPawn(ownerPawn);
		if (arms)
			finish(arms, "ownerPawn");
	}
	if (!r.arms && ownerPawn) {
		unsigned char* arms = HudArmsFromPawnSceneChildren(ownerPawn);
		if (arms)
			finish(arms, "ownerSceneChild");
	}
	if (!r.arms) {
		CEntityInstance* localPawn = AfxGetLocalViewerPawn();
		if (localPawn) {
			unsigned char* arms = HudArmsForPawn((unsigned char*)localPawn);
			if (arms)
				finish(arms, "localViewer");
			else {
				arms = HudArmsFromPawnSceneChildren((unsigned char*)localPawn);
				if (arms)
					finish(arms, "localSceneChild");
			}
		}
	}
	if (!r.arms && wantWeaponClass && *wantWeaponClass) {
		const int highest = GetHighestEntityIndex();
		for (int i = 1; i <= highest; ++i) {
			CEntityInstance* ent = (CEntityInstance*)EntFromIndex(i);
			if (!ent)
				continue;
			const char* cls = ent->GetClassName();
			if (!cls || std::strstr(cls, "HudModelArms") == nullptr)
				continue;
			++r.hudArmsEntities;
			if (HudArmsContainsWeaponClass((unsigned char*)ent, wantWeaponClass))
				finish((unsigned char*)ent, "entityScanClass");
		}
	}
	if (!r.arms) {
		const int highest = GetHighestEntityIndex();
		for (int i = 1; i <= highest; ++i) {
			CEntityInstance* ent = (CEntityInstance*)EntFromIndex(i);
			if (!ent)
				continue;
			const char* cls = ent->GetClassName();
			if (!cls || std::strstr(cls, "HudModelArms") == nullptr)
				continue;
			finish((unsigned char*)ent, "entityScanAny");
			break;
		}
	}
	return r;
}

// #region agent log
bool IsLikelyViewmodelClassText(const char* text) {
	return text && (
		std::strstr(text, "ViewModel") != nullptr ||
		std::strstr(text, "viewmodel") != nullptr ||
		std::strstr(text, "HudModel") != nullptr ||
		std::strstr(text, "Arms") != nullptr
	);
}

void AppendSceneChildProbe(char* out, size_t outSize, int& used, const char* tag, unsigned char* entity) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!out || outSize == 0 || !entity || used >= (int)outSize)
		return;
	if (o.C_BaseEntity.m_pGameSceneNode == 0 || o.CGameSceneNode.m_pChild == 0
		|| o.CGameSceneNode.m_pNextSibling == 0 || o.CGameSceneNode.m_pOwner == 0)
		return;
	__try {
		void* node = *(void**)(entity + o.C_BaseEntity.m_pGameSceneNode);
		if (!node)
			return;
		void* child = *(void**)((unsigned char*)node + o.CGameSceneNode.m_pChild);
		int count = 0;
		for (; child && count < 8 && used + 180 < (int)outSize; ++count) {
			void* owner = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pOwner);
			const char* cls = owner ? ((CEntityInstance*)owner)->GetClassName() : "?";
			const char* ccls = owner ? ((CEntityInstance*)owner)->GetClientClassName() : "?";
			int idx = -1;
			if (owner) {
				SOURCESDK::CS2::CBaseHandle h = ((CEntityInstance*)owner)->GetHandle();
				if (h.IsValid())
					idx = h.GetEntryIndex();
			}
			used += std::snprintf(out + used, outSize - (size_t)used,
				" | %sChild[%d]=%d cls='%s' ccls='%s'",
				tag ? tag : "child", count, idx, cls ? cls : "?", ccls ? ccls : "?");
			child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
		}
	} __except (1) {
	}
}

void BuildViewmodelProbe(unsigned char* ownerPawn, const char* wantWeaponClass, int worldEntityIndex,
	char* out, size_t outSize) {
	if (!out || outSize == 0)
		return;
	int used = 0;
	out[0] = '\0';

	int ownerPawnIdx = -1;
	if (ownerPawn) {
		__try {
			SOURCESDK::CS2::CBaseHandle ph = ((CEntityInstance*)ownerPawn)->GetHandle();
			if (ph.IsValid())
				ownerPawnIdx = ph.GetEntryIndex();
		} __except (1) {
			ownerPawnIdx = -1;
		}
	}

	int obsTargetIdx = -1;
	const int obsMode = (int)AfxGetLocalObserverState(&obsTargetIdx);
	CEntityInstance* localViewerPawn = AfxGetLocalViewerPawn();
	int localPawnIdx = -1;
	const char* localPawnClass = "?";
	int localActiveWeaponIdx = -1;
	if (localViewerPawn) {
		__try {
			SOURCESDK::CS2::CBaseHandle lph = localViewerPawn->GetHandle();
			if (lph.IsValid())
				localPawnIdx = lph.GetEntryIndex();
			localPawnClass = localViewerPawn->GetClassName();
			SOURCESDK::CS2::CBaseHandle aw = localViewerPawn->GetActiveWeaponHandle();
			if (aw.IsValid())
				localActiveWeaponIdx = aw.GetEntryIndex();
		} __except (1) {
			localPawnIdx = -1;
			localPawnClass = "?";
			localActiveWeaponIdx = -1;
		}
	}

	used += std::snprintf(out + used, outSize - (size_t)used,
		"VIEWPROBE worldIdx=%d wantClass='%s' ownerPawn=%d "
		"obsMode=%d obsTarget=%d localPawn=%d localClass='%s' localActive=%d",
		worldEntityIndex, wantWeaponClass ? wantWeaponClass : "?", ownerPawnIdx,
		obsMode, obsTargetIdx,
		localPawnIdx, localPawnClass ? localPawnClass : "?", localActiveWeaponIdx);

	AppendSceneChildProbe(out, outSize, used, "owner", ownerPawn);
	AppendSceneChildProbe(out, outSize, used, "local", (unsigned char*)localViewerPawn);

	int candidates = 0;
	const int highest = GetHighestEntityIndex();
	for (int i = 1; i <= highest && candidates < 8 && used + 160 < (int)outSize; ++i) {
		CEntityInstance* ent = (CEntityInstance*)EntFromIndex(i);
		if (!ent)
			continue;
		const char* cls = nullptr;
		const char* ccls = nullptr;
		int ownerIdx = -1;
		__try {
			cls = ent->GetClassName();
			ccls = ent->GetClientClassName();
			SOURCESDK::CS2::CBaseHandle oh = ent->GetOwnerEntityHandle();
			if (oh.IsValid())
				ownerIdx = oh.GetEntryIndex();
		} __except (1) {
			cls = nullptr;
			ccls = nullptr;
			ownerIdx = -1;
		}

		const bool interesting =
			IsLikelyViewmodelClassText(cls) ||
			IsLikelyViewmodelClassText(ccls) ||
			(wantWeaponClass && cls && 0 == std::strcmp(cls, wantWeaponClass)) ||
			(ownerPawnIdx > 0 && ownerIdx == ownerPawnIdx) ||
			(localPawnIdx > 0 && ownerIdx == localPawnIdx);
		if (!interesting)
			continue;

		used += std::snprintf(out + used, outSize - (size_t)used,
			" | cand[%d]=%d cls='%s' ccls='%s' owner=%d",
			candidates, i, cls ? cls : "?", ccls ? ccls : "?", ownerIdx);
		++candidates;
	}
}
// #endregion

bool CopyModelPath(const char* model, char* out, size_t outSize) {
	if (!model || !out || outSize == 0 || !IsSupportedModelPath(model))
		return false;
	size_t i = 0;
	for (; model[i] && i < outSize - 1; ++i) out[i] = model[i];
	out[i] = '\0';
	return i > 0;
}

// Walk CEconItemSchema CUtlMap<int, CEconItemDefinition*> (same node layout as paint kits) for defIndex.
bool TryGetModelFromDefIndex(int defIndex, char* out, size_t outSize, ptrdiff_t* outMapOff) {
	if (!g_fns.getEconItemSystem || defIndex <= 0)
		return false;
	__try {
		void* sys = g_fns.getEconItemSystem(nullptr);
		if (!sys || (uintptr_t)sys < 0x10000)
			return false;
		void* schema = *(void**)((unsigned char*)sys + kEconItemSystem_SchemaOffset);
		if (!schema || (uintptr_t)schema < 0x10000)
			return false;
		const ptrdiff_t* tryOffs = g_itemDefMapOffset ? &g_itemDefMapOffset : nullptr;
		for (int pass = 0; pass < (tryOffs ? 1 : (int)(sizeof(kItemDefMapOffsets) / sizeof(kItemDefMapOffsets[0]))); ++pass) {
			ptrdiff_t mapOff = tryOffs ? *tryOffs : kItemDefMapOffsets[pass];
			unsigned char* map = (unsigned char*)schema + mapOff;
			int count = *(int*)map;
			unsigned char* elems = *(unsigned char**)(map + 8);
			if (count <= 0 || count > 100000 || (uintptr_t)elems < 0x10000)
				continue;
			for (int i = 0; i < count; ++i) {
				unsigned char* node = elems + (ptrdiff_t)i * 32;
				int key = *(int*)(node + 16);
				if (key != defIndex)
					continue;
				void* def = *(void**)(node + 24);
				if (!def || (uintptr_t)def < 0x10000)
					continue;
				const char* model = *(const char**)((unsigned char*)def + kEconItemDefModelNameOffset);
				if (!CopyModelPath(model, out, outSize))
					continue;
				if (!g_itemDefMapOffset)
					g_itemDefMapOffset = mapOff;
				if (outMapOff)
					*outMapOff = mapOff;
				return true;
			}
		}
		if (g_fns.getItemDefByIndex) {
			void* def = g_fns.getItemDefByIndex(schema, defIndex);
			if (def && (uintptr_t)def > 0x10000) {
				const char* model = *(const char**)((unsigned char*)def + kEconItemDefModelNameOffset);
				if (CopyModelPath(model, out, outSize))
					return true;
			}
		}
	} __except (1) {
	}
	return false;
}

// SEH-guarded: read the target model path from a (def-already-set) econ item view via GetStaticData,
// validating the returned pointer/string. Returns true and fills out[] on success. failureReason (optional)
// is a short code for diagnostics: noFn, noDef, badModelPtr, badPath, seh.
bool TryGetModelFromStaticData(unsigned char* itemView, char* out, size_t outSize, const char** failureReason = nullptr) {
	auto fail = [&](const char* why) {
		if (failureReason) *failureReason = why;
		return false;
	};
	if (!itemView)
		return fail("noItemView");
	int defIndex = 0;
	__try {
		if (g_clientDllOffsets.C_EconItemView.m_iItemDefinitionIndex)
			defIndex = (int)*(uint16_t*)(itemView + g_clientDllOffsets.C_EconItemView.m_iItemDefinitionIndex);
	} __except (1) {
		defIndex = 0;
	}
	if (g_fns.getStaticData) {
		__try {
			void* def = g_fns.getStaticData(itemView);
			if (def) {
				const char* model = *(const char**)((unsigned char*)def + kEconItemDefModelNameOffset);
				if (model && (uintptr_t)model >= 0x10000 && IsSupportedModelPath(model)) {
					size_t i = 0;
					for (; model[i] && i < outSize - 1; ++i) out[i] = model[i];
					out[i] = '\0';
					if (i > 0) {
						if (failureReason) *failureReason = nullptr;
						return true;
					}
				}
			}
		} __except (1) {
		}
	}
	if (defIndex > 0 && TryGetModelFromDefIndex(defIndex, out, outSize, nullptr)) {
		if (failureReason) *failureReason = nullptr;
		return true;
	}
	if (!g_fns.getStaticData)
		return fail("noFn");
	return fail("noDef");
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

// Overwrite paint/seed/wear attrs in-place on one C_EconItemView attribute list (networked or local).
static void WriteGlovePaintAttrs(unsigned char* glove, ptrdiff_t listOff, int paintKit, float wear, int seed,
	int* paintW, int* seedW, int* wearW, int* attrVec, int* attrCount) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!glove || !listOff || !o.C_AttributeList.m_Attributes
		|| !o.CEconItemAttribute.m_iAttributeDefinitionIndex || !o.CEconItemAttribute.m_flValue
		|| !o.CEconItemAttribute.m_size)
		return;
	unsigned char* vec = glove + listOff + o.C_AttributeList.m_Attributes;
	int count = *(int*)vec;
	unsigned char* data = *(unsigned char**)(vec + 8);
	if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
		data = *(unsigned char**)vec;
		count = *(int*)(vec + 16);
	}
	if (count <= 0 || count > 128 || (uintptr_t)data <= 0x10000)
		return;
	if (listOff == o.C_EconItemView.m_NetworkedDynamicAttributes) {
		if (attrVec) *attrVec = 1;
		if (attrCount) *attrCount = count;
	}
	const int stride = (int)o.CEconItemAttribute.m_size;
	for (int i = 0; i < count; ++i) {
		unsigned char* attr = data + (ptrdiff_t)i * stride;
		const int def = (int)*(uint16_t*)(attr + o.CEconItemAttribute.m_iAttributeDefinitionIndex);
		float* pv = (float*)(attr + o.CEconItemAttribute.m_flValue);
		if (def == 6) { *pv = (float)paintKit; if (paintW) *paintW = 1; }
		else if (def == 7) { *pv = (float)seed; if (seedW) *seedW = 1; }
		else if (def == 8) { *pv = wear; if (wearW) *wearW = 1; }
	}
}

static void ReadGlovePaintAttrs(unsigned char* glove, ptrdiff_t listOff, int* paint, int* seed, float* wear) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!glove || !paint || !listOff || !o.C_AttributeList.m_Attributes
		|| !o.CEconItemAttribute.m_iAttributeDefinitionIndex || !o.CEconItemAttribute.m_flValue
		|| !o.CEconItemAttribute.m_size)
		return;
	unsigned char* vec = glove + listOff + o.C_AttributeList.m_Attributes;
	int count = *(int*)vec;
	unsigned char* data = *(unsigned char**)(vec + 8);
	if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
		data = *(unsigned char**)vec;
		count = *(int*)(vec + 16);
	}
	if (count <= 0 || count > 128 || (uintptr_t)data <= 0x10000)
		return;
	const int stride = (int)o.CEconItemAttribute.m_size;
	for (int i = 0; i < count; ++i) {
		unsigned char* attr = data + (ptrdiff_t)i * stride;
		const int def = (int)*(uint16_t*)(attr + o.CEconItemAttribute.m_iAttributeDefinitionIndex);
		const float v = *(float*)(attr + o.CEconItemAttribute.m_flValue);
		if (def == 6) *paint = (int)v;
		else if (def == 7 && seed) *seed = (int)v;
		else if (def == 8 && wear) *wear = v;
	}
}

// SEH-guarded glove field write + pawn/HUD-arms rebuild.
bool SafeApplyGloves(unsigned char* pawn, int gloveDef, int paintKit, float wear, int seed, uint32_t accountId,
	bool composePaintKit) {
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
	const char* modelFail = "unknown";
	// Diagnostics: POD counters filled inside the SEH block, logged after it (see "cosmetics.glove").
	int dAttrVec = 0, dAttrCount = 0, dPaintW = 0, dSeedW = 0, dWearW = 0, dCoreFaulted = 0, dBodyFaulted = 0;
	int dNamedSetter = 0, dConstructPk = 0;
	int dBodyGroupNum = 0, dBodyGroupStr = 0, dGlovesChanged = 0, dTeam = 0, dBootstrapped = 0;
	const bool trace = MvmDebugLog_Active();
	int pawnIdx = -1;
	__try {
		CEntityInstance* pawnInst = (CEntityInstance*)pawn;
		if (!pawnInst->IsPlayerPawn())
			return false;
		SOURCESDK::CS2::CBaseHandle ph = pawnInst->GetHandle();
		if (ph.IsValid())
			pawnIdx = ph.GetEntryIndex();
		if (o.C_BaseEntity.m_iTeamNum)
			dTeam = (int)*(uint8_t*)(pawn + o.C_BaseEntity.m_iTeamNum);
	} __except (1) {
		return false;
	}
	if (pawnIdx <= 0 || (dTeam != 2 && dTeam != 3))
		return false;
	__try {
		uint16_t curDef = *(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex);
		// Demo pawns often have m_EconGloves def=0 until the engine initializes default gloves.
		// Bootstrap team defaults before writing a custom glove (nerv clear-then-reapply pattern).
		if (curDef == 0) {
			const uint16_t bootDef = (dTeam == 2) ? 5028 : 5029;
			*(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex) = bootDef;
			dBootstrapped = 1;
		}
		*(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex) = (uint16_t)gloveDef;
		if (o.C_EconItemView.m_iEntityQuality)
			*(int32_t*)(glove + o.C_EconItemView.m_iEntityQuality) = 3; // QUALITY_UNUSUAL
		if (o.C_EconItemView.m_iAccountID)
			*(uint32_t*)(glove + o.C_EconItemView.m_iAccountID) = accountId;
		if (o.C_EconItemView.m_iItemIDHigh)
			*(uint32_t*)(glove + o.C_EconItemView.m_iItemIDHigh) = 1;
		if (o.C_EconItemView.m_iItemIDLow)
			*(uint32_t*)(glove + o.C_EconItemView.m_iItemIDLow) = accountId;
		if (o.C_EconItemView.m_iItemID) {
			uint64_t itemId = ((uint64_t)1u << 32) | (uint64_t)accountId;
			*(uint64_t*)(glove + o.C_EconItemView.m_iItemID) = itemId;
		}
		if (o.C_EconItemView.m_bDisallowSOC)
			*(bool*)(glove + o.C_EconItemView.m_bDisallowSOC) = false;
		if (o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache)
			*(bool*)(glove + o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache) = true;
		// Overwrite paint attributes on networked list, then local list if still empty.
		WriteGlovePaintAttrs(glove, o.C_EconItemView.m_NetworkedDynamicAttributes, paintKit, wear, seed,
			&dPaintW, &dSeedW, &dWearW, &dAttrVec, &dAttrCount);
		if (!dPaintW)
			WriteGlovePaintAttrs(glove, o.C_EconItemView.m_AttributeList, paintKit, wear, seed,
				&dPaintW, &dSeedW, &dWearW, nullptr, nullptr);
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
	} __except (1) {
		dCoreFaulted = 1;
	}
	if (!dCoreFaulted && composePaintKit && paintKit > 0 && g_fns.constructPaintKit) {
		__try {
			g_fns.constructPaintKit(glove);
			dConstructPk = 1;
		} __except (1) {
			if (trace) MvmDebugLog_LinefAlways("glove.swap", "step=constructPk.FAULT idx=%d", pawnIdx);
		}
	}
	// Body-group / glove-changed flags rebuild render composites -- only on the first burst frame.
	if (!dCoreFaulted && composePaintKit) {
		__try {
			if (g_fns.setBodyGroupNumeric) {
				g_fns.setBodyGroupNumeric(pawn, 0, 1);
				dBodyGroupNum = 1;
			}
			// Both teams need the named first_or_third_person group for third-person glove meshes.
			// (setBodyGroupNumeric(1,1) faults; the string form is safe on T and CT.)
			if ((dTeam == 2 || dTeam == 3) && g_fns.setBodyGroup) {
				g_fns.setBodyGroup(pawn, "first_or_third_person", 1);
				dBodyGroupStr = 1;
			}
			if (g_fns.updateBodyGroupChoice)
				g_fns.updateBodyGroupChoice(pawn);
			if (o.C_CSPlayerPawn.m_nEconGlovesChanged) {
				uint8_t* pChg = (uint8_t*)(pawn + o.C_CSPlayerPawn.m_nEconGlovesChanged);
				*pChg = (uint8_t)(*pChg + 1u);
				dGlovesChanged = 1;
			}
			if (o.C_CSPlayerPawn.m_bNeedToReApplyGloves)
				*(bool*)(pawn + o.C_CSPlayerPawn.m_bNeedToReApplyGloves) = true;
		} __except (1) {
			dBodyFaulted = 1;
		}
	}
	const int dFaulted = dCoreFaulted ? 1 : 0;
	bool haveGloveModel = false;
	ptrdiff_t defMapOff = g_itemDefMapOffset;
	if (dCoreFaulted) {
		if (trace) {
			MvmCrashWatch_Arm(pawnIdx, "glove-core-fault");
			MvmDebugLog_LinefAlways("glove.swap", "ABORT idx=%d coreFault=1 team=%d", pawnIdx, dTeam);
			MvmDebugLog_LinefAlways("cosmetics.glove",
				"def=%d paint=%d ABORT coreFault=1 team=%d pawnIdx=%d", gloveDef, paintKit, dTeam, pawnIdx);
		}
		return false;
	}
	haveGloveModel = TryGetModelFromStaticData(glove, gloveModel, sizeof(gloveModel), &modelFail);
	defMapOff = g_itemDefMapOffset;
	if (trace) {
		MvmCrashWatch_Arm(pawnIdx, haveGloveModel ? gloveModel : "glove");
		MvmDebugLog_LinefAlways("glove.swap", "BEGIN idx=%d pawn=%p def=%d paint=%d team=%d model='%s'",
			pawnIdx, (void*)pawn, gloveDef, paintKit, dTeam, haveGloveModel ? gloveModel : "");
	}
	if (haveGloveModel)
		SafePrecacheModel(gloveModel);
	const ptrdiff_t offArms = o.C_CSPlayerPawn.m_hHudModelArms;
	uint32_t armsHandle = 0;
	int armsIdx = -1;
	if (pawn && offArms) {
		__try {
			armsHandle = *(uint32_t*)(pawn + offArms);
			SOURCESDK::CS2::CBaseHandle h(armsHandle);
			if (h.IsValid())
				armsIdx = h.GetEntryIndex();
		} __except (1) { armsHandle = 0; }
	}
	unsigned char* arms = nullptr;
	bool armsResolved = false;
	if (composePaintKit) {
		// Refresh the pawn renderable so the new glove body group renders without waiting for a live sim
		// frame (essential during a paused demo). Separate SEH scope from the field writes above.
		SafePostDataUpdate(pawn);
		arms = HudArmsForPawn(pawn);
		armsResolved = arms != nullptr;
		if (arms) {
			__try {
				SOURCESDK::CS2::CBaseHandle ah = ((CEntityInstance*)arms)->GetHandle();
				if (ah.IsValid())
					armsIdx = ah.GetEntryIndex();
			} __except (1) { armsIdx = -1; }
		}
		if (arms && haveGloveModel && HudArmsOwnedByPawnIndex(arms, pawnIdx)) {
			if (trace) MvmDebugLog_LinefAlways("glove.swap", "step=armsSetModel.begin idx=%d arms=%p", pawnIdx, (void*)arms);
			SafeSetModel(arms, gloveModel);
			if (trace) MvmDebugLog_LinefAlways("glove.swap", "step=armsSetModel.end idx=%d", pawnIdx);
			SafePostDataUpdate(arms);
		} else if (arms && trace) {
			MvmDebugLog_LinefAlways("glove.swap", "step=armsSetModel.skip idx=%d arms=%p ownerMismatch=1", pawnIdx, (void*)arms);
		}
	}
	// Read live glove state after writes (persist check; networked first, then local attrs).
	int liveDef = 0, livePaint = 0;
	float liveWear = 0.0f;
	int liveSeed = 0;
	__try {
		liveDef = (int)*(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex);
		ReadGlovePaintAttrs(glove, o.C_EconItemView.m_NetworkedDynamicAttributes, &livePaint, &liveSeed, &liveWear);
		if (livePaint == 0)
			ReadGlovePaintAttrs(glove, o.C_EconItemView.m_AttributeList, &livePaint, &liveSeed, &liveWear);
	} __except (1) {
	}
	// Diagnostic line (always flushed during mvm_debug). Tells which sub-step broke: dAttrVec=0 -> the demo
	// glove has NO networked attribute vector (paint can't be written -> default skin); bodyGroupFn=0
	// -> UpdateBodyGroupChoice unresolved; arms=0 -> first-person glove model not refreshed; haveModel=0
	// -> glove def's model path didn't resolve.
	if (MvmDebugLog_Active()) {
		MvmDebugLog_LinefAlways("cosmetics.glove",
			"def=%d paint=%d seed=%d wear=%.3f faulted=%d attrVec=%d attrCount=%d wrote(p=%d s=%d w=%d) "
			"namedSetter=%d bootstrapped=%d bodyGroupFn=%d bodyGroupNum=%d bodyGroupStr=%d bodyFault=%d team=%d "
			"needReApplyOff=%d initOff=%d haveModel=%d arms=%d model='%s' "
			"liveDef=%d livePaint=%d liveSeed=%d liveWear=%.3f",
			gloveDef, paintKit, seed, wear, dFaulted, dAttrVec, dAttrCount, dPaintW, dSeedW, dWearW,
			dNamedSetter, dBootstrapped, g_fns.updateBodyGroupChoice ? 1 : 0, dBodyGroupNum, dBodyGroupStr, dBodyFaulted, dTeam,
			o.C_CSPlayerPawn.m_bNeedToReApplyGloves ? 1 : 0,
			o.C_EconItemView.m_bInitialized ? 1 : 0,
			haveGloveModel ? 1 : 0, armsResolved ? 1 : 0, haveGloveModel ? gloveModel : "",
			liveDef, livePaint, liveSeed, liveWear);
		char data[720];
		std::snprintf(data, sizeof(data),
			"\"wantDef\":%d,\"wantPaint\":%d,\"liveDef\":%d,\"livePaint\":%d,"
			"\"wrotePaint\":%d,\"namedSetter\":%d,\"constructPk\":%d,\"bodyGroupNum\":%d,"
			"\"bodyGroupStr\":%d,\"bodyFault\":%d,\"team\":%d,\"glovesChanged\":%d,"
			"\"attrVec\":%d,\"haveModel\":%d,\"modelFail\":\"%s\","
			"\"arms\":%d,\"armsHandle\":%u,\"armsIdx\":%d,\"pawnIdx\":%d,\"getStaticData\":%d,\"getItemDefByIndex\":%d,\"defMapOff\":%lld,"
			"\"disallowSocOff\":%d,\"restoreMatOff\":%d,\"itemIdOff\":%d,\"faulted\":%d",
			gloveDef, paintKit, liveDef, livePaint, dPaintW, dNamedSetter, dConstructPk, dBodyGroupNum,
			dBodyGroupStr, dBodyFaulted, dTeam, dGlovesChanged,
			dAttrVec,
			haveGloveModel ? 1 : 0, modelFail ? modelFail : "",
			armsResolved ? 1 : 0, armsHandle, armsIdx, pawnIdx,
			g_fns.getStaticData ? 1 : 0, g_fns.getItemDefByIndex ? 1 : 0, (long long)defMapOff,
			o.C_EconItemView.m_bDisallowSOC ? 1 : 0,
			o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache ? 1 : 0,
			o.C_EconItemView.m_iItemID ? 1 : 0,
			dFaulted);
		MvmAgentLog(liveDef == gloveDef && (paintKit <= 0 || livePaint == paintKit) ? "H5" : "H4",
			"CosmeticModelSwap.cpp:SafeApplyGloves", "apply_done", data);
		if (trace)
			MvmDebugLog_LinefAlways("glove.swap", "END idx=%d coreFault=%d bodyFault=%d arms=%d compose=%d",
				pawnIdx, dCoreFaulted, dBodyFaulted, armsResolved ? 1 : 0, composePaintKit ? 1 : 0);
	}
	return !dCoreFaulted;
}

// CUtlMap<int, CPaintKit*> node layout (x64): left@0 right@4 parent@8 type@12 key@16 value@24 (32 bytes).
// Map header: size@0 allocCount@4 memory@8 root@16 numElements@20.
static void* FindPaintKitInSchema(void* schema, int paintKitId) {
	if (!schema || paintKitId <= 0)
		return nullptr;
	unsigned char* map = (unsigned char*)schema + kEconItemSchema_PaintKitsOffset;
	unsigned char* memory = *(unsigned char**)(map + 8);
	if (!memory || (uintptr_t)memory < 0x10000)
		return nullptr;
	int root = *(int*)(map + 16);
	if (root >= 0) {
		int idx = root;
		for (int guard = 0; guard < 96 && idx >= 0; ++guard) {
			unsigned char* node = memory + (ptrdiff_t)idx * 32;
			int key = *(int*)(node + 16);
			if (paintKitId < key)
				idx = *(int*)(node + 0);
			else if (paintKitId > key)
				idx = *(int*)(node + 4);
			else {
				void* pk = *(void**)(node + 24);
				return (pk && (uintptr_t)pk > 0x10000) ? pk : nullptr;
			}
		}
	}
	// Fallback: scan the node pool (covers builds where the tree root is stale but nodes are populated).
	int pool = *(int*)(map + 4);
	if (pool <= 0 || pool > 100000)
		pool = *(int*)map;
	if (pool <= 0 || pool > 100000)
		return nullptr;
	for (int i = 0; i < pool; ++i) {
		unsigned char* node = memory + (ptrdiff_t)i * 32;
		if (*(int*)(node + 16) != paintKitId)
			continue;
		void* pk = *(void**)(node + 24);
		if (pk && (uintptr_t)pk > 0x10000)
			return pk;
	}
	return nullptr;
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
		void* pk = FindPaintKitInSchema(schema, paintKitId);
		if (!pk)
			return -1;
		return *(uint8_t*)((unsigned char*)pk + kPaintKit_IsUseLegacyModelOffset) ? 1 : 0;
	} __except (1) {
		return -1;
	}
}

const char* DesiredBodyChoiceName(uint64_t meshMask) {
	if (meshMask == 1)
		return "body_hd";
	if (meshMask == 2)
		return "body_legacy";
	return "?";
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
	const char* wantBodyChoice = DesiredBodyChoiceName(meshMask);
	char wantModel[160];
	wantModel[0] = '\0';
	if (!pawn) {
		if (trace)
			MvmDebugLog_LinefAlways("cosmetics.weapon.vm", "ABORT noPawn worldIdx=%d", entityIndex);
		return;
	}
	if (o.C_CSPlayerPawn.m_hHudModelArms == 0 || o.C_BaseEntity.m_pGameSceneNode == 0 ||
		o.CGameSceneNode.m_pChild == 0 || o.CGameSceneNode.m_pNextSibling == 0 ||
		o.CGameSceneNode.m_pOwner == 0) {
		if (trace)
			MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
				"ABORT missingOffsets worldIdx=%d offArms=0x%llx offScene=0x%llx",
				entityIndex, (unsigned long long)o.C_CSPlayerPawn.m_hHudModelArms,
				(unsigned long long)o.C_BaseEntity.m_pGameSceneNode);
		return;
	}

	const char* wantClass = nullptr;
	if (worldWeapon) {
		__try { wantClass = ((CEntityInstance*)worldWeapon)->GetClassName(); }
		__except (1) { wantClass = nullptr; }
	}
	ResolveWeaponMatchModelPath(worldWeapon, model, wantModel, sizeof(wantModel));

	HudArmsResolve hres = ResolveHudArmsForViewmodel(pawn, wantClass);
	unsigned char* arms = hres.arms;
	if (!arms) {
		if (trace) {
			int pawnIdx = -1;
			__try {
				SOURCESDK::CS2::CBaseHandle ph = ((CEntityInstance*)pawn)->GetHandle();
				if (ph.IsValid()) pawnIdx = ph.GetEntryIndex();
			} __except (1) {
				pawnIdx = -1;
			}
			MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
				"ABORT noHudArms pawnIdx=%d worldIdx=%d mask=%llu armsSrc=%s hudArmsEnts=%d wantClass='%s'",
				pawnIdx, entityIndex, (unsigned long long)meshMask, hres.source, hres.hudArmsEntities,
				wantClass ? wantClass : "?");
		}
		return;
	}

	// Class of the weapon we are mirroring onto its own first-person viewmodel. Only same-class
	// children receive the model/mesh write; every other weapon's viewmodel is left untouched.
	if (trace)
		MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
			"walk.begin worldIdx=%d arms=%p armsIdx=%d armsSrc=%s hudArmsEnts=%d wantClass='%s' wantBody='%s' model='%s' mask=%llu",
			entityIndex, (void*)arms, hres.armsIdx, hres.source, hres.hudArmsEntities,
			wantClass ? wantClass : "?", wantBodyChoice, model ? model : "", (unsigned long long)meshMask);
	// #region agent log
	{
		char adata[384];
		std::snprintf(adata, sizeof(adata),
			"\"worldIdx\":%d,\"armsIdx\":%d,\"armsSrc\":\"%s\",\"wantClass\":\"%s\",\"wantModel\":\"%s\",\"wantBody\":\"%s\",\"mask\":%llu",
			entityIndex, hres.armsIdx, hres.source ? hres.source : "?", wantClass ? wantClass : "?",
			wantModel, wantBodyChoice ? wantBodyChoice : "?", (unsigned long long)meshMask);
		MvmAgentLog("FP-VM", "CosmeticModelSwap.cpp:RefreshViewmodelWeapons", "vm_walk_begin", adata);
	}
	// #endregion

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
				bool classMatch = false;
				bool modelMatch = false;
				const char* oc = nullptr;
				int childIdx = -1;
				int childPaint = -1;
				char childModel[160];
				childModel[0] = '\0';
				oc = ((CEntityInstance*)owner)->GetClassName();
				SOURCESDK::CS2::CBaseHandle ch = ((CEntityInstance*)owner)->GetHandle();
				if (ch.IsValid())
					childIdx = ch.GetEntryIndex();
				ReadEntityModelPath((CEntityInstance*)owner, childModel, sizeof(childModel));
				classMatch = wantClass && oc && 0 == std::strcmp(oc, wantClass);
				modelMatch = SameModelPath(wantModel, childModel);
				sameWeapon = ViewmodelChildMatchesWeapon(wantClass, wantModel, oc, childModel);
				if (trace)
					MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
						"child worldIdx=%d n=%d owner=%p idx=%d cls='%s' model='%s' wantBody='%s' classMatch=%d modelMatch=%d same=%d paint=%d willWriteModel=%d willWriteMesh=%d",
						entityIndex, guard, owner, childIdx, oc ? oc : "?", childModel,
						wantBodyChoice, classMatch ? 1 : 0, modelMatch ? 1 : 0, sameWeapon ? 1 : 0,
						childPaint,
						(sameWeapon && model && *model) ? 1 : 0, (sameWeapon && meshMask) ? 1 : 0);
				// #region agent log
				if (guard < 4) {
					char adata[512];
					std::snprintf(adata, sizeof(adata),
						"\"worldIdx\":%d,\"n\":%d,\"idx\":%d,\"cls\":\"%s\",\"model\":\"%s\",\"wantClass\":\"%s\",\"wantModel\":\"%s\",\"wantBody\":\"%s\",\"classMatch\":%d,\"modelMatch\":%d,\"same\":%d,\"paint\":%d,\"meshWrite\":%d",
						entityIndex, guard, childIdx, oc ? oc : "?", childModel,
						wantClass ? wantClass : "?", wantModel, wantBodyChoice ? wantBodyChoice : "?",
						classMatch ? 1 : 0, modelMatch ? 1 : 0, sameWeapon ? 1 : 0, childPaint, (sameWeapon && meshMask) ? 1 : 0);
					MvmAgentLog("FP-VM", "CosmeticModelSwap.cpp:RefreshViewmodelWeapons", "vm_child", adata);
				}
				// #endregion
				if (sameWeapon) {
					if (model && *model)
						SafeSetModel((unsigned char*)owner, model);
					if (meshMask) {
						uint64_t liveChildMask = ReadEntityMeshGroupMask((CEntityInstance*)owner);
						if (liveChildMask != meshMask)
							SafeSetMeshMask((unsigned char*)owner, meshMask);
					}
					ResetAnimGraph((unsigned char*)owner, entityIndex, "vm");
					SafePostDataUpdate((unsigned char*)owner);
				}
			}
			child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
		}
	} __except (1) {
		if (trace) MvmDebugLog_LinefAlways("cosmetics.weapon.vm", "walk.FAULT worldIdx=%d (SEH caught during child walk)", entityIndex);
		return;
	}
	SafePostDataUpdate(arms);
	if (trace) MvmDebugLog_LinefAlways("cosmetics.weapon.vm", "walk.end worldIdx=%d", entityIndex);
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
	g_fns.setBodyGroupNumeric = (SetBodyGroupNumeric_t)FindClientPattern(
		"85 D2 0F 88 ?? ?? ?? ?? 55 53");
	g_fns.regenerateSkins = (RegenerateSkins_t)FindClientPattern(
		"48 83 EC ?? E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 10");
	g_fns.updateBodyGroupChoice = (UpdateBodyGroupChoice_t)ResolveRelCall(FindClientPattern(
		"E8 ?? ?? ?? ?? 4C 8B AC 24 ?? ?? ?? ?? 48 8B BC 24"));
	g_fns.getStaticData = (GetStaticData_t)FindClientPattern(
		"40 56 48 83 EC ?? 48 89 5C 24 ?? 48 8B F1 48 8B 1D ?? ?? ?? ?? 48 85 DB 75");
	g_fns.getEconItemSystem = (GetEconItemSystem_t)FindClientPattern(
		"48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 85 81");
	g_fns.setAttributeValueByName = (SetAttributeValueByName_t)ResolveRelCall(FindClientPattern(
		"E8 ?? ?? ?? ?? 66 41 0F 6E D4"));
	g_fns.constructPaintKit = (ConstructPaintKit_t)FindClientPattern(
		"48 89 5C 24 ?? 56 48 83 EC ?? 48 8B 01 FF 50");
	g_fns.getItemDefByIndex = (SchemaGetItemDefByIndex_t)FindClientPattern(
		"48 89 5C 24 ?? 57 48 83 EC ?? 48 8B D9 89 54 24");

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

bool ResolveKnifeModelPath(int targetDef, unsigned char* itemView, char* out, size_t outSize) {
	if (!out || outSize == 0)
		return false;
	out[0] = '\0';
	if (targetDef <= 0)
		return false;
	ResolveModelSwapFns();
	// Same resolution order as ApplyKnifeModelSwap: live econ definition first, built-in table fallback.
	if (TryGetModelFromStaticData(itemView, out, outSize) && out[0])
		return true;
	const char* tbl = KnifeModelForDef(targetDef);
	if (!tbl)
		return false;
	std::snprintf(out, outSize, "%s", tbl);
	return out[0] != '\0';
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

void ApplyWeaponMeshMask(unsigned char* weaponEntity, uint64_t meshMask, unsigned char* pawnForViewmodel, int entityIndex) {
	ResolveModelSwapFns();
	if (meshMask == 0)
		return;
	SafeSetMeshMask(weaponEntity, meshMask);
	// Same fix as ApplyKnifeModelSwap's "approach #1": reset the animgraph after the mesh-group write so
	// the engine rebuilds it for the new mesh rather than posing it with stale per-mesh-group data. A
	// legacy<->CS2 mesh-group toggle (this function) hits the same worker-thread null-deref the knife
	// investigation chased -- it was only ever fixed on the knife SetModel path, not here. Order matches
	// the knife path: mesh write, anim reset, viewmodel mirror, then PostDataUpdate last.
	ResetAnimGraph(weaponEntity, entityIndex, "world");
	if (pawnForViewmodel)
		RefreshViewmodelWeapons(pawnForViewmodel, nullptr, meshMask, weaponEntity);
	SafePostDataUpdate(weaponEntity);
	// NOTE: a trailing re-assert SafeSetMeshMask() call was tried here to fight a mesh-group revert
	// seen on weapons using the fallback (non-networked) paint path -- live-tested, did NOT converge
	// (preMask still never matched wantMask next frame) and reverted, so removed. The revert happens
	// somewhere other than (or in addition to) PostDataUpdate; root cause still open. See
	// [memory: weapon-mesh-mask-revert-investigation] before trying another patch here.
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

uint64_t ReadEntityMeshGroupMask(CEntityInstance* ent) {
	if (!ent)
		return 0;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.ModelChain.m_CBodyComponent == 0 || o.ModelChain.m_skeletonInstance == 0
		|| o.ModelChain.m_modelState == 0 || o.ModelChain.m_MeshGroupMask == 0)
		return 0;
	unsigned char* p = (unsigned char*)ent;
	__try {
		unsigned char* bodyComp = *(unsigned char**)(p + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)bodyComp <= 0x10000)
			return 0;
		unsigned char* modelState = bodyComp + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
		return *(uint64_t*)(modelState + o.ModelChain.m_MeshGroupMask);
	} __except (1) {
		return 0;
	}
}

int ReadNetPaintFromItemView(unsigned char* itemView) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!itemView || o.C_EconItemView.m_NetworkedDynamicAttributes == 0
		|| o.C_AttributeList.m_Attributes == 0 || o.CEconItemAttribute.m_iAttributeDefinitionIndex == 0
		|| o.CEconItemAttribute.m_flValue == 0)
		return -1;
	int stride = (int)o.CEconItemAttribute.m_size;
	if (stride < (int)o.CEconItemAttribute.m_flValue + (int)sizeof(float))
		stride = (int)o.CEconItemAttribute.m_flValue + (int)sizeof(float);
	unsigned char* vectorField = itemView + o.C_EconItemView.m_NetworkedDynamicAttributes + o.C_AttributeList.m_Attributes;
	__try {
		int count = *(int*)vectorField;
		unsigned char* data = *(unsigned char**)(vectorField + 8);
		if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
			data = *(unsigned char**)vectorField;
			count = *(int*)(vectorField + 16);
		}
		if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000))
			return -1;
		for (int i = 0; i < count; ++i) {
			unsigned char* attr = data + (ptrdiff_t)i * stride;
			int def = (int)*(uint16_t*)(attr + o.CEconItemAttribute.m_iAttributeDefinitionIndex);
			if (def == 6)
				return (int)*(float*)(attr + o.CEconItemAttribute.m_flValue);
		}
	} __except (1) {
	}
	return -1;
}

unsigned char* ItemViewForWeapon(unsigned char* weapon) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!weapon || o.C_EconEntity.m_AttributeManager == 0 || o.C_AttributeContainer.m_Item == 0)
		return nullptr;
	return weapon + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;
}

ViewmodelMirrorResult MirrorWeaponCosmeticsToViewmodel(
	unsigned char* pawn,
	unsigned char* worldWeapon,
	int worldEntityIndex,
	unsigned char* worldItemView,
	uint64_t meshMask,
	int32_t paintKit,
	float wear,
	int32_t seed,
	int statTrak,
	ptrdiff_t compositeOwnerOffset,
	ptrdiff_t offRestoreMaterial) {
	ViewmodelMirrorResult r = {};
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	const bool trace = MvmDebugLog_Active();
	const char* wantBodyChoice = DesiredBodyChoiceName(meshMask);
	char wantModel[160];
	wantModel[0] = '\0';
	if (!pawn || !worldWeapon || paintKit <= 0) {
		if (trace)
			MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
				"SKIP mirror worldIdx=%d pawn=%p weapon=%p paint=%d",
				worldEntityIndex, (void*)pawn, (void*)worldWeapon, paintKit);
		return r;
	}

	const char* wantClass = nullptr;
	__try { wantClass = ((CEntityInstance*)worldWeapon)->GetClassName(); }
	__except (1) { wantClass = nullptr; }
	ResolveWeaponMatchModelPath(worldWeapon, nullptr, wantModel, sizeof(wantModel));

	HudArmsResolve hres = ResolveHudArmsForViewmodel(pawn, wantClass);
	unsigned char* arms = hres.arms;
	if (!arms) {
		if (trace) {
			int pawnIdx = -1;
			__try {
				SOURCESDK::CS2::CBaseHandle ph = ((CEntityInstance*)pawn)->GetHandle();
				if (ph.IsValid()) pawnIdx = ph.GetEntryIndex();
			} __except (1) {}
			MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
				"MIRROR_ABORT noHudArms pawnIdx=%d worldIdx=%d wantClass='%s' paint=%d mask=%llu "
				"armsSrc=%s armsIdx=%d hudArmsEnts=%d",
				pawnIdx, worldEntityIndex, wantClass ? wantClass : "?", paintKit,
				(unsigned long long)meshMask, hres.source, hres.armsIdx, hres.hudArmsEntities);
			char probe[2048];
			BuildViewmodelProbe(pawn, wantClass, worldEntityIndex, probe, sizeof(probe));
			MvmDebugLog_Linef("cosmetics.weapon.vm", "%s", probe);
		}
		return r;
	}
	r.armsFound = true;

	if (o.C_BaseEntity.m_pGameSceneNode == 0 || o.CGameSceneNode.m_pChild == 0
		|| o.CGameSceneNode.m_pNextSibling == 0 || o.CGameSceneNode.m_pOwner == 0)
		return r;

	__try {
		void* armsNode = *(void**)(arms + o.C_BaseEntity.m_pGameSceneNode);
		if (!armsNode)
			return r;
		void* child = *(void**)((unsigned char*)armsNode + o.CGameSceneNode.m_pChild);
		for (int guard = 0; child && guard < 64; ++guard) {
			void* owner = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pOwner);
			if (!owner || !LooksLikeWeapon(owner)) {
				child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
				continue;
			}
			const char* oc = ((CEntityInstance*)owner)->GetClassName();
			char childModel[160];
			childModel[0] = '\0';
			ReadEntityModelPath((CEntityInstance*)owner, childModel, sizeof(childModel));
			const bool classMatch = wantClass && oc && 0 == std::strcmp(oc, wantClass);
			const bool modelMatch = SameModelPath(wantModel, childModel);
			SOURCESDK::CS2::CBaseHandle vhProbe = ((CEntityInstance*)owner)->GetHandle();
			int childIdx = vhProbe.IsValid() ? vhProbe.GetEntryIndex() : -1;
			if (trace)
				MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
					"mirror.child worldIdx=%d n=%d idx=%d cls='%s' model='%s' wantClass='%s' wantModel='%s' wantBody='%s' classMatch=%d modelMatch=%d",
					worldEntityIndex, guard, childIdx, oc ? oc : "?",
					childModel, wantClass ? wantClass : "?", wantModel, wantBodyChoice, classMatch ? 1 : 0, modelMatch ? 1 : 0);
			// #region agent log
			if (guard < 4) {
				char adata[384];
				std::snprintf(adata, sizeof(adata),
					"\"worldIdx\":%d,\"n\":%d,\"idx\":%d,\"cls\":\"%s\",\"model\":\"%s\",\"wantClass\":\"%s\",\"wantModel\":\"%s\",\"wantBody\":\"%s\",\"classMatch\":%d,\"modelMatch\":%d",
					worldEntityIndex, guard, childIdx, oc ? oc : "?",
					childModel, wantClass ? wantClass : "?", wantModel, wantBodyChoice ? wantBodyChoice : "?",
					classMatch ? 1 : 0, modelMatch ? 1 : 0);
				MvmAgentLog("FP-VM", "CosmeticModelSwap.cpp:MirrorWeaponCosmeticsToViewmodel", "vm_mirror_child", adata);
			}
			// #endregion
			if (!ViewmodelChildMatchesWeapon(wantClass, wantModel, oc, childModel)) {
				child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
				continue;
			}

			r.childMatched = true;
			unsigned char* vmWeapon = (unsigned char*)owner;
			unsigned char* vmItemView = ItemViewForWeapon(vmWeapon);
			SOURCESDK::CS2::CBaseHandle vh = ((CEntityInstance*)owner)->GetHandle();
			if (vh.IsValid())
				r.vmEntityIndex = vh.GetEntryIndex();

			r.meshBefore = ReadEntityMeshGroupMask((CEntityInstance*)owner);
			if (vmItemView)
				r.paintBefore = ReadNetPaintFromItemView(vmItemView);

			if (meshMask != 0) {
				SafeSetMeshMask(vmWeapon, meshMask);
				r.meshWritten = true;
			}

			if (vmItemView) {
				if (ReadNetPaintFromItemView(vmItemView) < 0)
					r.namedSetter = FireNamedSkinAttributes(vmItemView, paintKit, wear, seed, statTrak);
				DirectCompositeResult dc = FireDirectCompositeRefresh(
					vmWeapon, vmItemView, offRestoreMaterial, compositeOwnerOffset, paintKit, wear, seed);
				r.compositeCalled = dc.called;
				r.compositeFaulted = dc.faulted;
			}

			r.meshAfter = ReadEntityMeshGroupMask((CEntityInstance*)owner);
			if (vmItemView)
				r.paintAfter = ReadNetPaintFromItemView(vmItemView);

			ResetAnimGraph(vmWeapon, r.vmEntityIndex, "vm");
			SafePostDataUpdate(vmWeapon);

			if (trace) {
				MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
					"MIRROR worldIdx=%d vmIdx=%d armsSrc=%s cls='%s' paint=%d->%d mesh=%llu->%llu "
					"meshW=%d named=%d composite=%d fault=%d",
					worldEntityIndex, r.vmEntityIndex, hres.source, oc ? oc : "?",
					r.paintBefore, r.paintAfter,
					(unsigned long long)r.meshBefore, (unsigned long long)r.meshAfter,
					r.meshWritten ? 1 : 0, r.namedSetter ? 1 : 0,
					r.compositeCalled ? 1 : 0, r.compositeFaulted ? 1 : 0);
				char adata[384];
				std::snprintf(adata, sizeof(adata),
					"\"worldIdx\":%d,\"vmIdx\":%d,\"paintBefore\":%d,\"paintAfter\":%d,"
					"\"meshBefore\":%llu,\"meshAfter\":%llu,\"composite\":%d",
					worldEntityIndex, r.vmEntityIndex, r.paintBefore, r.paintAfter,
					(unsigned long long)r.meshBefore, (unsigned long long)r.meshAfter,
					r.compositeCalled ? 1 : 0);
				MvmAgentLog("FP-VM", "CosmeticModelSwap.cpp:MirrorWeaponCosmeticsToViewmodel", "vm_mirror", adata);
			}
			break; // only the matching active viewmodel class
		}
		if (!r.childMatched && trace) {
			MvmDebugLog_LinefAlways("cosmetics.weapon.vm",
				"MIRROR_NO_CHILD worldIdx=%d wantClass='%s' arms=%p paint=%d mask=%llu",
				worldEntityIndex, wantClass ? wantClass : "?", (void*)arms, paintKit,
				(unsigned long long)meshMask);
		}
	} __except (1) {
		if (trace)
			MvmDebugLog_LinefAlways("cosmetics.weapon.vm", "MIRROR_FAULT worldIdx=%d", worldEntityIndex);
	}
	(void)worldItemView;
	return r;
}

bool ReadActiveViewmodelWeaponState(unsigned char* pawn, const char* wantWeaponClass,
	int* outVmIndex, int* outPaint, uint64_t* outMeshMask) {
	if (outVmIndex) *outVmIndex = -1;
	if (outPaint) *outPaint = -1;
	if (outMeshMask) *outMeshMask = 0;
	if (!pawn || !wantWeaponClass || !*wantWeaponClass)
		return false;

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	HudArmsResolve hres = ResolveHudArmsForViewmodel(pawn, wantWeaponClass);
	unsigned char* arms = hres.arms;
	if (!arms) {
		return false;
	}
	if (o.C_BaseEntity.m_pGameSceneNode == 0 || o.CGameSceneNode.m_pChild == 0
		|| o.CGameSceneNode.m_pNextSibling == 0 || o.CGameSceneNode.m_pOwner == 0)
		return false;

	bool found = false;
	__try {
		void* armsNode = *(void**)(arms + o.C_BaseEntity.m_pGameSceneNode);
		if (!armsNode)
			return false;
		void* child = *(void**)((unsigned char*)armsNode + o.CGameSceneNode.m_pChild);
		for (int guard = 0; child && guard < 64; ++guard) {
			void* owner = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pOwner);
			if (owner && LooksLikeWeapon(owner)) {
				const char* oc = ((CEntityInstance*)owner)->GetClassName();
				if (oc && 0 == std::strcmp(oc, wantWeaponClass)) {
					SOURCESDK::CS2::CBaseHandle vh = ((CEntityInstance*)owner)->GetHandle();
					if (outVmIndex && vh.IsValid())
						*outVmIndex = vh.GetEntryIndex();
					if (outMeshMask)
						*outMeshMask = ReadEntityMeshGroupMask((CEntityInstance*)owner);
					unsigned char* iv = ItemViewForWeapon((unsigned char*)owner);
					if (outPaint && iv)
						*outPaint = ReadNetPaintFromItemView(iv);
					found = true;
					break;
				}
			}
			child = *(void**)((unsigned char*)child + o.CGameSceneNode.m_pNextSibling);
		}
	} __except (1) {
		return false;
	}
	return found;
}

bool ApplyGloveModel(unsigned char* pawn, int gloveDef, int paintKit, float wear, int seed,
	uint32_t accountId, bool composePaintKit) {
	ResolveModelSwapFns();
	if (!pawn || gloveDef <= 0) {
		if (MvmDebugLog_Active())
			MvmAgentLog("H3", "CosmeticModelSwap.cpp:ApplyGloveModel", "bad_args",
				pawn ? "\"pawn\":1" : "\"pawn\":0");
		return false;
	}
	if (!g_status.GlovesOk()) {
		if (MvmDebugLog_Active()) {
			char data[160];
			std::snprintf(data, sizeof(data),
				"\"setModel\":%d,\"updateBodyGroupChoice\":%d",
				g_status.setModel ? 1 : 0, g_status.updateBodyGroupChoice ? 1 : 0);
			MvmAgentLog("H3", "CosmeticModelSwap.cpp:ApplyGloveModel", "gloves_not_ok", data);
			MvmDebugLog_LinefAlways("cosmetics.glove", "ABORT GlovesOk=0 setModel=%d bodyGroupFn=%d",
				g_status.setModel ? 1 : 0, g_status.updateBodyGroupChoice ? 1 : 0);
		}
		return false;
	}
	return SafeApplyGloves(pawn, gloveDef, paintKit, wear, seed, accountId, composePaintKit);
}

} // namespace Filmmaker
