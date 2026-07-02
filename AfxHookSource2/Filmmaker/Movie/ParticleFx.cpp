#include "ParticleFx.h"

#include "../Filmmaker.h"                  // PlayingDemoPath (level-change detection)
#include "../Cosmetics/CosmeticDebugLog.h" // MvmDebugLog_* (thread-safe flight recorder)
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../../hlaeFolder.h"              // GetHlaeRoamingAppDataFolderW
#include "../../../shared/AfxConsole.h"    // advancedfx::Message/Warning + ICommandArgs

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../deps/release/Detours/src/detours.h"
#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h"

// Engine pointer (same one CameraPath/MovieMode use) for the toggle-time re-apply seek.
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Filmmaker {

namespace {

// ============================== engine binding =====================================

// CParticleSystemMgr::FindParticleSystem (vtable +0x78). rdx is an out slot the engine
// writes the resolved handle into (it also returns a pointer whose pointee is the handle);
// r9b=1 mirrors what the engine's own create-by-name wrapper passes. This function also
// owns the "not precached -> blocking load" path, so resolving a never-yet-seen system by
// name self-heals (one-time hitch) instead of failing.
typedef void* (__fastcall* FindSystem_t)(void* self, void** out, const char* name, unsigned char byName);

// The create-by-handle vtable STUB (+0x88): a standard vtable method (rcx = manager
// `this`, discarded by its own `xor ecx,ecx`; rdx = particle-system strong handle). We
// detour THIS, never the shared body behind it -- see the crash-lesson comment at the
// install site. Args are forwarded as RAW 8-byte slots with headroom (12): typed
// narrowing (u8/float/int) truncates stack slots, and static RE undercounted the arg
// count -- both crashed in-demo (2026-07-02 dumps).
typedef void* (__fastcall* CreateBody_t)(unsigned long long thisOrFlag, void* handle, void* a3, void* a4,
	unsigned long long a5, unsigned long long a6, unsigned long long a7, unsigned long long a8,
	unsigned long long a9, unsigned long long a10, unsigned long long a11, unsigned long long a12);

void* g_mgr = nullptr;                 // ParticleSystemMgr003 interface pointer
FindSystem_t g_find = nullptr;
CreateBody_t g_origCreateBody = nullptr;
std::atomic<bool> g_installed{ false };
bool g_installFailedHard = false;      // shape mismatch: stop retrying, feature inactive
unsigned long long g_lastInstallTryMs = 0;

// CResourceSystem ("ResourceSystem013") blocking-load pair. Both take the SAME args
// (this, resourceId*, const char* who). vtable +0x140 is the naked single-resource load
// FindParticleSystem falls back to for a non-precached system; +0x148 is
// CResourceSystem::BlockingLoadResourceByNameIntoJustInTimeManifest, which wraps the load
// in a one-resource MANIFEST -- the same pipeline map precache uses, so dependent
// resources load and the definition's strong-handle fields get fixed up. See the
// CRASH LESSON 6 comment on HandleCreateReady for why the difference is fatal.
typedef void* (__fastcall* ResBlockingLoad_t)(void* self, void* resourceId, const char* who);
ResBlockingLoad_t g_origPlainLoad = nullptr;   // detoured: vt+0x140
ResBlockingLoad_t g_jitManifestLoad = nullptr; // called instead while t_jitRedirect: vt+0x148
bool g_jitRedirectInstalled = false;
bool g_jitInstallTried = false;
// Set ONLY around the resolver's FindParticleSystem call (main thread). The detour is a
// strict pass-through for every other caller in the process.
thread_local bool t_jitRedirect = false;

// ============================== apply-now reseek ===================================
// The hook only affects particle systems at CREATION time; systems already alive (and
// frozen on screen while the demo is paused) are untouched by a toggle, which reads as
// "the setting does nothing" / "it stays loaded after I change it". The engine-native
// fix: a tiny backward demo seek destroys every live particle (full update) and replays
// the recent event stream, re-creating the moment's effects UNDER THE NEW RULES -- More,
// On, Less, and Off/default swap/pass-through choices visibly replace each other. Any
// settings change requests this (debounced 400ms so a burst of UI clicks costs one seek);
// the pump performs it only while the demo is PAUSED --
// during playback new effects appear within seconds anyway and a seek would hitch.
// (`fx apply` forces one immediately regardless of pause.) Note surface DECALS (blood
// splatter on walls) are not particles and are unaffected.
std::atomic<unsigned long long> g_applyAtMs{ 0 };

bool DemoIsPlaying() {
	if (!g_pEngineToClient) return false;
	if (auto pDemo = g_pEngineToClient->GetDemoFile()) return pDemo->IsPlayingDemo();
	return false;
}
bool DemoIsPaused() {
	if (!g_pEngineToClient) return false;
	if (auto pDemo = g_pEngineToClient->GetDemoFile()) return pDemo->IsDemoPaused();
	return false;
}
int DemoTick() {
	if (!g_pEngineToClient) return -1;
	if (auto pDemo = g_pEngineToClient->GetDemoFile()) return pDemo->GetDemoTick();
	return -1;
}

void ApplyReseekNow() {
	const int tick = DemoTick();
	if (tick <= 1 || !g_pEngineToClient)
		return;
	// One tick back: guarantees a real backward seek (goal == current can no-op), and
	// ~15ms of position change is imperceptible.
	char cmd[64];
	std::snprintf(cmd, sizeof(cmd), "demo_gototick %d", tick - 1);
	g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.apply", "reseek to %d (paused=%d) to re-create live effects", tick - 1,
			DemoIsPaused() ? 1 : 0);
}

void RequestApplyReseek() {
	if (!DemoIsPlaying())
		return;
	g_applyAtMs.store(GetTickCount64() + 400);
}

// The engine's stock do-nothing system: what "blocked" categories are swapped to, so the
// engine always gets a valid collection back (returning null risks crashing callers).
constexpr const char* kEmptySystem = "particles/dev/empty.vpcf";

// ============================== rules + telemetry ==================================

struct CustomRule {
	std::string match;  // lowercase substring of the full resource path
	std::string target; // exact target resource path to swap to; empty = block (empty system)
};

struct FxEvent {
	std::string name;   // as read off the handle
	char action;        // '=' pass, 'X' blocked, '>' swapped
	std::string target; // for '>'
};

struct NameStat {
	unsigned long long seen = 0;
	unsigned long long acted = 0;
};

struct HandleCacheEntry {
	void* handle = nullptr;
	unsigned long long retryAtMs = 0; // for failed resolves: don't re-resolve until then
};

constexpr size_t kRingCap = 128;
constexpr size_t kNameStatsCap = 8192;

// One mutex guards all mutable rule/telemetry state. Creation frequency is low (dozens per
// second at the wildest), so a plain mutex in the hook is fine; the engine call used for
// swap resolution is made OUTSIDE the lock.
std::mutex g_mx;
bool g_enabled = true;
FxMode g_modes[kFxCategoryCount] = {}; // default FxMode::On
std::vector<CustomRule> g_customRules;
bool g_logging = false;
std::vector<FxEvent> g_ring;           // capped ring, g_ringNext = oldest slot
size_t g_ringNext = 0;
std::map<std::string, NameStat> g_nameStats;
std::unordered_map<std::string, HandleCacheEntry> g_handleCache;
// Swap-target names waiting for MAIN-THREAD resolution (see ResolveHandleByName).
std::vector<std::string> g_resolveQueue;

std::atomic<unsigned long long> g_totalSeen{ 0 };
std::atomic<unsigned long long> g_totalNoName{ 0 };
std::atomic<unsigned long long> g_totalBlocked{ 0 };
std::atomic<unsigned long long> g_totalSwapped{ 0 };

// ============================== guarded memory access ==============================
// The hook dereferences engine-owned structures on whatever thread creates particles.
// Every read is SEH-guarded so a layout change after a CS2 update degrades to "no name ->
// pass through" instead of crashing the game. (Separate functions: MSVC forbids __try in
// functions with C++ objects needing unwinding.)

bool SehReadPtr(const void* addr, void** out) {
	__try {
		*out = *(void* const*)addr;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehCopyStr(const char* src, char* dst, size_t cap) {
	__try {
		size_t i = 0;
		for (; i + 1 < cap; ++i) {
			char c = src[i];
			dst[i] = c;
			if (!c)
				return true;
		}
		dst[cap - 1] = 0;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehReadDword(const void* addr, int* out) {
	__try {
		*out = *(const int*)addr;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehReadBytes(const void* addr, unsigned char* dst, size_t n) {
	__try {
		std::memcpy(dst, addr, n);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool InImage(unsigned char* base, size_t size, const void* p) {
	return p >= base && p < base + size;
}

// A strong handle is CREATE-READY only when the engine's own create-by-name wrapper says
// so (particles.dll 0xA0E78..0xA0E8B, build 14166): data ptr [handle] non-null AND the
// live counter dword [handle+0x20] > 0. CRASH LESSON 5 (2026-07-02, dump 09:23 + the
// impact_concrete->impact_plaster control test): a handle that only passes the null check
// is a header-loaded definition whose CHILD systems were never resolved -- creating from
// it dies at particles.dll+0x3E3xx walking null children. Precached systems pass both.
//
// CRASH LESSON 6 (2026-07-02, mvm_debug_093425: first tracer swap AV'd reading
// 0xFFFFFFFFFFFFFFFF at particles.dll+0x3e36a): the two checks above are NOT sufficient.
// Every top-level create unconditionally walks the definition's fallback strong-handle
// fields at [def+0x308] and [def+0x310] (particles.dll+0x3e33b/+0x3e359, build 14166).
// A definition loaded via the naked blocking load has those fields left UNFIXED (-1),
// and the walk derefs the -1. A properly (manifest-)loaded definition holds 0 or a valid
// binding pointer there. So: reject any definition still carrying a -1 fallback field.
bool HandleCreateReady(void* handle) {
	if (!handle)
		return false;
	void* data = nullptr;
	if (!SehReadPtr(handle, &data) || !data)
		return false;
	int live = 0;
	if (!SehReadDword((unsigned char*)handle + 0x20, &live) || live <= 0)
		return false;
	void* fb = nullptr;
	if (!SehReadPtr((unsigned char*)data + 0x308, &fb) || fb == (void*)~0ull)
		return false;
	if (!SehReadPtr((unsigned char*)data + 0x310, &fb) || fb == (void*)~0ull)
		return false;
	return true;
}

// Resource name off a particle-system strong handle: [handle+8] -> info, [info] -> char*.
// (Matches the engine's own "Attempting to use invalid particle definition \"%s\"" print.)
bool HandleName(void* handle, char* buf, size_t cap) {
	if (!handle)
		return false;
	void* info = nullptr;
	if (!SehReadPtr((unsigned char*)handle + 8, &info) || !info)
		return false;
	void* str = nullptr;
	if (!SehReadPtr(info, &str) || !str)
		return false;
	if (!SehCopyStr((const char*)str, buf, cap))
		return false;
	return buf[0] != 0;
}

// ============================== classification =====================================

void LowerCopy(const char* in, char* out, size_t cap) {
	size_t i = 0;
	for (; in[i] && i + 1 < cap; ++i) {
		char c = in[i];
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		if (c == '\\') c = '/';
		out[i] = c;
	}
	out[i] = 0;
}

bool StartsWith(const char* s, const char* prefix) {
	return 0 == std::strncmp(s, prefix, std::strlen(prefix));
}

// n must already be lowercase (LowerCopy). Returns a FxCategory or -1 (untracked).
// Prefixes verified against the build-14166 pak01 particle list (Source2Viewer dump):
// HE grenades detonate as particles/entity/env_explosion/explosion_hegrenade_*, blood
// also appears under impact_fx/ and as explosions_fx/ screen splatter, and current maps
// (mirage etc.) spawn ambience from the LEGACY particles/maps/de_dust/... names plus the
// generic ambient_fx/environment/rain_fx folders.
int ClassifyLower(const char* n) {
	if (StartsWith(n, "particles/ui/")) return -1;         // HUD/MVP effects: never touch
	if (std::strstr(n, "blood")) return kFxBlood;          // wins over folder rules
	if (StartsWith(n, "particles/impact_fx/") || StartsWith(n, "particles/water_impact/")
		|| StartsWith(n, "particles/breakable_fx/")) return kFxImpacts;
	if (StartsWith(n, "particles/weapons/cs_weapon_fx/") || StartsWith(n, "particles/unified_weapon_fx/"))
		return std::strstr(n, "tracer") ? kFxTracers : kFxWeaponFx;
	if (StartsWith(n, "particles/explosions_fx/") || StartsWith(n, "particles/entity/env_explosion/"))
		return kFxExplosions;
	if (StartsWith(n, "particles/burning_fx/") || StartsWith(n, "particles/inferno_fx")) return kFxMolotov;
	if (StartsWith(n, "particles/maps/") || StartsWith(n, "particles/ambient_fx/")
		|| StartsWith(n, "particles/environment/") || StartsWith(n, "particles/rain_fx/")
		|| StartsWith(n, "particles/critters/")) return kFxMapFx;
	return -1;
}

const char* kCategoryKeys[kFxCategoryCount] = {
	"impacts", "tracers", "weaponfx", "blood", "explosions", "molotov", "mapfx"
};
const char* kModeNames[] = { "on", "less", "off", "more" };
constexpr int kModeCount = 4;

int CategoryFromKey(const char* key) {
	for (int i = 0; i < kFxCategoryCount; ++i)
		if (0 == _stricmp(key, kCategoryKeys[i]))
			return i;
	return -1;
}

int ModeFromName(const char* name) {
	for (int i = 0; i < kModeCount; ++i)
		if (0 == _stricmp(name, kModeNames[i]))
			return i;
	return -1;
}

// Mode targets are real Source 1 Better Particles variants converted to Source 2 with
// long0900/source1import and mounted under particles/filmmaker/betterparticles/.
//
// Mode mapping from the original CS:GO mod folders:
//   On   -> p_betterparticlesmod_classic_c057b
//   More -> p_betterparticlesmod_classic updated_c057b
//   Less -> p_betterparticlesmod_lessimpacts for bullet impacts,
//           p_betterparticlesmod_lesssmoke_22ac2 for smoke/muzzle/blood/fire/explosions.
//
// If the converted pack is not mounted, resolution fails open and the original CS2 effect
// plays. There are no stock CS2 placeholder substitutions here.
struct VariantRule {
	const char* match;
	const char* classic;
	const char* more;
	const char* lessImpacts;
	const char* lessSmoke;
};

#define FXVAR(variant, pack, name) "particles/filmmaker/betterparticles/" variant "/" pack "/" name ".vpcf"
#define FXRULE(match, pack, name) { match, \
	FXVAR("classic", pack, name), FXVAR("classic_updated", pack, name), \
	FXVAR("less_impacts", pack, name), FXVAR("less_smoke", pack, name) }

const VariantRule kVariantBlood[] = {
	FXRULE("particles/blood_impact/blood_impact_basic.vpcf",          "blood_impact", "1.cinematic_blood_impact_v2"),
	FXRULE("particles/blood_impact/blood_impact_light.vpcf",          "blood_impact", "blood_impact_red_01"),
	FXRULE("particles/blood_impact/blood_impact_medium.vpcf",         "blood_impact", "1.cinematic_blood_impact_v2"),
	FXRULE("particles/blood_impact/blood_impact_high.vpcf",           "blood_impact", "blood_impact_heavy"),
	FXRULE("particles/blood_impact/blood_impact_heavy.vpcf",          "blood_impact", "blood_impact_heavy"),
	FXRULE("particles/blood_impact/blood_impact_low.vpcf",            "blood_impact", "blood_impact_red_01"),
	FXRULE("particles/blood_impact/blood_impact_med.vpcf",            "blood_impact", "1.cinematic_blood_impact_v2"),
	FXRULE("particles/blood_impact/blood_impact_friendly.vpcf",       "blood_impact", "blood_impact_red_01"),
	FXRULE("particles/blood_impact/blood_impact_localfrontenemy.vpcf", "blood_impact", "blood_impact_red_01"),
	FXRULE("particles/blood_impact/blood_impact_localplayer.vpcf",    "blood_impact", "blood_impact_red_01"),
	FXRULE("particles/blood_impact/blood_impact_headshot.vpcf",       "blood_impact", "blood_impact_headshot"),
	FXRULE("particles/blood_impact/blood_impact_light_headshot.vpcf", "blood_impact", "blood_impact_light_headshot"),
};
const VariantRule kVariantImpacts[] = {
	FXRULE("particles/impact_fx/impact_concrete.vpcf",    "impact_fx", "impact_concrete"),
	FXRULE("particles/impact_fx/impact_plaster.vpcf",     "impact_fx", "impact_plaster"),
	FXRULE("particles/impact_fx/impact_brick.vpcf",       "impact_fx", "impact_brick"),
	FXRULE("particles/impact_fx/impact_tile.vpcf",        "impact_fx", "impact_tile"),
	FXRULE("particles/impact_fx/impact_sheetrock.vpcf",   "impact_fx", "impact_sheetrock"),
	FXRULE("particles/impact_fx/impact_asphalt.vpcf",     "impact_fx", "impact_asphalt"),
	FXRULE("particles/impact_fx/impact_rock.vpcf",        "impact_fx", "impact_rock"),
	FXRULE("particles/impact_fx/impact_dirt.vpcf",        "impact_fx", "impact_dirt"),
	FXRULE("particles/impact_fx/impact_plastic.vpcf",     "impact_fx", "impact_plastic"),
	FXRULE("particles/impact_fx/impact_metal.vpcf",       "impact_fx", "impact_metal"),
	FXRULE("particles/impact_fx/impact_metal_grate.vpcf", "impact_fx", "impact_metal"),
	FXRULE("particles/impact_fx/impact_metal_vent.vpcf",  "impact_fx", "impact_metal"),
	FXRULE("particles/impact_fx/impact_wood.vpcf",        "impact_fx", "impact_wood"),
	FXRULE("particles/impact_fx/impact_wallbang_light.vpcf",        "impact_fx", "impact_wallbang_light"),
	FXRULE("particles/impact_fx/impact_wallbang_light_silent.vpcf", "impact_fx", "impact_wallbang_light"),
	FXRULE("particles/impact_fx/impact_wallbang_heavy.vpcf",        "impact_fx", "impact_wallbang_heavy"),
	FXRULE("particles/impact_fx/impact_ricochet.vpcf",    "impact_fx", "impact_ricochet"),
};
const VariantRule kVariantTracers[] = {
	FXRULE("particles/weapons/cs_weapon_fx/weapon_tracers.vpcf",              "weapons/cs_weapon_fx", "weapon_tracers"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_tracers_pistol.vpcf",       "weapons/cs_weapon_fx", "weapon_tracers_pistol"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_tracers_smg.vpcf",          "weapons/cs_weapon_fx", "weapon_tracers_smg"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_tracers_rifle.vpcf",        "weapons/cs_weapon_fx", "weapon_tracers_rifle"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_tracers_rifle_scar.vpcf",   "weapons/cs_weapon_fx", "weapon_tracers_rifle"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_tracers_rifle_ssg.vpcf",    "weapons/cs_weapon_fx", "weapon_tracers_rifle"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_tracers_assrifle.vpcf",     "weapons/cs_weapon_fx", "weapon_tracers_assrifle"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_tracers_assrifle_aug.vpcf", "weapons/cs_weapon_fx", "weapon_tracers_assrifle"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_tracers_mach.vpcf",         "weapons/cs_weapon_fx", "weapon_tracers_mach"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_tracers_shot.vpcf",         "weapons/cs_weapon_fx", "weapon_tracers_shot"),
};
const VariantRule kVariantWeaponFx[] = {
	FXRULE("particles/unified_weapon_fx/uweapon_muzflsh_ak47.vpcf",         "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzflsh_ak47_fps.vpcf",     "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_fp"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzflsh_riffle.vpcf",       "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzflsh_riffle_fps.vpcf",   "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_fp"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzflsh_aug.vpcf",          "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzflsh_aug_fps.vpcf",      "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_fp"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzflsh_shot.vpcf",         "weapons/cs_weapon_fx", "weapon_muzzle_flash_shotgun"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzflsh_shot_fps.vpcf",     "weapons/cs_weapon_fx", "weapon_muzzle_flash_shotgun_fp"),
	FXRULE("particles/unified_weapon_fx/weapon_muzzleflash_snip.vpcf",      "weapons/cs_weapon_fx", "weapon_muzzle_flash_awp"),
	FXRULE("particles/unified_weapon_fx/weapon_muzzleflash_snip_fps.vpcf",  "weapons/cs_weapon_fx", "weapon_muzzle_flash_huntingrifle_fp"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzzleflash_subm.vpcf",     "weapons/cs_weapon_fx", "weapon_muzzle_flash_smg"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzzleflash_subm_fps.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_flash_smg_fp"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzsilenced_subm.vpcf",     "weapons/cs_weapon_fx", "weapon_muzzle_flash_smg_silenced"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzsilenced_subm_fps.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_flash_smg_silenced_fp"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzsilenced_rif.vpcf",      "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_silenced"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzsilenced_rif_fps.vpcf",  "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_silenced"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzzleflash_pist.vpcf",     "weapons/cs_weapon_fx", "weapon_muzzle_flash_pistol"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzzleflash_pist_fps.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_flash_pistol_fp"),
	// Deagle/R8, ironsight (scoped) AUG/SG556, and LMG muzzles are separate
	// top-level systems (verified via `fx names` on a live demo, 2026-07-02).
	FXRULE("particles/unified_weapon_fx/uweapon_muzflsh_deagle.vpcf",            "weapons/cs_weapon_fx", "weapon_muzzle_flash_pistol"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzflsh_deagle_fps.vpcf",        "weapons/cs_weapon_fx", "weapon_muzzle_flash_pistol_fp"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzflsh_aug_fps_ironsight.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_fp"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzflsh_sg_fps_ironsight.vpcf",  "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_fp"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzflsh_riffle_lrg.vpcf",        "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzflsh_riffle_lrg_fps.vpcf",    "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle_fp"),
	// R8 primary fire ("fanning") uses its own pist_revolver systems, distinct
	// from the deagle-shared secondary fire.
	FXRULE("particles/unified_weapon_fx/uweapon_muzzleflash_pist_revolver.vpcf",      "weapons/cs_weapon_fx", "weapon_muzzle_flash_pistol"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzzleflash_pist_revolver_fps.vpcf",  "weapons/cs_weapon_fx", "weapon_muzzle_flash_pistol_fp"),
	FXRULE("particles/unified_weapon_fx/uweapon_muzzleflash_pist_fire_revolver.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_flash_pistol"),
	// Sustained-fire barrel smoke ("line trace following the barrel"): top-level
	// systems the game spawns after continued fire; the mod ships its own set.
	FXRULE("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke.vpcf",             "weapons/cs_weapon_fx", "weapon_muzzle_smoke"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_b.vpcf",           "weapons/cs_weapon_fx", "weapon_muzzle_smoke_b"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_b_version_2.vpcf", "weapons/cs_weapon_fx", "weapon_muzzle_smoke_b_version_#2"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_long.vpcf",        "weapons/cs_weapon_fx", "weapon_muzzle_smoke_long"),
	FXRULE("particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_long_b.vpcf",      "weapons/cs_weapon_fx", "weapon_muzzle_smoke_long_b"),
	FXRULE("particles/unified_weapon_fx/weapon_muzzleflash_basic.vpcf",     "weapons/cs_weapon_fx", "weapon_muzzle_flash_assaultrifle"),
};
const VariantRule kVariantExplosions[] = {
	FXRULE("particles/explosions_fx/explosion_basic.vpcf",              "explosions_fx", "explosion_basic"),
	FXRULE("particles/explosions_fx/explosion_hegrenade_brief.vpcf",    "explosions_fx", "explosion_hegrenade_interior"),
	FXRULE("particles/entity/env_explosion/explosion_hegrenade_a.vpcf", "explosions_fx", "explosion_hegrenade_interior"),
	FXRULE("particles/entity/env_explosion/explosion_hegrenade_b.vpcf", "explosions_fx", "explosion_hegrenade_interior"),
	FXRULE("particles/entity/env_explosion/explosion_hegrenade_c.vpcf", "explosions_fx", "explosion_hegrenade_interior"),
	FXRULE("particles/entity/env_explosion/explosion_hegrenade_d.vpcf", "explosions_fx", "explosion_hegrenade_interior"),
	FXRULE("particles/entity/env_explosion/explosion_hegrenade_e.vpcf", "explosions_fx", "explosion_hegrenade_interior"),
	FXRULE("particles/entity/env_explosion/explosion_hegrenade_f.vpcf", "explosions_fx", "explosion_hegrenade_interior"),
	FXRULE("particles/entity/env_explosion/explosion_hegrenade_g.vpcf", "explosions_fx", "explosion_hegrenade_interior"),
	FXRULE("particles/entity/env_explosion/explosion_hegrenade_h.vpcf", "explosions_fx", "explosion_hegrenade_interior"),
};
const VariantRule kVariantMolotov[] = {
	FXRULE("particles/inferno_fx/molotov_groundfire.vpcf",    "inferno_fx", "molotov_groundfire_00high"),
	FXRULE("particles/inferno_fx/incendiary_groundfire.vpcf", "inferno_fx", "molotov_groundfire_00high"),
	FXRULE("particles/inferno_fx/molotov_fire01.vpcf",        "inferno_fx", "molotov_fire01"),
	FXRULE("particles/inferno_fx/molotov_explosion.vpcf",     "inferno_fx", "molotov_explosion"),
	FXRULE("particles/inferno_fx/incendiary_explosion.vpcf",  "inferno_fx", "molotov_explosion"),
};

#undef FXRULE
#undef FXVAR

const char* SelectVariantTarget(int cat, FxMode mode, const VariantRule& rule) {
	switch (mode) {
	case FxMode::On:
		return rule.classic;
	case FxMode::More:
		return rule.more;
	case FxMode::Less:
		return cat == kFxImpacts ? rule.lessImpacts : rule.lessSmoke;
	default:
		return nullptr;
	}
}

const char* VariantTargetLower(int cat, FxMode mode, const char* n) {
	if (mode == FxMode::Off)
		return nullptr;
	const VariantRule* rules = nullptr;
	size_t count = 0;
	switch (cat) {
	case kFxWeaponFx:   rules = kVariantWeaponFx;   count = sizeof(kVariantWeaponFx) / sizeof(rules[0]); break;
	case kFxTracers:    rules = kVariantTracers;    count = sizeof(kVariantTracers) / sizeof(rules[0]); break;
	case kFxBlood:      rules = kVariantBlood;      count = sizeof(kVariantBlood) / sizeof(rules[0]); break;
	case kFxExplosions: rules = kVariantExplosions; count = sizeof(kVariantExplosions) / sizeof(rules[0]); break;
	case kFxImpacts:    rules = kVariantImpacts;    count = sizeof(kVariantImpacts) / sizeof(rules[0]); break;
	case kFxMolotov:    rules = kVariantMolotov;    count = sizeof(kVariantMolotov) / sizeof(rules[0]); break;
	default: return nullptr;
	}
	for (size_t i = 0; i < count; ++i)
		if (0 == std::strcmp(n, rules[i].match))
			return SelectVariantTarget(cat, mode, rules[i]);
	return nullptr;
}

// Money-on-headshot: event-gated optional swap for the mod's money headshot effect. The
// Source 1 mod implements this by replacing impact_fx.pcf with impact_fxmoney.pcf. Do not
// point this at CS2's stock moneycrate burst; a converted asset pack should provide this
// resource path, and until then the lookup fails open to the original particle.
constexpr const char* kMoneyBurst = "particles/filmmaker/betterparticles/classic/impact_fxmoney/impact_helmet_headshot.vpcf";
bool g_moneyHeadshot = false; // guarded by g_mx
std::atomic<unsigned int> g_moneyHeadshotBudget{ 0 };
std::atomic<unsigned long long> g_lastMoneyHeadshotEventMs{ 0 };
std::atomic<unsigned long long> g_lastMoneyHeadshotSignature{ 0 };
std::atomic<unsigned long long> g_lastMoneyHeadshotSignatureMs{ 0 };
constexpr unsigned long long kMoneyHeadshotWindowMs = 1500;
constexpr unsigned long long kMoneyHeadshotDuplicateMs = 75;
constexpr unsigned int kMoneyHeadshotParticlesPerEvent = 2;
bool IsMoneyHeadshotCandidateLower(const char* n) {
	return 0 == std::strcmp(n, "particles/blood_impact/blood_impact_headshot.vpcf")
		|| 0 == std::strcmp(n, "particles/blood_impact/blood_impact_light_headshot.vpcf")
		|| 0 == std::strcmp(n, "particles/impact_fx/impact_helmet_headshot.vpcf");
}

bool ConsumeRecentMoneyHeadshotEvent() {
	const unsigned long long last = g_lastMoneyHeadshotEventMs.load(std::memory_order_relaxed);
	if (!last || GetTickCount64() - last > kMoneyHeadshotWindowMs)
		return false;
	unsigned int cur = g_moneyHeadshotBudget.load(std::memory_order_relaxed);
	while (cur > 0) {
		if (g_moneyHeadshotBudget.compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed))
			return true;
	}
	return false;
}

unsigned long long MixMoneyHeadshotSignature(unsigned long long h, unsigned int v) {
	h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
	return h ? h : 0xCBF29CE484222325ull;
}

bool MoneyHeadshotEventIsDuplicate(unsigned long long signature) {
	const unsigned long long now = GetTickCount64();
	const unsigned long long lastSignature = g_lastMoneyHeadshotSignature.load(std::memory_order_relaxed);
	const unsigned long long lastMs = g_lastMoneyHeadshotSignatureMs.load(std::memory_order_relaxed);
	if (signature == lastSignature && lastMs && now - lastMs <= kMoneyHeadshotDuplicateMs)
		return true;
	g_lastMoneyHeadshotSignature.store(signature, std::memory_order_relaxed);
	g_lastMoneyHeadshotSignatureMs.store(now, std::memory_order_relaxed);
	return false;
}

// ============================== swap-handle resolution =============================

// CRASH LESSON (2026-07-02, two access-violation dumps): swap-target names must NEVER be
// resolved from inside the create-collection hook. FindParticleSystem's "not precached ->
// blocking load" path re-enters the resource/particle system mid-create on whatever thread
// is creating particles; that survived for the tiny always-resident dev/empty.vpcf but
// crashed the first time a "More" target (a real, unloaded system) had to load. So the
// hook is CACHE-HIT-ONLY: a miss fails open (original effect plays
// once) and queues the name; the main-thread pump resolves it in the engine's own precache
// context, and the next creation swaps.

// The detour on CResourceSystem's plain blocking load (vt+0x140). While the resolver's
// FindParticleSystem call is on this thread's stack, the load is upgraded to the
// JIT-manifest load (vt+0x148, identical signature) so the particle definition arrives
// with its dependencies loaded and its resource-handle fields fixed up instead of -1.
// Every other caller (models, materials, other threads) passes through untouched.
void* __fastcall Hook_PlainBlockingLoad(void* self, void* resourceId, const char* who) {
	if (t_jitRedirect && g_jitManifestLoad) {
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("fx.resolve", "plain blocking load redirected to JIT manifest load (%s)",
				who ? who : "?");
		return g_jitManifestLoad(self, resourceId, who);
	}
	return g_origPlainLoad(self, resourceId, who);
}

// Canary: does fn's body (first scanLen bytes) contain a rip-relative lea to a string
// starting with prefix? Confirms a vtable slot still holds the function we RE'd after a
// CS2 update reshuffles things. SEH-guarded byte reads throughout.
bool FnReferencesString(const void* fn, size_t scanLen, const char* prefix) {
	unsigned char buf[0x800];
	if (scanLen > sizeof(buf))
		scanLen = sizeof(buf);
	if (!SehReadBytes(fn, buf, scanLen))
		return false;
	const size_t prefLen = std::strlen(prefix);
	for (size_t i = 0; i + 7 <= scanLen; ++i) {
		// 48/49/4C/4D 8D modrm(mod00 rm101): lea r64, [rip+disp32]
		if ((buf[i] & 0xF8) != 0x48 || buf[i + 1] != 0x8D || (buf[i + 2] & 0xC7) != 0x05)
			continue;
		int disp = 0;
		std::memcpy(&disp, buf + i + 3, 4);
		const char* s = (const char*)fn + i + 7 + disp;
		char str[96];
		if (SehCopyStr(s, str, sizeof(str)) && 0 == std::strncmp(str, prefix, prefLen))
			return true;
	}
	return false;
}

// Install the plain->JIT blocking-load redirect on CResourceSystem. Failure is NON-fatal:
// the feature degrades to "swap targets must be precached by the demo" (the
// HandleCreateReady gate keeps that safe), it does not crash or disable the hook.
void TryInstallJitRedirect() {
	if (g_jitInstallTried)
		return;
	g_jitInstallTried = true;

	HMODULE mod = GetModuleHandleA("resourcesystem.dll");
	if (!mod) {
		advancedfx::Warning("fx: resourcesystem.dll not loaded; swap targets limited to demo-precached systems.\n");
		return;
	}
	unsigned char* base = (unsigned char*)mod;
	const int ntOff = *(const int*)(base + 0x3C);
	const size_t imageSize = *(const unsigned int*)(base + ntOff + 0x50);

	typedef void* (*CreateInterface_t)(const char*, int*);
	CreateInterface_t ci = (CreateInterface_t)GetProcAddress(mod, "CreateInterface");
	void* rs = ci ? ci("ResourceSystem013", nullptr) : nullptr;
	void** vt = nullptr;
	if (!rs || !SehReadPtr(rs, (void**)&vt) || !InImage(base, imageSize, vt)) {
		advancedfx::Warning("fx: ResourceSystem013 not resolvable; swap targets limited to demo-precached systems.\n");
		return;
	}
	void* plain = nullptr; void* jit = nullptr;
	SehReadPtr(vt + 0x140 / 8, &plain); // naked blocking load (FindParticleSystem's fallback)
	SehReadPtr(vt + 0x148 / 8, &jit);   // BlockingLoadResourceByNameIntoJustInTimeManifest
	if (!InImage(base, imageSize, plain) || !InImage(base, imageSize, jit)
		|| !FnReferencesString(jit, 0x600, "CResourceSystem::BlockingLoadResourceByName")) {
		advancedfx::Warning("fx: resource-system blocking-load slots moved (CS2 update?); "
			"swap targets limited to demo-precached systems.\n");
		return;
	}

	g_origPlainLoad = (ResBlockingLoad_t)plain;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_origPlainLoad, Hook_PlainBlockingLoad);
	if (NO_ERROR != DetourTransactionCommit()) {
		g_origPlainLoad = nullptr;
		advancedfx::Warning("fx: blocking-load detour failed; swap targets limited to demo-precached systems.\n");
		return;
	}
	g_jitManifestLoad = (ResBlockingLoad_t)jit;
	g_jitRedirectInstalled = true;
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.install", "jit redirect armed: rs=%p plain=%p jit=%p", rs, plain, jit);
}

// MAIN THREAD ONLY. Resolves via the manager's FindParticleSystem (owns the blocking-load
// fallback, safe here). Failed resolves are cached for 5s; success is cached forever (the
// manager's name registry keeps the handle alive for the process lifetime).
void ResolveHandleOnMainThread(const char* name) {
	{
		std::lock_guard<std::mutex> lock(g_mx);
		auto it = g_handleCache.find(name);
		if (it != g_handleCache.end()) {
			if (it->second.handle)
				return;
			if (GetTickCount64() < it->second.retryAtMs)
				return;
		}
	}
	void* h = nullptr;
	int live = -1;
	void* fb308 = nullptr; void* fb310 = nullptr;
	if (g_find && g_mgr) {
		void* out[4] = {};
		// Upgrade any blocking load this triggers to the JIT-manifest load (dependencies
		// + handle fixup) -- see Hook_PlainBlockingLoad. Without it, a non-precached
		// system loads with -1 fallback fields and creating from it crashes the engine.
		t_jitRedirect = true;
		void* r = g_find(g_mgr, out, name, 1);
		t_jitRedirect = false;
		if (r)
			SehReadPtr(r, &h);
		if (!h)
			h = out[0];
		if (h) {
			SehReadDword((unsigned char*)h + 0x20, &live);
			void* data = nullptr;
			if (SehReadPtr(h, &data) && data) {
				SehReadPtr((unsigned char*)data + 0x308, &fb308);
				SehReadPtr((unsigned char*)data + 0x310, &fb310);
			}
		}
		// Full create-ready gate (null data / live<=0 / -1 fallback fields): a handle
		// that is not fully loaded+fixed-up must NOT be cached as usable.
		if (h && !HandleCreateReady(h))
			h = nullptr;
	}
	{
		std::lock_guard<std::mutex> lock(g_mx);
		HandleCacheEntry& e = g_handleCache[name];
		e.handle = h;
		// Failed/not-ready resolves retry (the system may finish loading as the demo
		// streams in); ready handles are refreshed only on level change.
		e.retryAtMs = h ? 0 : GetTickCount64() + 5000;
	}
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.resolve", "%s -> %p live=%d fb=%p/%p jit=%d %s", name, h, live,
			fb308, fb310, g_jitRedirectInstalled ? 1 : 0, h ? "READY" : "not-ready");
}

// Any thread. Cache hit or nothing: on a miss (or an expired failed resolve) the name is
// queued for the main-thread pump and null is returned (caller fails open).
void* LookupHandleOrQueue(const char* name) {
	std::lock_guard<std::mutex> lock(g_mx);
	auto it = g_handleCache.find(name);
	if (it != g_handleCache.end()) {
		if (it->second.handle)
			return it->second.handle;
		if (GetTickCount64() < it->second.retryAtMs)
			return nullptr; // failed resolve cooling down
		g_handleCache.erase(it); // cooldown over: fall through and requeue
	}
	for (const std::string& q : g_resolveQueue)
		if (q == name)
			return nullptr;
	g_resolveQueue.push_back(name);
	return nullptr;
}

// Queue every name current settings could swap to, so targets are (re)resolved ahead of
// the first creation that needs them. Called on install and on every settings change.
void QueueActiveSwapTargetsLocked() {
	auto add = [](const char* n) {
		if (g_handleCache.find(n) != g_handleCache.end())
			return;
		for (const std::string& q : g_resolveQueue)
			if (q == n)
				return;
		g_resolveQueue.push_back(n);
	};
	add(kEmptySystem);
	if (g_moneyHeadshot)
		add(kMoneyBurst);
	for (const CustomRule& r : g_customRules)
		if (!r.target.empty())
			add(r.target.c_str());
	const VariantRule* tables[] = { kVariantWeaponFx, kVariantTracers, kVariantBlood,
		kVariantExplosions, kVariantImpacts, kVariantMolotov };
	const size_t counts[] = {
		sizeof(kVariantWeaponFx) / sizeof(VariantRule), sizeof(kVariantTracers) / sizeof(VariantRule),
		sizeof(kVariantBlood) / sizeof(VariantRule), sizeof(kVariantExplosions) / sizeof(VariantRule),
		sizeof(kVariantImpacts) / sizeof(VariantRule), sizeof(kVariantMolotov) / sizeof(VariantRule)
	};
	const FxCategory cats[] = { kFxWeaponFx, kFxTracers, kFxBlood, kFxExplosions, kFxImpacts, kFxMolotov };
	for (int t = 0; t < 6; ++t) {
		const FxMode mode = g_modes[cats[t]];
		if (mode == FxMode::Off)
			continue;
		for (size_t i = 0; i < counts[t]; ++i)
			if (const char* target = SelectVariantTarget(cats[t], mode, tables[t][i]))
				add(target);
	}
}

void QueueActiveSwapTargets() {
	std::lock_guard<std::mutex> lock(g_mx);
	QueueActiveSwapTargetsLocked();
}

// ============================== telemetry helpers (g_mx held) ======================

void RecordEventLocked(const char* name, char action, const char* target) {
	if (g_ring.size() < kRingCap) {
		g_ring.push_back({ name, action, target ? target : "" });
	} else {
		g_ring[g_ringNext] = { name, action, target ? target : "" };
		g_ringNext = (g_ringNext + 1) % kRingCap;
	}
}

void RecordNameLocked(const char* lowName, bool acted) {
	if (g_nameStats.size() >= kNameStatsCap && g_nameStats.find(lowName) == g_nameStats.end())
		return;
	NameStat& s = g_nameStats[lowName];
	++s.seen;
	if (acted)
		++s.acted;
}

// ============================== the hook ===========================================

void* __fastcall Hook_CreateBody(unsigned long long thisOrFlag, void* handle, void* a3, void* a4,
	unsigned long long a5, unsigned long long a6, unsigned long long a7, unsigned long long a8,
	unsigned long long a9, unsigned long long a10, unsigned long long a11, unsigned long long a12) {
	void* useHandle = handle;
	char raw[260];
	if (handle && HandleName(handle, raw, sizeof(raw))) {
		g_totalSeen.fetch_add(1, std::memory_order_relaxed);
		char low[260];
		LowerCopy(raw, low, sizeof(low));

		char action = '=';
		std::string target;
		bool logging;
		{
			std::lock_guard<std::mutex> lock(g_mx);
			logging = g_logging;
			if (g_enabled) {
				// Custom rules outrank category modes (they're the power-user override).
				for (const CustomRule& r : g_customRules) {
					if (std::strstr(low, r.match.c_str())) {
						action = r.target.empty() ? 'X' : '>';
						target = r.target;
						break;
					}
				}
				if (action == '=' && g_moneyHeadshot && IsMoneyHeadshotCandidateLower(low)
					&& ConsumeRecentMoneyHeadshotEvent()) {
					action = '>';
					target = kMoneyBurst;
				}
				if (action == '=') {
					const int cat = ClassifyLower(low);
					if (cat >= 0) {
						const FxMode m = g_modes[cat];
						if (m != FxMode::Off) {
							if (const char* t = VariantTargetLower(cat, m, low)) {
								action = '>';
								target = t;
							}
						}
					}
				}
			}
		}

		if (action == 'X')
			target = kEmptySystem;
		if (action != '=') {
			// Cache-hit-only: resolving here can trigger a blocking load inside the
			// create path (crashes -- see ResolveHandleOnMainThread). A miss queues the
			// name for the main-thread pump and this one creation plays unmodified.
			void* nh = LookupHandleOrQueue(target.c_str());
			// Last-line probe: the engine's full create-ready gate, re-checked at use time.
			if (nh && !HandleCreateReady(nh))
				nh = nullptr;
			if (nh && nh != handle) {
				useHandle = nh;
				if (action == 'X') g_totalBlocked.fetch_add(1, std::memory_order_relaxed);
				else g_totalSwapped.fetch_add(1, std::memory_order_relaxed);
			} else {
				action = '='; // fail-open: original effect plays
			}
		}

		{
			std::lock_guard<std::mutex> lock(g_mx);
			RecordNameLocked(low, action != '=');
			if (logging || action != '=')
				RecordEventLocked(raw, action, target.c_str());
		}
		// Flight recorder (thread-safe; same pattern as the knife animfix breadcrumbs).
		if (MvmDebugLog_Active() && (logging || action != '=')) {
			if (action == '=')
				MvmDebugLog_Linef("fx.create", "= %s", raw);
			else
				MvmDebugLog_Linef("fx.create", "%c %s -> %s", action, raw, target.c_str());
		}
	} else if (handle) {
		g_totalNoName.fetch_add(1, std::memory_order_relaxed);
	}
	return g_origCreateBody(thisOrFlag, useHandle, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
}

// ============================== install ============================================

void WarnOnceInstallFailed(const char* why) {
	g_installFailedHard = true;
	advancedfx::Warning("fx: particle hook NOT installed (%s). Effects play unmodified; "
		"a CS2 update likely changed CParticleSystemMgr -- the toggles need a mod update.\n", why);
}

bool TryInstall() {
	if (g_installed.load())
		return true;
	if (g_installFailedHard)
		return false;

	HMODULE mod = GetModuleHandleA("particles.dll");
	if (!mod)
		return false; // not loaded yet; pump retries later
	unsigned char* base = (unsigned char*)mod;
	// Image size straight from the PE optional header (no psapi dependency).
	const int ntOff = *(const int*)(base + 0x3C);
	const size_t imageSize = *(const unsigned int*)(base + ntOff + 0x50);

	typedef void* (*CreateInterface_t)(const char*, int*);
	CreateInterface_t ci = (CreateInterface_t)GetProcAddress(mod, "CreateInterface");
	if (!ci) { WarnOnceInstallFailed("no CreateInterface export"); return false; }
	void* mgr = ci("ParticleSystemMgr003", nullptr);
	if (!mgr) { WarnOnceInstallFailed("ParticleSystemMgr003 interface not found"); return false; }

	void** vt = nullptr;
	if (!SehReadPtr(mgr, (void**)&vt) || !InImage(base, imageSize, vt)) {
		WarnOnceInstallFailed("manager vtable outside particles.dll");
		return false;
	}
	void* find = nullptr; void* stub = nullptr; void* nameCreate = nullptr;
	SehReadPtr(vt + 15, &find);      // FindParticleSystem
	SehReadPtr(vt + 17, &stub);      // create-by-handle entry stub
	SehReadPtr(vt + 18, &nameCreate);// create-by-name (unused; validated as a slot-order canary)
	if (!InImage(base, imageSize, find) || !InImage(base, imageSize, stub)
		|| !InImage(base, imageSize, nameCreate)) {
		WarnOnceInstallFailed("vtable slots out of image (slot order changed?)");
		return false;
	}

	// The +0x88 slot is a tiny stub: `xor ecx,ecx` then a tail-jmp into the shared body.
	// Detour the STUB, NOT the body. CRASH LESSON 3 (2026-07-02, dumps 084146..090524):
	// detouring the body crashed in-demo even in pure pass-through (swapped=0) -- the
	// body's INTERNAL callers (child-system creation, i.e. every composite combat effect)
	// use an LTCG custom register convention a C detour cannot preserve, so children were
	// created with trashed volatile-register state and the child-walk loop died at
	// particles.dll+0x3E3xx. The stub is a real vtable method with a guaranteed standard
	// convention, and the internal child path bypassing us is FINE: we only ever classify
	// top-level systems (children of a swapped/blocked parent never spawn anyway).
	// The stub-shape canary below just confirms the slot still looks right after updates.
	unsigned char sb[0x20] = {};
	void* hookTarget = stub;
	if (!SehReadBytes(stub, sb, sizeof(sb)) || !((sb[0] == 0x33 || sb[0] == 0x31) && sb[1] == 0xC9)) {
		WarnOnceInstallFailed("create stub shape unrecognized (not xor ecx,ecx)");
		return false;
	}

	g_origCreateBody = (CreateBody_t)hookTarget;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_origCreateBody, Hook_CreateBody);
	if (NO_ERROR != DetourTransactionCommit()) {
		g_origCreateBody = nullptr;
		WarnOnceInstallFailed("DetourTransactionCommit failed");
		return false;
	}
	g_mgr = mgr;
	g_find = (FindSystem_t)find;
	TryInstallJitRedirect();  // upgrade resolver blocking loads to manifest loads
	QueueActiveSwapTargets(); // pre-resolve targets on the main-thread pump
	g_installed.store(true);
	advancedfx::Message("fx: particle hook installed (create-collection %s detoured).\n",
		hookTarget == stub ? "stub" : "body");
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.install", "mgr=%p vt=%p find=%p stub=%p target=%p",
			mgr, (void*)vt, find, stub, hookTarget);
	return true;
}

// ============================== persistence ========================================

std::wstring SettingsPath() {
	std::wstring path = GetHlaeRoamingAppDataFolderW();
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path += L'\\';
	path += L"filmmaker_fx.json";
	return path;
}

typedef bool (*SaveKV3AsJSON_t)(const SOURCESDK::CS2::KeyValues3* kv,
	SOURCESDK::CS2::CUtlString* error, SOURCESDK::CS2::CUtlString* output);

SaveKV3AsJSON_t GetSaveKV3AsJSON() {
	static bool s_tried = false;
	static SaveKV3AsJSON_t s_fn = nullptr;
	if (!s_tried) {
		s_tried = true;
		HMODULE tier0 = GetModuleHandleA("tier0.dll");
		if (tier0)
			s_fn = (SaveKV3AsJSON_t)GetProcAddress(tier0, "?SaveKV3AsJSON@@YA_NPEBVKeyValues3@@PEAVCUtlString@@1@Z");
	}
	return s_fn;
}

bool GameEventDataToJson(SOURCESDK::CS2::CGameEvent* event, JsonValue& out) {
	SaveKV3AsJSON_t fn = GetSaveKV3AsJSON();
	if (!fn)
		return false;
	SOURCESDK::CS2::CUtlString error;
	SOURCESDK::CS2::CUtlString text;
	if (!fn(event->GetDataKeys(), &error, &text) || !text.Get())
		return false;
	return JsonParse(text.Get(), out) && out.type == JsonValue::Type::Object;
}

int JsonEventInt(const JsonValue& root, const char* key, int def = 0) {
	if (const JsonValue* v = root.Find(key))
		return v->AsInt(def);
	return def;
}

bool JsonEventBool(const JsonValue& root, const char* key, bool def = false) {
	if (const JsonValue* v = root.Find(key))
		return v->AsBool(def) || v->AsInt(def ? 1 : 0) != 0;
	return def;
}

bool JsonEventHasPositiveInt(const JsonValue& root, const char* key) {
	if (const JsonValue* v = root.Find(key))
		return v->AsInt(0) > 0;
	return false;
}

struct MoneyHeadshotGateEvent {
	int userid = 0;
	int attacker = 0;
	int hitgroup = 0;
	int dmgHealth = 0;
	bool death = false;
	unsigned long long signature = 0;
};

bool GameEventIsValidMoneyHeadshot(SOURCESDK::CS2::CGameEvent* event, const char* name, MoneyHeadshotGateEvent& out) {
	JsonValue root;
	if (!GameEventDataToJson(event, root))
		return false;

	out.userid = JsonEventInt(root, "userid");
	out.attacker = JsonEventInt(root, "attacker");
	if (out.userid <= 0 || out.attacker <= 0 || out.userid == out.attacker)
		return false;

	bool isHeadshot = false;
	if (0 == _stricmp(name, "player_death")) {
		out.death = true;
		isHeadshot = JsonEventBool(root, "headshot");
	} else if (0 == _stricmp(name, "player_hurt")) {
		out.hitgroup = JsonEventInt(root, "hitgroup");
		out.dmgHealth = JsonEventInt(root, "dmg_health");
		isHeadshot = JsonEventBool(root, "headshot") || out.hitgroup == 1;
		if (!JsonEventHasPositiveInt(root, "dmg_health"))
			return false;
	} else {
		return false;
	}
	if (!isHeadshot)
		return false;

	unsigned long long h = 0xCBF29CE484222325ull;
	h = MixMoneyHeadshotSignature(h, out.death ? 1u : 0u);
	h = MixMoneyHeadshotSignature(h, (unsigned int)out.userid);
	h = MixMoneyHeadshotSignature(h, (unsigned int)out.attacker);
	h = MixMoneyHeadshotSignature(h, (unsigned int)out.hitgroup);
	h = MixMoneyHeadshotSignature(h, (unsigned int)out.dmgHealth);
	out.signature = h;
	return true;
}

} // namespace

// ============================== public API =========================================

void ParticleFx_OnGameEvent(SOURCESDK::CS2::CGameEvent* event) {
	if (!event)
		return;
	const char* name = event->GetName();
	if (!name)
		return;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		if (!g_enabled || !g_moneyHeadshot)
			return;
	}
	if (0 != _stricmp(name, "player_hurt") && 0 != _stricmp(name, "player_death"))
		return;

	MoneyHeadshotGateEvent gate;
	if (!GameEventIsValidMoneyHeadshot(event, name, gate))
		return;
	if (MoneyHeadshotEventIsDuplicate(gate.signature))
		return;

	g_lastMoneyHeadshotEventMs.store(GetTickCount64(), std::memory_order_relaxed);
	g_moneyHeadshotBudget.store(kMoneyHeadshotParticlesPerEvent, std::memory_order_relaxed);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.event", "moneyshot gate armed by %s userid=%d attacker=%d hitgroup=%d dmg_health=%d",
			name, gate.userid, gate.attacker, gate.hitgroup, gate.dmgHealth);
}

ParticleFx& ParticleFxRef() {
	static ParticleFx s_instance;
	return s_instance;
}

const char* FxModeName(FxMode mode) { return kModeNames[(int)mode < kModeCount ? (int)mode : 0]; }
const char* FxCategoryKey(FxCategory cat) {
	return (cat >= 0 && cat < kFxCategoryCount) ? kCategoryKeys[cat] : "?";
}

bool ParticleFx::EnsureInstalled() { return TryInstall(); }
bool ParticleFx::Installed() const { return g_installed.load(); }

bool ParticleFx::WantsHook() const {
	std::lock_guard<std::mutex> lock(g_mx);
	if (g_logging)
		return true;
	return g_enabled;
}

bool ParticleFx::MoneyHeadshot() const {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_moneyHeadshot;
}

void ParticleFx::SetMoneyHeadshot(bool on) {
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_moneyHeadshot = on;
		QueueActiveSwapTargetsLocked();
	}
	if (on)
		EnsureInstalled();
	SaveSettings();
	RequestApplyReseek();
}

void ParticleFx::PumpMainThread() {
	if (g_installed.load()) {
		// CRASH LESSON 4 (2026-07-02, mvm_debug_091656 caught it): handles resolved at
		// the MAIN MENU dangle once a demo loads (map transition purges resources); the
		// first swap then hands the engine freed memory and its child-walk dies at
		// particles.dll+0x3E3xx. So the cache is tied to the level: any demo/level
		// change drops every cached handle and re-resolves in the NEW resource context.
		static std::wstring s_lastDemoPath; // main-thread only
		const std::wstring cur = PlayingDemoPath();
		if (cur != s_lastDemoPath) {
			s_lastDemoPath = cur;
			std::lock_guard<std::mutex> lock(g_mx);
			g_handleCache.clear();
			g_resolveQueue.clear();
			QueueActiveSwapTargetsLocked();
			if (MvmDebugLog_Active())
				MvmDebugLog_Linef("fx.resolve", "level change -> handle cache cleared, %zu target(s) requeued",
					g_resolveQueue.size());
		}
		// Drain swap-target resolves here, in the engine's own precache context. One per
		// frame: a cold resolve blocking-loads the resource and can hitch.
		std::string name;
		{
			std::lock_guard<std::mutex> lock(g_mx);
			if (!g_resolveQueue.empty()) {
				name = g_resolveQueue.front();
				g_resolveQueue.erase(g_resolveQueue.begin());
			}
		}
		if (!name.empty())
			ResolveHandleOnMainThread(name.c_str());
		// Debounced apply-now: re-create the paused moment's live effects under the new
		// rules (see the g_applyAtMs comment). Skipped while playing -- self-corrects.
		const unsigned long long applyAt = g_applyAtMs.load();
		if (applyAt && GetTickCount64() >= applyAt) {
			g_applyAtMs.store(0);
			if (DemoIsPlaying() && DemoIsPaused())
				ApplyReseekNow();
		}
		return;
	}
	if (g_installFailedHard)
		return;
	if (!WantsHook())
		return;
	const unsigned long long now = GetTickCount64();
	if (now - g_lastInstallTryMs < 3000)
		return;
	g_lastInstallTryMs = now;
	TryInstall();
}

bool ParticleFx::Enabled() const {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_enabled;
}

void ParticleFx::SetEnabled(bool on) {
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_enabled = on;
	}
	if (on)
		EnsureInstalled();
	SaveSettings();
	RequestApplyReseek();
}

FxMode ParticleFx::Mode(FxCategory cat) const {
	if (cat < 0 || cat >= kFxCategoryCount)
		return FxMode::On;
	std::lock_guard<std::mutex> lock(g_mx);
	return g_modes[cat];
}

void ParticleFx::SetMode(FxCategory cat, FxMode mode) {
	if (cat < 0 || cat >= kFxCategoryCount)
		return;
	bool enabled = false;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_modes[cat] = mode;
		QueueActiveSwapTargetsLocked();
		enabled = g_enabled;
	}
	if (enabled)
		EnsureInstalled();
	SaveSettings();
	RequestApplyReseek();
}

void ParticleFx::SetLogging(bool on) {
	{
		std::lock_guard<std::mutex> lock(g_mx);
		g_logging = on;
	}
	if (on)
		EnsureInstalled();
}

bool ParticleFx::Logging() const {
	std::lock_guard<std::mutex> lock(g_mx);
	return g_logging;
}

void ParticleFx::LoadSettings() {
	std::ifstream f{ std::filesystem::path(SettingsPath()) };
	if (!f.is_open())
		return;
	std::stringstream ss;
	ss << f.rdbuf();
	JsonValue root;
	if (!JsonParse(ss.str(), root) || root.type != JsonValue::Type::Object)
		return;
	std::lock_guard<std::mutex> lock(g_mx);
	if (const JsonValue* v = root.Find("enabled"))
		g_enabled = v->AsBool(true);
	if (const JsonValue* v = root.Find("moneyHeadshot"))
		g_moneyHeadshot = v->AsBool(false);
	if (const JsonValue* modes = root.Find("modes"); modes && modes->type == JsonValue::Type::Object) {
		for (int i = 0; i < kFxCategoryCount; ++i) {
			if (const JsonValue* m = modes->Find(kCategoryKeys[i])) {
				const int mi = ModeFromName(m->AsString("on").c_str());
				g_modes[i] = mi >= 0 ? (FxMode)mi : FxMode::On;
			}
		}
	}
	g_customRules.clear();
	if (const JsonValue* rules = root.Find("custom"); rules && rules->type == JsonValue::Type::Array) {
		for (const JsonValue& r : rules->arr) {
			if (r.type != JsonValue::Type::Object)
				continue;
			CustomRule cr;
			if (const JsonValue* m = r.Find("match"))
				cr.match = m->AsString();
			if (const JsonValue* t = r.Find("target"))
				cr.target = t->AsString();
			std::transform(cr.match.begin(), cr.match.end(), cr.match.begin(),
				[](unsigned char c) { return (char)std::tolower(c); });
			if (!cr.match.empty())
				g_customRules.push_back(std::move(cr));
		}
	}
	QueueActiveSwapTargetsLocked();
}

bool ParticleFx::SaveSettings() const {
	JsonBuilder b;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		b.BeginObject();
		b.BoolField("enabled", g_enabled);
		b.BoolField("moneyHeadshot", g_moneyHeadshot);
		b.Key("modes");
		b.BeginObject();
		for (int i = 0; i < kFxCategoryCount; ++i)
			b.StringField(kCategoryKeys[i], kModeNames[(int)g_modes[i]]);
		b.EndObject();
		b.Key("custom");
		b.BeginArray();
		for (const CustomRule& r : g_customRules) {
			b.BeginObject();
			b.StringField("match", r.match);
			if (!r.target.empty())
				b.StringField("target", r.target);
			b.EndObject();
		}
		b.EndArray();
		b.EndObject();
	}
	std::ofstream f{ std::filesystem::path(SettingsPath()), std::ios::trunc };
	if (!f.is_open())
		return false;
	f << b.Str();
	return true;
}

std::string ParticleFx::DebugStateJson() const {
	JsonBuilder b;
	b.BeginObject();
	b.BoolField("installed", g_installed.load());
	b.BoolField("installFailedHard", g_installFailedHard);
	b.BoolField("jitRedirect", g_jitRedirectInstalled);
	b.IntField("seen", (int64_t)g_totalSeen.load());
	b.IntField("noName", (int64_t)g_totalNoName.load());
	b.IntField("blocked", (int64_t)g_totalBlocked.load());
	b.IntField("swapped", (int64_t)g_totalSwapped.load());
	{
		std::lock_guard<std::mutex> lock(g_mx);
		b.BoolField("enabled", g_enabled);
		b.BoolField("logging", g_logging);
		b.BoolField("moneyHeadshot", g_moneyHeadshot);
		b.Key("modes");
		b.BeginObject();
		for (int i = 0; i < kFxCategoryCount; ++i)
			b.StringField(kCategoryKeys[i], kModeNames[(int)g_modes[i]]);
		b.EndObject();
		b.IntField("customRules", (int64_t)g_customRules.size());
		b.IntField("namesTracked", (int64_t)g_nameStats.size());
		b.IntField("handleCache", (int64_t)g_handleCache.size());
		b.IntField("resolveQueue", (int64_t)g_resolveQueue.size());
	}
	b.EndObject();
	return b.Str();
}

// ============================== console command ====================================

namespace {

void PrintFxHelp(const char* cmd) {
	advancedfx::Message(
		"%s fx set <category> <on|more|less|off> - control one effect category. Categories:\n"
		"    impacts    - bullet-surface impacts (particles/impact_fx)\n"
		"    tracers    - bullet tracers\n"
		"    weaponfx   - muzzle flash, muzzle/shell smoke, brass\n"
		"    blood      - blood impacts\n"
		"    explosions - HE / C4 explosions\n"
		"    molotov    - molotov / incendiary fire\n"
		"    mapfx      - map ambience particles\n"
		"  (on = converted classic mod, more = converted classic-updated mod, less =\n"
		"   converted less-impacts + less-smoke mods, off = default CS2 pass-through.\n"
		"   If the asset pack is not mounted, the original CS2 effect plays. Smoke GRENADES are never touched - CS2\n"
		"   volumetric smoke is not a particle swap.)\n"
		"%s fx on|off - master switch over all category modes + rules.\n"
		"%s fx state - status + counters.\n"
		"%s fx log on|off - capture every particle creation (view with 'fx recent').\n"
		"%s fx recent [n] - print the last n captured/acted creations (default 30).\n"
		"%s fx names [filter] - aggregated creation counts seen so far.\n"
		"%s fx block <substr> | unblock <substr> - custom block rule (name substring).\n"
		"%s fx swap <substr> <target.vpcf> | unswap <substr> - custom swap rule.\n"
		"%s fx rules - list custom rules.  %s fx test <name> - dry-run classify a name.\n"
		"%s fx apply - re-create the current moment's live effects under the current\n"
		"    settings (tiny backward seek; toggles do this automatically while paused).\n"
		"%s fx moneyshot on|off - event-gated money effect on confirmed headshot hits.\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

// Dry-run of the hook's decision logic for one name (no engine calls).
void FxTestName(const char* name) {
	char low[260];
	LowerCopy(name, low, sizeof(low));
	const int cat = ClassifyLower(low);
	char action = '=';
	std::string target;
	std::string why = "untracked category";
	{
		std::lock_guard<std::mutex> lock(g_mx);
		if (!g_enabled) {
			why = "master OFF";
		} else {
			for (const CustomRule& r : g_customRules) {
				if (std::strstr(low, r.match.c_str())) {
					action = r.target.empty() ? 'X' : '>';
					target = r.target;
					why = std::string("custom rule '") + r.match + "'";
					break;
				}
			}
			if (action == '=' && cat >= 0) {
				const FxMode m = g_modes[cat];
				why = std::string("category ") + kCategoryKeys[cat] + " = " + kModeNames[(int)m];
				if (m == FxMode::Off) {
					why += " (default CS2 pass-through)";
				} else {
					if (const char* t = VariantTargetLower(cat, m, low)) {
						action = '>';
						target = t;
						why += " (converted variant table match)";
					} else {
						why += " (no converted variant table entry, passes through)";
					}
				}
			}
		}
	}
	if (action == 'X')
		target = kEmptySystem;
	advancedfx::Message("fx test: %s\n  category: %s\n  decision: %s%s%s\n  reason: %s\n",
		name, cat >= 0 ? kCategoryKeys[cat] : "(other)",
		action == '=' ? "PASS" : (action == 'X' ? "BLOCK -> " : "SWAP -> "),
		action == '=' ? "" : target.c_str(), "", why.c_str());
}

} // namespace

void ParticleFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	ParticleFx& fx = ParticleFxRef();
	const char* action = (argc >= 3) ? args->ArgV(2) : "";

	if (0 == _stricmp(action, "state")) {
		advancedfx::Message("[fx][state] %s\n", fx.DebugStateJson().c_str());
		return;
	}
	if (0 == _stricmp(action, "install")) {
		const bool ok = fx.EnsureInstalled();
		advancedfx::Message("fx: hook %s.\n", ok ? "installed" : "NOT installed (see warnings)");
		return;
	}
	if (0 == _stricmp(action, "on") || 0 == _stricmp(action, "off")) {
		fx.SetEnabled(0 == _stricmp(action, "on"));
		if (!(argc >= 4 && 0 == _stricmp(args->ArgV(3), "quiet")))
			advancedfx::Message("fx: effects control %s.\n", fx.Enabled() ? "ON" : "off");
		return;
	}
	if (0 == _stricmp(action, "set")) {
		if (argc < 5) { advancedfx::Warning("usage: %s fx set <category> <on|more|less|off>\n", cmd); return; }
		const int cat = CategoryFromKey(args->ArgV(3));
		const int mode = ModeFromName(args->ArgV(4));
		if (cat < 0) { advancedfx::Warning("fx: unknown category '%s' (see '%s fx').\n", args->ArgV(3), cmd); return; }
		if (mode < 0) { advancedfx::Warning("fx: unknown mode '%s' (on|more|less|off).\n", args->ArgV(4)); return; }
		fx.SetMode((FxCategory)cat, (FxMode)mode);
		if (!(argc >= 6 && 0 == _stricmp(args->ArgV(5), "quiet")))
			advancedfx::Message("fx: %s = %s.%s\n", kCategoryKeys[cat], kModeNames[mode],
				fx.Installed() ? "" : " (hook not armed yet - arms once a demo/map has loaded)");
		return;
	}
	if (0 == _stricmp(action, "log")) {
		const char* v = (argc >= 4) ? args->ArgV(3) : "";
		if (0 == _stricmp(v, "on")) fx.SetLogging(true);
		else if (0 == _stricmp(v, "off")) fx.SetLogging(false);
		else { advancedfx::Message("fx: logging is %s (fx log on|off).\n", fx.Logging() ? "ON" : "off"); return; }
		advancedfx::Message("fx: logging %s.%s\n", fx.Logging() ? "ON" : "off",
			fx.Logging() ? " Play the demo, then 'fx recent' / 'fx names'." : "");
		return;
	}
	if (0 == _stricmp(action, "recent")) {
		const size_t n = (argc >= 4) ? (size_t)atoi(args->ArgV(3)) : 30;
		std::lock_guard<std::mutex> lock(g_mx);
		const size_t count = g_ring.size();
		const size_t want = n && n < count ? n : count;
		advancedfx::Message("fx: last %zu creation(s) (of %zu captured; = pass, X blocked, > swapped):\n", want, count);
		for (size_t i = count - want; i < count; ++i) {
			// ring order: g_ringNext is the OLDEST once full
			const FxEvent& e = g_ring[(g_ringNext + i) % (count < kRingCap ? count : kRingCap)];
			if (e.action == '=')
				advancedfx::Message("  = %s\n", e.name.c_str());
			else
				advancedfx::Message("  %c %s -> %s\n", e.action, e.name.c_str(), e.target.c_str());
		}
		return;
	}
	if (0 == _stricmp(action, "names")) {
		const char* filter = (argc >= 4) ? args->ArgV(3) : nullptr;
		std::vector<std::pair<std::string, NameStat>> rows;
		{
			std::lock_guard<std::mutex> lock(g_mx);
			for (const auto& [name, stat] : g_nameStats)
				if (!filter || std::strstr(name.c_str(), filter))
					rows.push_back({ name, stat });
		}
		std::sort(rows.begin(), rows.end(),
			[](const auto& a, const auto& b) { return a.second.seen > b.second.seen; });
		const size_t cap = 60;
		advancedfx::Message("fx: %zu name(s)%s (top %zu by count; seen/acted):\n",
			rows.size(), filter ? " matching filter" : "", rows.size() < cap ? rows.size() : cap);
		for (size_t i = 0; i < rows.size() && i < cap; ++i)
			advancedfx::Message("  %6llu %5llu  %s\n", rows[i].second.seen, rows[i].second.acted,
				rows[i].first.c_str());
		return;
	}
	if (0 == _stricmp(action, "block") || 0 == _stricmp(action, "swap")) {
		const bool isSwap = 0 == _stricmp(action, "swap");
		if (argc < (isSwap ? 5 : 4)) {
			advancedfx::Warning("usage: %s fx %s <substr>%s\n", cmd, action, isSwap ? " <target.vpcf>" : "");
			return;
		}
		CustomRule r;
		r.match = args->ArgV(3);
		std::transform(r.match.begin(), r.match.end(), r.match.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		if (isSwap)
			r.target = args->ArgV(4);
		{
			std::lock_guard<std::mutex> lock(g_mx);
			g_customRules.push_back(r);
			QueueActiveSwapTargetsLocked();
		}
		fx.EnsureInstalled();
		fx.SaveSettings();
		RequestApplyReseek();
		advancedfx::Message("fx: rule added: '%s' -> %s.\n", r.match.c_str(),
			r.target.empty() ? "BLOCK" : r.target.c_str());
		return;
	}
	if (0 == _stricmp(action, "unblock") || 0 == _stricmp(action, "unswap")) {
		if (argc < 4) { advancedfx::Warning("usage: %s fx %s <substr>\n", cmd, action); return; }
		std::string match = args->ArgV(3);
		std::transform(match.begin(), match.end(), match.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		size_t removed = 0;
		{
			std::lock_guard<std::mutex> lock(g_mx);
			const size_t before = g_customRules.size();
			g_customRules.erase(std::remove_if(g_customRules.begin(), g_customRules.end(),
				[&](const CustomRule& r) { return r.match == match; }), g_customRules.end());
			removed = before - g_customRules.size();
		}
		fx.SaveSettings();
		RequestApplyReseek();
		advancedfx::Message("fx: removed %zu rule(s) matching '%s'.\n", removed, match.c_str());
		return;
	}
	if (0 == _stricmp(action, "moneyshot")) {
		const char* v = (argc >= 4) ? args->ArgV(3) : "";
		if (0 == _stricmp(v, "on")) fx.SetMoneyHeadshot(true);
		else if (0 == _stricmp(v, "off")) fx.SetMoneyHeadshot(false);
		else { advancedfx::Message("fx: moneyshot is %s (fx moneyshot on|off).\n", fx.MoneyHeadshot() ? "ON" : "off"); return; }
		advancedfx::Message("fx: money-on-headshot %s.\n",
			fx.MoneyHeadshot() ? "ON (event-gated; requires converted money asset)" : "off");
		return;
	}
	if (0 == _stricmp(action, "apply")) {
		// Manual, immediate, regardless of pause state: re-create the current moment's
		// live effects under the current rules (tiny backward seek).
		ApplyReseekNow();
		advancedfx::Message("fx: re-applied settings to live effects (reseek).\n");
		return;
	}
	if (0 == _stricmp(action, "rules")) {
		std::lock_guard<std::mutex> lock(g_mx);
		if (g_customRules.empty()) { advancedfx::Message("fx: no custom rules.\n"); return; }
		for (size_t i = 0; i < g_customRules.size(); ++i)
			advancedfx::Message("  %zu | '%s' -> %s\n", i, g_customRules[i].match.c_str(),
				g_customRules[i].target.empty() ? "BLOCK" : g_customRules[i].target.c_str());
		return;
	}
	if (0 == _stricmp(action, "test")) {
		if (argc < 4) { advancedfx::Warning("usage: %s fx test <particles/....vpcf>\n", cmd); return; }
		FxTestName(args->ArgV(3));
		return;
	}
	PrintFxHelp(cmd);
	advancedfx::Message("[fx][state] %s\n", fx.DebugStateJson().c_str());
}

} // namespace Filmmaker
