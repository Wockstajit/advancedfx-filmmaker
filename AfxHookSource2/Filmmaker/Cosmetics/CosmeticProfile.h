#pragma once

// Offline demo-only cosmetic OVERRIDE PROFILES, keyed by SteamID64.
//
// A profile holds the cosmetic choices the user made for one player (weapon skin, knife, gloves,
// agent). Profiles are keyed by the player's SteamID64 -- NOT by pawn/entity index -- so the
// override follows the player across pawn recreation, round restarts, death/respawn, observer
// target switches, demo seeks and entity recreation. The apply loop (CosmeticOverrideSystem)
// resolves the current owner of each econ entity by XUID and looks the profile up by SteamID.
//
// Persisted to %APPDATA%\HLAE\cosmetic_profiles.json (NOT the repo tree) so user profiles never
// dirty git, consistent with Config/DemoFolderStore. This is a movie-making tool for OFFLINE demo
// playback only; it is never used for live/online play.

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Filmmaker {

// One weapon/knife/gloves slot override. `set` distinguishes "user picked something for this slot"
// from "slot left alone". paintKit == 0 is a legal finish (vanilla) when set == true.
struct CosmeticItem {
	bool set = false;     // true if the user overrode this slot
	int defIndex = 0;     // item definition index; for weapons 0 = keep the held weapon's own def,
	                      // for knives a real knife def (e.g. 515 = Butterfly) drives a def/model swap
	int paintKit = 0;     // paint kit / finish id (0 = vanilla finish)
	float wear = 0.0f;    // 0..1 float wear
	int seed = 0;         // pattern seed
	int statTrak = -1;    // -1 = none, >= 0 = StatTrak kill count
};

// Agent (player pawn model) override. Render-only model swap; see CosmeticOverrideSystem for the
// (research-gated) application path.
struct CosmeticAgent {
	bool set = false;
	int defIndex = 0;        // econ agent definition index (informational)
	std::string model;       // vmdl path, e.g. "agents/models/tm_phoenix/tm_phoenix.vmdl"
};

// All cosmetic overrides for one player. Keyed externally by SteamID64 in the store.
struct CosmeticProfile {
	std::string name;        // last-seen display name (informational / for status output)
	CosmeticItem primary;    // rifle / SMG / shotgun / MG slot
	CosmeticItem secondary;  // pistol slot
	CosmeticItem knife;      // melee slot
	CosmeticItem gloves;     // hand wraps
	CosmeticAgent agent;     // player model

	// True when nothing is overridden (used to prune empty profiles on save).
	bool Empty() const;
};

// SteamID64-keyed profile store with JSON persistence. Owns the global "enabled" flag because it is
// persisted alongside the profiles in the same JSON document.
class CosmeticProfileStore {
public:
	// Returns the existing profile for steamId, inserting an empty one if absent.
	CosmeticProfile& GetOrCreate(uint64_t steamId);
	// Returns the profile for steamId, or nullptr if none exists.
	CosmeticProfile* Find(uint64_t steamId);
	const CosmeticProfile* Find(uint64_t steamId) const;

	void ClearPlayer(uint64_t steamId);
	void ClearAll();

	bool Empty() const { return m_profiles.empty(); }
	const std::unordered_map<uint64_t, CosmeticProfile>& All() const { return m_profiles; }

	bool Enabled() const { return m_enabled; }
	void SetEnabled(bool e) { m_enabled = e; }

	// Persistence. Path is %APPDATA%\HLAE\cosmetic_profiles.json (GetHlaeRoamingAppDataFolderW()).
	// Load() is tolerant: a missing/malformed file just yields an empty (disabled) store.
	bool Load();
	bool Save() const;

	// Absolute path of the JSON file (for status output / diagnostics).
	std::wstring FilePath() const;

private:
	std::unordered_map<uint64_t, CosmeticProfile> m_profiles;
	bool m_enabled = false;
};

} // namespace Filmmaker
