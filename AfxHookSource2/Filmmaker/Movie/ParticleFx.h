#pragma once

// Runtime particle-effect modifiers (the EFFECTS section of the Config panel): per-category
// On / More / Less / Off control over a demo's visual effects -- bullet impacts, muzzle
// flash + shell smoke, tracers, blood, grenade explosions, molotov fire, map ambience. The
// hook can only swap to Source 2 particle resources (.vpcf/.vpcf_c) known to CS2. The old
// CS:GO "Better Particles" variants in panorama ref are Source 1 .pcf/.vmt/.vtf files and
// must be ported with source1import before they can be used here; stock placeholder swaps
// are intentionally not used.
//
// HOW: one Detours hook on particles.dll's CParticleSystemMgr create-collection
// implementation -- the single chokepoint every particle-system instantiation funnels
// through (name-based creates, resource-handle creates and the async client path all end
// there). The hook reads the system's resource name off the handle ([[handle+8]] -> char*,
// guarded), classifies it by path prefix, and either passes it through or SWAPS the handle
// for a different system resolved by name via the manager's own FindParticleSystem. Per-
// category "Off" is vanilla CS2 pass-through; only explicit custom block rules swap to
// the engine's stock empty system (particles/dev/empty.vpcf). Any failed swap fails OPEN
// (original effect plays).
//
// Resolution is deliberately pattern-free: CreateInterface("ParticleSystemMgr003") ->
// vtable slots (FindParticleSystem +0x78, create-by-handle stub +0x88, create-by-name
// +0x90), then the stub's tail-jmp is followed to the shared body so the detour also
// catches the internal direct-call (profiling-duplicate) path. Every resolved pointer is
// bounds-checked against the particles.dll image and the stub/body byte shapes are
// validated; any mismatch after a CS2 update degrades the feature to INACTIVE with a
// console warning -- effects then simply play unmodified (repo convention: pattern misses
// degrade, never crash).
//
// SMOKE GRENADES ARE DELIBERATELY NOT TOUCHED: CS2 smoke is a gameplay-coupled volumetric
// voxel system, not a swappable particle system.

#include <string>

namespace advancedfx { class ICommandArgs; }
namespace SOURCESDK { namespace CS2 { class CGameEvent; } }

namespace Filmmaker {

// Category order is also the persisted/UI order. kFxTracers is matched BEFORE kFxWeaponFx
// (tracers live under the same weapon-fx path prefixes and are split out by "tracer" in the
// name) so the two can be controlled independently.
enum FxCategory {
	kFxImpacts = 0,   // particles/impact_fx/ + water_impact/ + breakable_fx/ (bullet-surface impacts)
	kFxTracers,       // .../weapons paths + "tracer"    (bullet tracers)
	kFxWeaponFx,      // particles/weapons/cs_weapon_fx/ + particles/unified_weapon_fx/
	kFxBlood,         // any path containing "blood" (blood_impact/, impact_fx/, screen splatter)
	kFxExplosions,    // particles/explosions_fx/ + particles/entity/env_explosion/ (HE, C4)
	kFxMolotov,       // particles/burning_fx/ + particles/inferno_fx*
	kFxMapFx,         // particles/maps|ambient_fx|environment|rain_fx|critters/ (map ambience)
	kFxCategoryCount
};

enum class FxMode : unsigned char {
	On = 0,   // converted Better Particles classic variant
	Less = 1, // converted less-impacts + less-smoke variants
	Off = 2,  // vanilla CS2 pass-through for this category
	More = 3  // converted Better Particles classic-updated variant
};

class ParticleFx {
public:
	// Resolves the manager + installs the detour. Cheap when already installed; returns
	// false (and only warns ONCE for hard shape mismatches) when the hook cannot arm yet.
	bool EnsureInstalled();
	bool Installed() const;
	// True when current settings need the hook. Since On means the classic mod variant, the
	// enabled default needs the hook too; logging can also force it while disabled.
	bool WantsHook() const;
	// Lazy install retry, called once per main-thread frame (internally rate-limited).
	void PumpMainThread();

	bool Enabled() const;            // master switch over all category modes + custom rules
	void SetEnabled(bool on);        // auto-saves
	FxMode Mode(FxCategory cat) const;
	void SetMode(FxCategory cat, FxMode mode); // auto-saves

	void SetLogging(bool on);        // capture creations to the recent-ring (+ mvm_debug)
	bool Logging() const;

	// Optional money/headshot effect. It is event-gated: candidate headshot particles are
	// only eligible immediately after a real player_hurt/player_death headshot event.
	bool MoneyHeadshot() const;
	void SetMoneyHeadshot(bool on);

	void LoadSettings();             // tolerant: missing/malformed file = defaults
	bool SaveSettings() const;

	std::string DebugStateJson() const;
};

ParticleFx& ParticleFxRef();

// Lowercase JSON/command tokens ("on"/"less"/"off", "impacts"/"tracers"/...).
const char* FxModeName(FxMode mode);
const char* FxCategoryKey(FxCategory cat);

// Console entry: handles "mirv_filmmaker fx ..." (dispatched from FilmmakerCommand.cpp).
void ParticleFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Called from the global game-event hook so effect decisions can be tied to real demo
// events instead of generic particle names.
void ParticleFx_OnGameEvent(SOURCESDK::CS2::CGameEvent* event);

} // namespace Filmmaker
