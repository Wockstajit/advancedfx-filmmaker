#pragma once

#include "FollowCameraMath.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Filmmaker {

// Logical target groups. C4 and dropped/held weapons all live under Weapon and are
// distinguished by class checks + WeaponSource. Attachment is now a MODE, not a type.
enum class FollowTargetType {
	Player = 0,
	Grenade = 1,
	Weapon = 2
};

// Turret (camera placed at a fixed point, only rotates to track the target) vs rigid
// ride-along (camera position + orientation are derived from the target every frame,
// with a local offset / rotation trim / FOV applied).
enum class FollowMode {
	LockOn = 0,
	Attach = 1
};

// For Weapon targets: follow the player's active (held) weapon, the dropped weapon
// entity on the ground, or auto-pick based on the selected candidate's owner.
enum class WeaponSource {
	Auto = 0,
	Held = 1,
	Dropped = 2
};

enum class FollowTargetStatus {
	Missing = 0,
	Alive = 1,
	Dead = 2,
	Active = 3
};

struct FollowTargetCandidate {
	int entityIndex = -1;
	uint64_t handle = 0;
	std::string name;
	std::string className;
	int team = 0;
	bool alive = false;
	double distance = 0.0;
	bool isWeapon = false;
	bool isBomb = false;
	bool dropped = false;
	bool held = false;
	int ownerIndex = -1;
	std::string ownerName;
	std::string status;
	std::string grenadeType;
	int throwTick = -1;
	int tickDelta = 0;
	std::vector<std::string> attachments;
};

// A recorded loadout event from the demo pre-scan (.fmjson v5): weapon/C4 drops and
// pickups. Surfaced as the Events list so Preview Tick can jump to the moment.
struct FollowEventRecord {
	int tick = -1;
	std::string type;       // "weapon_drop" | "bomb_dropped" | "bomb_pickup" | "item_pickup"
	std::string item;       // e.g. "weapon_ak47", "c4"
	uint32_t accountId = 0;  // player involved (Steam account id; 0 = unknown)
	std::string ownerName;   // resolved from the scoreboard when available
};

struct FollowCameraState {
	bool enabled = false;
	bool hasCamera = false;
	FollowVec3 cameraPosition;
	FollowAngles cameraAngles;
	double fov = 90.0;
	FollowTargetType targetType = FollowTargetType::Player;
	FollowMode mode = FollowMode::LockOn;
	WeaponSource weaponSource = WeaponSource::Auto;
	int targetEntityIndex = -1;
	int weaponPlayerIndex = -1; // when following a HELD weapon: the owning player pawn
	uint64_t targetHandle = 0;
	FollowVec3 offset;
	FollowAngles rotationOffset; // attach: applied on top of the attachment orientation;
	                             // lock-on: aim trim added after the look-at solve.
	double lookSmoothing = 0.10;
	double positionSmoothing = 0.04;
	double prediction = 0.0;
	double deadzone = 0.0;
	double maxTurnSpeed = 720.0;
	double blendInTime = 0.10;
	double retargetBlendTime = 0.30;
	bool autoDisableOnDeath = true;
	bool switchToDroppedWeaponOnDeath = false;
	bool switchToDroppedBombOnDeath = false;
	bool holdLastKnownPosition = true;
	std::string attachmentName = "head";
};

class IFollowTargetProvider {
public:
	virtual ~IFollowTargetProvider() = default;
	virtual bool IsValid() = 0;
	virtual FollowVec3 GetWorldPosition() = 0;
	// Rigid-attach transform: fills pos (+ ang when oriented) for ride-along mode.
	// Returns true when an orientation is available (attachment quat / render angles),
	// false when only a position is known (caller then keeps the placed orientation).
	virtual bool GetWorldTransform(FollowVec3& pos, FollowAngles& ang) {
		pos = GetWorldPosition();
		(void)ang;
		return false;
	}
	virtual std::string GetDisplayName() = 0;
	virtual FollowTargetStatus GetStatus() = 0;
	virtual int EntityIndex() const = 0;
};

class FollowCamera {
public:
	void RunFrame(bool cameraEditorActive);

	void PlaceCamera();
	void BeginReposition();
	void PlaceReposition();
	void CancelReposition();
	void ClearCamera();
	void Preview();
	void StopPreview(const char* reason = nullptr);
	void SetTargetType(FollowTargetType type);
	void SetMode(FollowMode mode);
	void SetWeaponSource(WeaponSource source);
	bool SelectNearest();
	bool SelectEntity(int entityIndex);
	bool SelectHandle(uint64_t handle);
	bool TrackSelectedGrenade();
	bool SelectEvent(int eventIndex);   // pick a recorded drop/pickup event
	bool PreviewTick();                 // jump the demo to just before the selected event

	void SetOffset(double x, double y, double z);
	void SetOffsetAxis(int axis, double value); // 0=x 1=y 2=z (slider granularity)
	void SetRotationOffset(double pitch, double yaw, double roll);
	void SetRotationAxis(int axis, double value); // 0=pitch 1=yaw 2=roll
	void SetFov(double value);
	void SetPreset(const std::string& name);
	void SetLookSmoothing(double value);
	void SetPositionSmoothing(double value);
	void SetPrediction(double value);
	void SetDeadzone(double value);
	void SetMaxTurnSpeed(double value);
	void SetAttachmentName(const std::string& value);
	void SetAutoDisableOnDeath(bool value) { m_state.autoDisableOnDeath = value; }
	void SetSwitchToDroppedWeapon(bool value) { m_state.switchToDroppedWeaponOnDeath = value; }
	void SetSwitchToDroppedBomb(bool value) { m_state.switchToDroppedBombOnDeath = value; }
	void SetHoldLastKnown(bool value) { m_state.holdLastKnownPosition = value; }
	void SetDebug(bool value) { m_debug = value; }

	// Attach mode rides the target and needs no placed camera; lock-on requires Place.
	bool OwnsView() const { return m_state.enabled && (m_state.hasCamera || m_state.mode == FollowMode::Attach); }
	bool Repositioning() const { return m_repositioning; }
	bool Debug() const { return m_debug; }
	const FollowCameraState& State() const { return m_state; }
	std::vector<FollowTargetCandidate> Candidates() const;
	std::string BuildStateJson() const;
	void PrintStatus() const;
	void PrintCamPose() const;   // instrumentation: dump computed pose + target world pos

private:
	std::unique_ptr<IFollowTargetProvider> MakeProvider() const;
	bool SelectCandidate(const FollowTargetCandidate& candidate);
	bool TryDeathRetarget();
	bool ResolvePendingGrenade(int currentTick);
	void UpdateGrenadeCache() const;
	void ResetMotion();
	double WallDt();

	// Background load of the current demo's loadout events (.fmjson v5). Kicks off a
	// detached worker the first time the events are needed for a given demo path.
	void EnsureEventsLoaded() const;
	std::vector<FollowEventRecord> EventsSnapshot() const;

	struct GrenadeRecord {
		uint64_t handle = 0;
		int entityIndex = -1;
		std::string className;
		std::string grenadeType;
		std::string throwerName;
		int throwerIndex = -1;
		int team = 0;
		int throwTick = -1;
		bool active = false;
	};

	FollowCameraState m_state;
	FollowAngles m_outputAngles;
	FollowVec3 m_smoothedTarget;
	FollowVec3 m_smoothedCamPos; // attach mode: smoothed camera position
	FollowVec3 m_lastRawTarget;
	FollowVec3 m_lastKnownTarget;
	FollowVec3 m_targetVelocity;
	FollowVec3 m_retargetFrom;
	bool m_motionInitialized = false;
	bool m_haveLastRawTarget = false;
	bool m_haveLastKnownTarget = false;
	bool m_retargeting = false;
	double m_retargetElapsed = 0.0;
	double m_deathRetargetElapsed = 0.0;
	int m_lastPlayerWeaponIndex = -1;
	int m_lastPlayerBombIndex = -1;
	long long m_lastQpc = 0;
	bool m_debug = false;
	bool m_repositioning = false;
	uint64_t m_selectedGrenadeHandle = 0;
	bool m_grenadeTrackPending = false;
	bool m_grenadeSeekObserved = false;
	bool m_grenadeResumeIssued = false;
	int m_grenadeThrowTick = -1;
	int m_grenadeSeekTick = -1;
	std::string m_lastMessage;
	mutable std::unordered_map<uint64_t, GrenadeRecord> m_grenadeCache;
	unsigned m_debugFrame = 0;

	// Selected recorded event + last computed pose (instrumentation readout).
	int m_selectedEvent = -1;
	FollowVec3 m_lastCamPos;
	FollowAngles m_lastCamAng;
	FollowVec3 m_lastTargetPos;

	// Demo loadout-event index, loaded lazily on a background thread.
	enum class EventStatus { Idle = 0, Loading = 1, Ready = 2, Failed = 3 };
	mutable std::mutex m_eventMutex;
	mutable std::string m_eventDemoPath;     // demo path the loaded events belong to
	mutable std::string m_eventLoadingPath;  // path of the in-flight background load
	mutable std::atomic<int> m_eventStatus{ (int)EventStatus::Idle };
	mutable std::vector<FollowEventRecord> m_events;
};

FollowCamera& FollowCameraRef();
const char* FollowTargetTypeName(FollowTargetType type);

} // namespace Filmmaker
