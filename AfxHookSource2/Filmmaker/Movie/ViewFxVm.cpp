#include "ViewFxVm.h"

#include "FollowTargetProviders.h" // EntityAt / EntityClass / Lower (external linkage helpers)
#include "../Cosmetics/CosmeticModelSwap.h" // ReadActiveViewmodelWeaponState -- proven HudModelArms
                                             // child walk (entity-list scan for "*viewmodel*" class
                                             // names finds nothing; CS2's viewmodel is the pawn's
                                             // HudModelArms child, not a standalone *ViewModel* entity)
#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"
#include "../../../shared/AfxConsole.h"
#include "../../../shared/binutils.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../deps/release/Detours/src/detours.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace Filmmaker {

namespace {

// ---------------- configuration state (netcon/console writes, hook threads read; all plain
// scalars, torn reads are benign for an experiment toggle) -----------------
std::atomic<int> g_site{ 0 };      // 0 off | 1 helper-post | 2 main pump | 3 render pump | 4 caller-post
std::atomic<int> g_field{ 1 };     // 1 m_angRotation | 2 nodeToWorld quat | 3 both
std::atomic<int> g_mode{ 0 };      // 0 test-spin | 1 deadzone
double g_spinDegPerApply = 0.0;    // test mode: additive yaw per apply (visible = write works)
double g_freeAimPitch = 0.0, g_freeAimYaw = 0.0; // deadzone mode: pushed by ViewFx

// diagnostics
std::atomic<unsigned long long> g_applies{ 0 };
std::atomic<int> g_lastVmIndex{ -1 };
int g_quatOffset = -1;             // detected offset of the orientation quat inside m_nodeToWorld

size_t g_helperAddr = 0;           // ViewModel.cpp's hooked helper (site-4 caller discovery)

// ---------------- small math ----------------
struct Quat { double x = 0, y = 0, z = 0, w = 1; };

Quat QuatFromYawPitchDeg(double yawDeg, double pitchDeg) {
	// Source convention: yaw about +Z (world up), pitch about +Y (right, after yaw). For the
	// small free-aim angles involved, composing yaw*pitch about world axes is close enough.
	const double yr = yawDeg * 3.14159265358979323846 / 360.0;   // half angles
	const double pr = pitchDeg * 3.14159265358979323846 / 360.0;
	const Quat qy{ 0.0, 0.0, std::sin(yr), std::cos(yr) };
	const Quat qp{ 0.0, std::sin(pr), 0.0, std::cos(pr) };
	// qy * qp
	Quat r;
	r.w = qy.w * qp.w - qy.x * qp.x - qy.y * qp.y - qy.z * qp.z;
	r.x = qy.w * qp.x + qy.x * qp.w + qy.y * qp.z - qy.z * qp.y;
	r.y = qy.w * qp.y - qy.x * qp.z + qy.y * qp.w + qy.z * qp.x;
	r.z = qy.w * qp.z + qy.x * qp.y - qy.y * qp.x + qy.z * qp.w;
	return r;
}

void QuatMulInto(const Quat& a, const float* b4, float* out4) { // out = a * b (b, out: x y z w)
	const double bx = b4[0], by = b4[1], bz = b4[2], bw = b4[3];
	out4[0] = (float)(a.w * bx + a.x * bw + a.y * bz - a.z * by);
	out4[1] = (float)(a.w * by - a.x * bz + a.y * bw + a.z * bx);
	out4[2] = (float)(a.w * bz + a.x * by - a.y * bx + a.z * bw);
	out4[3] = (float)(a.w * bw - a.x * bx - a.y * by - a.z * bz);
}

// ---------------- viewmodel entity + scene node ----------------
// CS2 has no standalone "*ViewModel*"-classed entity to scan for (confirmed empirically: an
// entity-list class-name scan finds nothing). The rendered first-person weapon is a CHILD scene
// node of the local viewer's HudModelArms entity -- CosmeticModelSwap.cpp's ReadActiveViewmodelWeaponState
// already implements that exact walk (used by the skin-changer to mirror cosmetics onto it), so
// reuse it instead of re-deriving the same offsets/SEH-guarded pointer chase here.
unsigned char* FindViewmodelSceneNode(int* outEntityIndex) {
	if (outEntityIndex) *outEntityIndex = -1;
	const ptrdiff_t offNode = g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode;
	if (offNode == 0) return nullptr;

	int pawnIndex = AfxGetSpectatedPawnIndex();
	CEntityInstance* pawn = pawnIndex >= 0 ? EntityAt(pawnIndex) : nullptr;
	if (!pawn) pawn = AfxGetLocalViewerPawn(); // demo/spectate fallback (see ClientEntitySystem.h)
	if (!pawn) return nullptr;

	const auto weaponHandle = pawn->GetActiveWeaponHandle();
	if (!weaponHandle.IsValid()) return nullptr;
	CEntityInstance* worldWeapon = EntityAt(weaponHandle.GetEntryIndex());
	if (!worldWeapon) return nullptr;
	const char* weaponClass = worldWeapon->GetClassName();
	if (!weaponClass || !*weaponClass) return nullptr;

	int vmIndex = -1;
	if (!ReadActiveViewmodelWeaponState((unsigned char*)pawn, weaponClass, &vmIndex, nullptr, nullptr))
		return nullptr;
	if (vmIndex < 0) return nullptr;

	CEntityInstance* vmEnt = EntityAt(vmIndex);
	if (!vmEnt) return nullptr;
	unsigned char* node = *(unsigned char**)((unsigned char*)vmEnt + offNode);
	if (!node) return nullptr;
	if (outEntityIndex) *outEntityIndex = vmIndex;
	return node;
}

// Auto-detect the orientation quaternion inside m_nodeToWorld: the only 4 consecutive floats
// in the transform window whose norm is ~1. (CTransformWS layout differs across dumps; the
// position triple can be validated against m_vecAbsOrigin.)
int DetectQuatOffset(unsigned char* node) {
	const ptrdiff_t offXform = g_clientDllOffsets.CGameSceneNode.m_nodeToWorld;
	if (offXform == 0) return -1;
	const float* f = (const float*)(node + offXform);
	for (int i = 0; i <= 4; ++i) { // window: 8 floats (32 bytes)
		const double n = std::sqrt((double)f[i] * f[i] + (double)f[i + 1] * f[i + 1]
			+ (double)f[i + 2] * f[i + 2] + (double)f[i + 3] * f[i + 3]);
		if (std::fabs(n - 1.0) < 0.02)
			return (int)(offXform + i * 4);
	}
	return -1;
}

// ---------------- the actual write ----------------
void ApplyRotation(const char* siteTag) {
	double pitch = 0.0, yaw = 0.0;
	if (g_mode.load(std::memory_order_relaxed) == 1) {
		pitch = g_freeAimPitch; yaw = g_freeAimYaw;
		if (pitch == 0.0 && yaw == 0.0) return;
	} else {
		yaw = g_spinDegPerApply;
		if (yaw == 0.0) return;
	}

	int vmIndex = -1;
	unsigned char* node = FindViewmodelSceneNode(&vmIndex);
	if (!node) return;
	g_lastVmIndex.store(vmIndex, std::memory_order_relaxed);

	const int field = g_field.load(std::memory_order_relaxed);

	if ((field & 1) && g_clientDllOffsets.CGameSceneNode.m_angRotation != 0) {
		float* ang = (float*)(node + g_clientDllOffsets.CGameSceneNode.m_angRotation);
		ang[0] = (float)(ang[0] + pitch);
		ang[1] = (float)(ang[1] + yaw);
	}
	if (field & 2) {
		if (g_quatOffset < 0) g_quatOffset = DetectQuatOffset(node);
		if (g_quatOffset >= 0) {
			float* q = (float*)(node + g_quatOffset);
			float out[4];
			QuatMulInto(QuatFromYawPitchDeg(yaw, pitch), q, out);
			q[0] = out[0]; q[1] = out[1]; q[2] = out[2]; q[3] = out[3];
		}
	}

	const unsigned long long n = g_applies.fetch_add(1, std::memory_order_relaxed) + 1;
	if (n <= 3 || (n % 512) == 0)
		advancedfx::Message("[viewfxvm] apply #%llu site=%s field=%d vmEnt=%d pitch=%.2f yaw=%.2f.\n",
			n, siteTag, field, vmIndex, pitch, yaw);
}

// ---------------- site 4: detour the helper's CALLER ----------------
typedef void* (__fastcall* VmCaller_t)(void* a, void* b, void* c, void* d);
VmCaller_t g_origVmCaller = nullptr;
bool g_callerInstalled = false;

void* __fastcall Hook_VmCaller(void* a, void* b, void* c, void* d) {
	void* r = g_origVmCaller(a, b, c, d);
	if (g_site.load(std::memory_order_relaxed) == 4)
		ApplyRotation("4/caller-post");
	return r;
}

// Scan client.dll code for the E8 call targeting the helper, then walk back to the enclosing
// function's start (preceded by 0xCC padding, MSVC convention) and detour it.
size_t FindHelperCallerStart() {
	if (!g_helperAddr) return 0;
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client) return 0;
	Afx::BinUtils::ImageSectionsReader sections(client);
	for (; !sections.Eof(); sections.Next()) {
		Afx::BinUtils::MemRange range = sections.GetMemRange();
		unsigned char* p = (unsigned char*)range.Start;
		unsigned char* end = (unsigned char*)range.End - 5;
		for (; p < end; ++p) {
			if (*p != 0xE8) continue;
			const long long rel = *(int*)(p + 1);
			if ((size_t)(p + 5 + rel) != g_helperAddr) continue;
			// Found a call site; walk back for a >=2-byte 0xCC padding run.
			unsigned char* q = p;
			unsigned char* lo = p - 0x800 > (unsigned char*)range.Start ? p - 0x800 : (unsigned char*)range.Start;
			for (; q > lo; --q) {
				if (q[-1] == 0xCC && q[-2] == 0xCC) {
					advancedfx::Message("[viewfxvm] helper call site %p, caller start %p (first bytes %02X %02X %02X %02X).\n",
						p, q, q[0], q[1], q[2], q[3]);
					return (size_t)q;
				}
			}
			advancedfx::Warning("[viewfxvm] found the call site (%p) but no padding boundary above it.\n", p);
			return 0;
		}
	}
	advancedfx::Warning("[viewfxvm] no call site targeting the viewmodel helper was found.\n");
	return 0;
}

bool EnsureCallerDetourInstalled() {
	if (g_callerInstalled) return true;
	const size_t start = FindHelperCallerStart();
	if (!start) return false;
	g_origVmCaller = (VmCaller_t)start;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_origVmCaller, Hook_VmCaller);
	g_callerInstalled = (NO_ERROR == DetourTransactionCommit());
	advancedfx::Message("[viewfxvm] caller detour %s.\n", g_callerInstalled ? "installed" : "FAILED");
	return g_callerInstalled;
}

} // namespace

void ViewFxVm_NoteViewmodelHelper(size_t helperAddr) { g_helperAddr = helperAddr; }

void ViewFxVm_OnViewmodelCalc() {
	if (g_site.load(std::memory_order_relaxed) == 1)
		ApplyRotation("1/helper-post");
}
void ViewFxVm_MainPump() {
	if (g_site.load(std::memory_order_relaxed) == 2)
		ApplyRotation("2/main-pump");
}
void ViewFxVm_RenderPump() {
	if (g_site.load(std::memory_order_relaxed) == 3)
		ApplyRotation("3/render-pump");
}

void ViewFxVm_SetFreeAim(double pitchDeg, double yawDeg) {
	g_freeAimPitch = pitchDeg;
	g_freeAimYaw = yawDeg;
}

bool ViewFxVm_RotationActive() {
	return g_mode.load(std::memory_order_relaxed) == 1 && g_site.load(std::memory_order_relaxed) != 0;
}

void ViewFxVm_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = (argc >= 4) ? args->ArgV(3) : "";
	const char* val = (argc >= 5) ? args->ArgV(4) : "";

	if (0 == _stricmp(sub, "info")) {
		int vmIndex = -1;
		unsigned char* node = FindViewmodelSceneNode(&vmIndex);
		int found = (vmIndex >= 0) ? 1 : 0;
		if (found) {
			CEntityInstance* ent = EntityAt(vmIndex);
			const std::string cls = ent ? EntityClass(ent) : "?";
			advancedfx::Message("[viewfxvm][info] ent=%d class=%s node=%p (via HudModelArms child walk)\n",
				vmIndex, cls.c_str(), node);
			if (g_clientDllOffsets.CGameSceneNode.m_angRotation != 0) {
				const float* a = (const float*)(node + g_clientDllOffsets.CGameSceneNode.m_angRotation);
				advancedfx::Message("[viewfxvm][info]   m_angRotation=(%.2f %.2f %.2f)\n", a[0], a[1], a[2]);
			}
			if (g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin != 0) {
				const float* o = (const float*)(node + g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin);
				advancedfx::Message("[viewfxvm][info]   m_vecAbsOrigin=(%.2f %.2f %.2f)\n", o[0], o[1], o[2]);
			}
			if (g_clientDllOffsets.CGameSceneNode.m_nodeToWorld != 0) {
				const float* f = (const float*)(node + g_clientDllOffsets.CGameSceneNode.m_nodeToWorld);
				advancedfx::Message("[viewfxvm][info]   nodeToWorld[0..7]=(%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f) quatOff=%d\n",
					f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], DetectQuatOffset(node));
			}
		}
		if (!found) advancedfx::Message("[viewfxvm][info] no *viewmodel* entities in the list right now.\n");
		return;
	}
	if (0 == _stricmp(sub, "site")) {
		const int s = atoi(val);
		if (s == 4 && !EnsureCallerDetourInstalled()) {
			advancedfx::Warning("[viewfxvm] site 4 unavailable (caller not resolved); staying at %d.\n",
				g_site.load());
			return;
		}
		g_site.store(s);
		advancedfx::Message("[viewfxvm] site=%d.\n", s);
		return;
	}
	if (0 == _stricmp(sub, "field")) {
		int f = 1;
		if (0 == _stricmp(val, "quat")) f = 2; else if (0 == _stricmp(val, "both")) f = 3;
		g_field.store(f);
		advancedfx::Message("[viewfxvm] field=%d (1=local ang, 2=nodeToWorld quat, 3=both).\n", f);
		return;
	}
	if (0 == _stricmp(sub, "spin")) {
		g_mode.store(0);
		g_spinDegPerApply = atof(val);
		advancedfx::Message("[viewfxvm] test spin=%.2f deg/apply (mode=test).\n", g_spinDegPerApply);
		return;
	}
	if (0 == _stricmp(sub, "mode")) {
		g_mode.store(0 == _stricmp(val, "deadzone") ? 1 : 0);
		advancedfx::Message("[viewfxvm] mode=%s.\n", g_mode.load() == 1 ? "deadzone" : "test");
		return;
	}
	if (0 == _stricmp(sub, "state")) {
		advancedfx::Message("[viewfxvm][state] site=%d field=%d mode=%d spin=%.2f applies=%llu lastVmEnt=%d "
			"quatOff=%d callerHook=%d freeAim=(%.2f %.2f)\n",
			g_site.load(), g_field.load(), g_mode.load(), g_spinDegPerApply,
			g_applies.load(), g_lastVmIndex.load(), g_quatOffset, g_callerInstalled ? 1 : 0,
			g_freeAimPitch, g_freeAimYaw);
		return;
	}
	advancedfx::Message(
		"%s viewfx vmtest info - list viewmodel entities + scene-node dump.\n"
		"%s viewfx vmtest site <0-4> - write site (0 off, 1 helper-post, 2 main pump, 3 render pump, 4 caller-post).\n"
		"%s viewfx vmtest field <local|quat|both> - which scene-node field to rotate.\n"
		"%s viewfx vmtest spin <deg> - constant additive yaw per apply (visible spin = write works).\n"
		"%s viewfx vmtest mode <test|deadzone> - test spin, or drive from the deadzone free-aim.\n"
		"%s viewfx vmtest state - current config + apply counter.\n",
		cmd, cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker
