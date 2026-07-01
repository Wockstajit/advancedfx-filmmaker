#include "FollowCamera.h"
#include "FollowTargetProviders.h"

#include "CameraBridge.h"
#include "CameraPath.h"
#include "../Filmmaker.h"
#include "../Panorama/CameraTimelineHud.h"
#include "../Panorama/GraphEditorExperimentHud.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../Demo/DemoInfoHelper.h"
#include "../../../shared/AfxConsole.h"

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_map>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

constexpr double kMediumDistance = 1800.0;
constexpr double kFarDistance = 3500.0;
constexpr double kVeryFarDistance = 7000.0;
constexpr double kTeleportDistance = 1800.0;
constexpr double kDefaultFov = 90.0;
constexpr double kDefaultLockOnLookSmoothing = 0.10;
constexpr double kDefaultLockOnPositionSmoothing = 0.04;
constexpr double kDefaultLockOnPrediction = 0.0;
constexpr double kDefaultLockOnDeadzone = 0.0;
constexpr double kDefaultLockOnMaxTurnSpeed = 720.0;

FollowVec3 DefaultLockOnOffset() {
	return FollowVec3{ 0.0, 0.0, 0.0 };
}

FollowVec3 DefaultAttachOffset() {
	return FollowVec3{ 72.0, 0.0, 8.0 };
}

FollowAngles DefaultRotationOffset() {
	return FollowAngles{ 0.0, 0.0, 0.0 };
}

} // namespace

FollowCamera& FollowCameraRef() {
	static FollowCamera instance;
	return instance;
}

double FollowCamera::WallDt() {
	LARGE_INTEGER frequency, now;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&now);
	if (!m_lastQpc) m_lastQpc = now.QuadPart;
	double dt = (double)(now.QuadPart - m_lastQpc) / (double)frequency.QuadPart;
	m_lastQpc = now.QuadPart;
	if (dt < 0.0) dt = 0.0;
	if (dt > 0.1) dt = 0.1;
	return dt;
}

void FollowCamera::ResetMotion() {
	m_motionInitialized = false;
	m_haveLastRawTarget = false;
	m_retargeting = false;
	m_retargetElapsed = 0.0;
	m_deathRetargetElapsed = 0.0;
	m_lastQpc = 0;
	m_attachDebug = AttachDebugSnapshot();
	m_attachRebaseValid = false;
	m_attachBaseOriented = false;
	m_attachOffsetIsLocal = false;
	m_havePrevAttachFresh = false;
	m_havePrevDebug = false;
}

void FollowCamera::ResetFramingState() {
	m_state.offset = (m_state.mode == FollowMode::Attach) ? DefaultAttachOffset() : DefaultLockOnOffset();
	m_state.rotationOffset = DefaultRotationOffset();
	m_state.fov = kDefaultFov;
}

void FollowCamera::ApplyAttachTrackingDefaults() {
	m_state.lookSmoothing = 0.0;
	m_state.positionSmoothing = 0.0;
	m_state.prediction = 0.0;
	m_state.deadzone = 0.0;
	m_state.maxTurnSpeed = 0.0;
	m_renderTimeSample = true;
}

void FollowCamera::ApplyLockOnTrackingState() {
	m_state.lookSmoothing = m_lockOnLookSmoothing;
	m_state.positionSmoothing = m_lockOnPositionSmoothing;
	m_state.prediction = m_lockOnPrediction;
	m_state.deadzone = m_lockOnDeadzone;
	m_state.maxTurnSpeed = m_lockOnMaxTurnSpeed;
}

void FollowCamera::ResetTrackingState() {
	m_lockOnLookSmoothing = kDefaultLockOnLookSmoothing;
	m_lockOnPositionSmoothing = kDefaultLockOnPositionSmoothing;
	m_lockOnPrediction = kDefaultLockOnPrediction;
	m_lockOnDeadzone = kDefaultLockOnDeadzone;
	m_lockOnMaxTurnSpeed = kDefaultLockOnMaxTurnSpeed;
	m_state.autoDisableOnDeath = true;
	m_state.switchToDroppedWeaponOnDeath = false;
	m_state.switchToDroppedBombOnDeath = false;
	m_state.holdLastKnownPosition = true;
	if (m_state.mode == FollowMode::Attach)
		ApplyAttachTrackingDefaults();
	else
		ApplyLockOnTrackingState();
}

void FollowCamera::PlaceCamera() {
	const bool replaced = m_state.hasCamera;
	double pos[3] = {}, ang[3] = {}, fov = 90.0;
	CameraBridge_GetCurrentCamera(pos, ang, fov);
	m_state.cameraPosition = FollowVec3{ pos[0], pos[1], pos[2] };
	m_state.cameraAngles = FollowAngles{ ang[0], ang[1], ang[2] };
	m_state.fov = fov;
	m_state.hasCamera = true;
	m_repositioning = false;
	ResetMotion();
	// Include the tick so each Place produces a DISTINCT status line -- the on-screen text
	// changes every time, making a registered click visibly distinguishable from a no-op.
	int placeTick = 0; g_MirvTime.GetCurrentDemoTick(placeTick);
	m_lastMessage = (replaced ? "Follow Camera replaced @ tick " : "Follow Camera placed @ tick ")
		+ std::to_string(placeTick) + ".";
	advancedfx::Message("[followcam] single camera %s pos=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f) fov=%.1f.\n",
		replaced ? "replaced" : "placed",
		pos[0], pos[1], pos[2], ang[0], ang[1], ang[2], fov);
}

void FollowCamera::BeginReposition() {
	if (m_state.mode == FollowMode::Attach) {
		const bool haveTarget = m_state.targetEntityIndex >= 0
			|| (m_state.targetType == FollowTargetType::Weapon && m_state.weaponPlayerIndex >= 0)
			|| (m_state.targetType == FollowTargetType::Grenade && m_state.targetHandle);
		if (!haveTarget) {
			advancedfx::Warning("[followcam] attach reposition refused: select a target first.\n");
			m_lastMessage = "Select a target before repositioning attach.";
			return;
		}
	}
	m_resumePreviewAfterReposition = m_state.enabled && m_state.mode == FollowMode::Attach;
	StopPreview("reposition started");
	m_repositioning = true;
	CameraBridge_SetFreeCamEnabled(true);
	if (CameraEditor_Active())
		CameraEditor_SetCursorMode(false);
	if (m_state.mode == FollowMode::Attach) {
		m_lastMessage = "Move freecam, then left-click to save attach offset/rotation/FOV.";
		advancedfx::Message("[followcam] repositioning attach mount: move + left-click to save relative pose (X/Esc cancel).\n");
	} else {
		advancedfx::Message("[followcam] repositioning single camera: move + left-click to place (X/Esc cancel).\n");
	}
}

void FollowCamera::PlaceReposition() {
	if (!m_repositioning) return;
	if (m_state.mode == FollowMode::Attach) {
		const bool resumePreview = m_resumePreviewAfterReposition;
		if (!CaptureAttachPoseFromCurrentView())
			return;
		m_repositioning = false;
		m_resumePreviewAfterReposition = false;
		if (CameraEditor_Active())
			CameraEditor_SetCursorMode(true);
		if (resumePreview)
			Preview();
		advancedfx::Message("[followcam] attach reposition complete; editor mouse restored.\n");
		return;
	}
	PlaceCamera();
	m_resumePreviewAfterReposition = false;
	if (CameraEditor_Active())
		CameraEditor_SetCursorMode(true);
	advancedfx::Message("[followcam] reposition complete; editor mouse restored.\n");
}

void FollowCamera::CancelReposition() {
	if (!m_repositioning) return;
	m_repositioning = false;
	m_resumePreviewAfterReposition = false;
	if (CameraEditor_Active())
		CameraEditor_SetCursorMode(true);
	advancedfx::Message("[followcam] reposition cancelled; editor mouse restored.\n");
}

bool FollowCamera::CaptureAttachPoseFromCurrentView() {
	if (m_state.mode != FollowMode::Attach) {
		advancedfx::Warning("[followcam] attach capture refused: switch to Attach mode first.\n");
		m_lastMessage = "Switch to Attach mode before saving a relative mount.";
		return false;
	}

	auto provider = MakeProvider();
	if (!provider || !provider->IsValid()) {
		advancedfx::Warning("[followcam] attach capture refused: selected target is not valid.\n");
		m_lastMessage = "Selected attach target is not valid.";
		return false;
	}

	FollowVec3 target;
	FollowAngles baseAngles;
	const bool oriented = provider->GetWorldTransform(target, baseAngles);
	if (!IsFiniteOrigin(target)) {
		advancedfx::Warning("[followcam] attach capture refused: target transform is invalid.\n");
		m_lastMessage = "Selected attach target transform is invalid.";
		return false;
	}

	double pos[3] = {}, ang[3] = {}, fov = 90.0;
	CameraBridge_GetCurrentCamera(pos, ang, fov);
	const FollowVec3 camPos{ pos[0], pos[1], pos[2] };
	const FollowAngles camAngles{ ang[0], ang[1], ang[2] };
	if (!oriented)
		baseAngles = camAngles;

	const FollowVec3 worldOffset{
		camPos.x - target.x,
		camPos.y - target.y,
		camPos.z - target.z
	};
	m_state.offset = FollowInverseRotateVector(worldOffset, baseAngles);
	m_state.rotationOffset = FollowAngles{
		FollowWrapDegrees(camAngles.pitch - baseAngles.pitch),
		FollowWrapDegrees(camAngles.yaw - baseAngles.yaw),
		FollowWrapDegrees(camAngles.roll - baseAngles.roll)
	};
	m_state.fov = std::clamp(fov, 1.0, 170.0);
	m_state.cameraPosition = camPos;
	m_state.cameraAngles = baseAngles;
	m_baseAngles = baseAngles;
	ResetMotion();
	m_lastMessage = "Attach mount saved relative to '" + m_state.attachmentName + "'.";
	advancedfx::Message(
		"[followcam] attach mount saved: attach=%s offset=(%.2f %.2f %.2f) rotation=(%.2f %.2f %.2f) fov=%.1f.\n",
		m_state.attachmentName.c_str(),
		m_state.offset.x, m_state.offset.y, m_state.offset.z,
		m_state.rotationOffset.pitch, m_state.rotationOffset.yaw, m_state.rotationOffset.roll,
		m_state.fov);
	return true;
}

void FollowCamera::ClearCamera() {
	CancelReposition();
	StopPreview();
	ResetTrackingState();
	ResetFramingState();
	ResetMotion();
	m_state.hasCamera = false;
	m_state.targetEntityIndex = -1;
	m_state.targetHandle = 0;
	m_selectedGrenadeHandle = 0;
	m_grenadeTrackPending = false;
	m_haveLastKnownTarget = false;
	m_lastMessage = "Follow Camera cleared.";
	CameraBridge_SetFollowCameraMarker(false, 0, 0, 0, 0, 0, 0, 90);
	advancedfx::Message("[followcam] camera cleared.\n");
}

void FollowCamera::Preview() {
	if (m_state.mode == FollowMode::LockOn && !m_state.hasCamera) {
		advancedfx::Warning("[followcam] preview refused: place the follow camera first (lock-on mode).\n");
		m_lastMessage = "Place the camera first, or switch to Attach mode.";
		return;
	}
	const bool haveTarget = m_state.targetEntityIndex >= 0
		|| (m_state.targetType == FollowTargetType::Weapon && m_state.weaponPlayerIndex >= 0);
	if (!haveTarget) {
		advancedfx::Warning("[followcam] preview refused: select a target first.\n");
		m_lastMessage = "Select a target first.";
		return;
	}
	CameraPathRef().StopPreview();
	CameraPathRef().StopScrub();
	GraphEditorExperimentHudRef().SetDrive(false);
	CameraBridge_SetFreeCamEnabled(true);
	CameraBridge_SetPathDrawEnabled(false);
	m_state.enabled = true;
	ResetMotion();
	advancedfx::Message("[followcam] preview started: type=%s entity=%d.\n",
		FollowTargetTypeName(m_state.targetType), m_state.targetEntityIndex);
}

void FollowCamera::StopPreview(const char* reason) {
	if (m_state.enabled && reason && *reason)
		advancedfx::Message("[followcam] preview stopped: %s.\n", reason);
	m_state.enabled = false;
	m_attachRebaseValid = false;     // don't let a stale attach pose drive the view after stop
	m_havePrevAttachFresh = false;
	m_grenadeTrackPending = false;
	m_grenadeSeekObserved = false;
	m_grenadeResumeIssued = false;
	if (reason && *reason)
		m_lastMessage = std::string("Tracking stopped: ") + reason + ".";
	if (CameraEditor_Active() && CameraTimelineHudRef().Cursor())
		GraphEditorExperimentHudRef().SetDrive(true);
	ResetMotion();
}

bool FollowCamera::ViewSetupAttachOverride(float curTime,
	double& x, double& y, double& z, double& pitch, double& yaw, double& roll, double& fov) {
	(void)curTime;
	if (!m_attachRebaseValid || !m_state.enabled || m_state.mode != FollowMode::Attach)
		return false;
	// Sample the target HERE (view setup = mid-render), where entities are interpolated, instead of
	// the tick-stepped value RunFrame read at FrameStageNotify.
	auto provider = MakeProvider();
	if (!provider || !provider->IsValid()) return false;
	FollowVec3 freshTarget; FollowAngles freshAng;
	const bool freshOriented = provider->GetWorldTransform(freshTarget, freshAng);
	if (!IsFiniteOrigin(freshTarget))
		return false;
	// Diagnostic: freshDelta is the frame-to-frame movement of the render-time sample. If the
	// render-time sample is interpolated this is a small, smoothly-varying value EVERY frame; if it
	// were still stepped it would read 0.000 then jump (the FrameStageNotify signature).
	const double freshDelta = m_havePrevAttachFresh ? FollowDistance(m_prevAttachFresh, freshTarget) : 0.0;
	const double steppedVsFresh = FollowDistance(m_attachSteppedTarget, freshTarget);
	m_prevAttachFresh = freshTarget; m_havePrevAttachFresh = true;
	if (m_debug && ((m_debugViewFrame++ % 15) == 0)) {
		advancedfx::Message("[followcam][rtsample] fresh=(%.2f %.2f %.2f) freshDelta=%.3f steppedVsFresh=%.3f apply=%d.\n",
			freshTarget.x, freshTarget.y, freshTarget.z, freshDelta, steppedVsFresh,
			m_renderTimeSample ? 1 : 0);
	}
	if (!m_renderTimeSample) return false;
	// Rebase the smoothed camera onto the interpolated target. For oriented attach points the
	// offset is stored in target-local axes, so it rotates with the same render-time bone pose the
	// visible model uses. A world-space offset from the stepped pose makes hand/weapon mounts look
	// stable in screenshots but swim/jitter in motion as the bone orientation interpolates.
	const FollowVec3 freshOffset = freshOriented && m_attachOffsetIsLocal
		? FollowRotateVector(m_attachOffsetFromTarget, freshAng)
		: m_attachOffsetFromTarget;
	x = freshTarget.x + freshOffset.x;
	y = freshTarget.y + freshOffset.y;
	z = freshTarget.z + freshOffset.z;
	const FollowAngles freshOutput = freshOriented && m_attachBaseOriented
		? FollowAddAngles(freshAng, m_attachAngleOffsetFromBase)
		: m_outputAngles;
	pitch = freshOutput.pitch;
	yaw = freshOutput.yaw;
	roll = freshOutput.roll;
	fov = m_state.fov;
	return true;
}

void FollowCamera::SetTargetType(FollowTargetType type) {
	if (m_state.targetType == type) return;
	m_state.targetType = type;
	m_state.targetEntityIndex = -1;
	m_state.weaponPlayerIndex = -1;
	m_state.targetHandle = 0;
	m_grenadeTrackPending = false;
	m_selectedEvent = -1;
	// Start from safe defaults. Candidate selection upgrades these to validated
	// model/player points (for example muzzle) only when the selected target exposes them.
	if (type == FollowTargetType::Weapon) m_state.attachmentName = "entity";
	else if (type == FollowTargetType::Grenade) m_state.attachmentName = "entity";
	else if (type == FollowTargetType::Player) m_state.attachmentName = "head";
	ResetMotion();
}

void FollowCamera::SetMode(FollowMode mode) {
	if (m_state.mode == mode) {
		if (mode == FollowMode::Attach) {
			ApplyAttachTrackingDefaults();
			ResetMotion();
		}
		return;
	}
	if (m_state.mode != FollowMode::Attach) {
		m_lockOnLookSmoothing = m_state.lookSmoothing;
		m_lockOnPositionSmoothing = m_state.positionSmoothing;
		m_lockOnPrediction = m_state.prediction;
		m_lockOnDeadzone = m_state.deadzone;
		m_lockOnMaxTurnSpeed = m_state.maxTurnSpeed;
	}
	m_state.mode = mode;
	if (mode == FollowMode::Attach) {
		ApplyAttachTrackingDefaults();
	} else {
		ApplyLockOnTrackingState();
	}
	if (mode == FollowMode::Attach
		&& std::fabs(m_state.offset.x) < 0.001
		&& std::fabs(m_state.offset.y) < 0.001
		&& std::fabs(m_state.offset.z) < 0.001) {
		m_state.offset = DefaultAttachOffset();
	}
	ResetMotion();
}

void FollowCamera::SetWeaponSource(WeaponSource source) {
	if (m_state.weaponSource == source) return;
	m_state.weaponSource = source;
	ResetMotion();
}

std::unique_ptr<IFollowTargetProvider> FollowCamera::MakeProvider() const {
	const bool attach = m_state.mode == FollowMode::Attach;
	switch (m_state.targetType) {
	case FollowTargetType::Player:
		if (attach)
			return std::make_unique<AttachmentTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Player, m_state.attachmentName);
		return std::make_unique<PlayerTargetProvider>(m_state.targetEntityIndex);
	case FollowTargetType::Grenade:
		if (attach)
			return std::make_unique<AttachmentTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Grenade, m_state.attachmentName);
		return std::make_unique<EntityTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Grenade);
	case FollowTargetType::Weapon: {
		const bool held = (m_state.weaponSource == WeaponSource::Held)
			|| (m_state.weaponSource == WeaponSource::Auto && m_state.weaponPlayerIndex >= 0);
		if (held && m_state.weaponPlayerIndex >= 0)
			return std::make_unique<HeldWeaponProvider>(m_state.weaponPlayerIndex,
				attach ? m_state.attachmentName : std::string());
		if (attach && !m_state.attachmentName.empty())
			return std::make_unique<AttachmentTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Weapon, m_state.attachmentName);
		return std::make_unique<EntityTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Weapon);
	}
	default:
		return std::make_unique<PlayerTargetProvider>(m_state.targetEntityIndex);
	}
}

void FollowCamera::RunFrame(bool cameraEditorActive) {
	if (!cameraEditorActive) {
		CameraBridge_SetFollowCameraMarker(false, 0, 0, 0, 0, 0, 0, 90);
		if (m_state.enabled) StopPreview("camera editor closed");
		return;
	}
	if (m_state.hasCamera && m_state.mode == FollowMode::LockOn && !m_state.enabled) {
		CameraBridge_SetFollowCameraMarker(true,
			m_state.cameraPosition.x, m_state.cameraPosition.y, m_state.cameraPosition.z,
			m_state.cameraAngles.pitch, m_state.cameraAngles.yaw, m_state.cameraAngles.roll, m_state.fov);
	} else {
		CameraBridge_SetFollowCameraMarker(false, 0, 0, 0, 0, 0, 0, 90);
	}
	// Attach mode rides the target and needs no placed camera; lock-on requires one.
	if (!m_state.enabled)
		return;
	if (m_state.mode == FollowMode::LockOn && !m_state.hasCamera)
		return;

	double dt = WallDt();
	// Is the demo actually advancing this frame? When it's paused/frozen the entity still
	// micro-jitters from interpolation; dividing that by the (tiny) wall-clock dt yields a
	// huge bogus velocity that drives the "face travel" path and makes an attached camera
	// spin/jitter in place. Detect a frozen tick so we can zero velocity below.
	int frameTick = 0; g_MirvTime.GetCurrentDemoTick(frameTick);
	const int previousFrameTick = m_lastFrameTick;
	const bool demoAdvancing = (previousFrameTick >= 0 && frameTick != previousFrameTick);
	if (demoAdvancing) {
		float interval = g_MirvTime.interval_per_tick_get();
		if (!(interval > 0.00001f)) interval = 1.0f / 64.0f;
		const int tickDelta = std::abs(frameTick - previousFrameTick);
		if (tickDelta > 0 && tickDelta < 128)
			dt = std::clamp((double)tickDelta * (double)interval, 0.0001, 0.1);
	}
	m_lastFrameTick = frameTick;
	if (m_state.targetType == FollowTargetType::Grenade && m_grenadeTrackPending) {
		UpdateGrenadeCache();
		int currentTick = 0;
		g_MirvTime.GetCurrentDemoTick(currentTick);

		if (!m_grenadeSeekObserved) {
			if (currentTick >= m_grenadeSeekTick - 3 && currentTick <= m_grenadeThrowTick - 8)
				m_grenadeSeekObserved = true;
			CameraBridge_SetFreeCamEnabled(true);
			CameraBridge_SetCameraPose(
				m_state.cameraPosition.x, m_state.cameraPosition.y, m_state.cameraPosition.z,
				m_state.cameraAngles.pitch, m_state.cameraAngles.yaw, m_state.cameraAngles.roll, m_state.fov);
			if (!m_grenadeSeekObserved)
				return;
		}

		if (!m_grenadeResumeIssued && g_pEngineToClient) {
			g_pEngineToClient->ExecuteClientCmd(0, "demo_resume", true);
			m_grenadeResumeIssued = true;
			m_lastMessage = "Waiting for the selected grenade to be thrown.";
		}

		if (currentTick >= m_grenadeThrowTick - 2 && ResolvePendingGrenade(currentTick)) {
			// Continue into the normal provider path this frame.
		} else if (currentTick > m_grenadeThrowTick + 96) {
			m_lastMessage = "Could not reacquire the selected grenade after its throw tick.";
			advancedfx::Warning("[followcam] grenade track failed: handle %llu did not appear near tick %d.\n",
				(unsigned long long)m_selectedGrenadeHandle, m_grenadeThrowTick);
			StopPreview("grenade could not be reacquired after seek");
			return;
		} else {
			CameraBridge_SetFreeCamEnabled(true);
			CameraBridge_SetCameraPose(
				m_state.cameraPosition.x, m_state.cameraPosition.y, m_state.cameraPosition.z,
				m_state.cameraAngles.pitch, m_state.cameraAngles.yaw, m_state.cameraAngles.roll, m_state.fov);
			return;
		}
	}

	auto provider = MakeProvider();
	if (!provider) return;

	if (m_state.mode == FollowMode::Attach) {
		const int activeEntityIndex = provider->EntityIndex();
		CEntityInstance* activeEntity = EntityAt(activeEntityIndex);
		bool isWeapon = false, isBomb = false;
		if (activeEntity) {
			const std::string cls = EntityClass(activeEntity);
			isWeapon = IsWeaponClass(cls);
			isBomb = IsBombClass(cls);
		}
		auto points = AvailableAttachPoints(activeEntity, m_state.targetType, isWeapon, isBomb);
		bool currentValid = false;
		for (const auto& point : points) {
			if (point.valid && point.id == m_state.attachmentName) {
				currentValid = true;
				break;
			}
		}
		if (!points.empty() && !currentValid) {
			const char* preferred = m_state.targetType == FollowTargetType::Player ? "head"
				: (m_state.targetType == FollowTargetType::Weapon ? "muzzle" : "entity");
			const FollowAttachPoint* fallback = nullptr;
			for (const auto& point : points) {
				if (point.valid && point.id == preferred) { fallback = &point; break; }
			}
			if (!fallback) {
				for (const auto& point : points) {
					if (point.valid) { fallback = &point; break; }
				}
			}
			if (fallback) {
				const std::string old = m_state.attachmentName;
				m_state.attachmentName = fallback->id;
				m_lastMessage = "Attach point '" + old + "' is not valid on this target; using '" + fallback->id + "'.";
				advancedfx::Warning("[followcam] attach point '%s' invalid for entity %d; using '%s'.\n",
					old.c_str(), activeEntityIndex, fallback->id.c_str());
				ResetMotion();
				provider = MakeProvider();
				if (!provider) return;
			}
		}
	}

	if (m_state.targetType == FollowTargetType::Player) {
		CEntityInstance* pawn = EntityAt(m_state.targetEntityIndex);
		if (pawn && pawn->IsPlayerPawn() && pawn->GetHealth() > 0) {
			m_deathRetargetElapsed = 0.0;
			auto weapon = pawn->GetActiveWeaponHandle();
			if (weapon.IsValid()) m_lastPlayerWeaponIndex = weapon.GetEntryIndex();
			m_lastPlayerBombIndex = -1;
			const int pawnIndex = pawn->GetHandle().IsValid() ? pawn->GetHandle().GetEntryIndex() : m_state.targetEntityIndex;
			for (int i = 0; i <= GetHighestEntityIndex(); ++i) {
				CEntityInstance* entity = EntityAt(i);
				if (!entity || !IsBombClass(EntityClass(entity))) continue;
				auto owner = entity->GetOwnerEntityHandle();
				if (owner.IsValid() && owner.GetEntryIndex() == pawnIndex) {
					m_lastPlayerBombIndex = i;
					break;
				}
			}
		}
	}

	if (!provider->IsValid()) {
		if (provider->GetStatus() == FollowTargetStatus::Dead) {
			const bool wantsRetarget = m_state.switchToDroppedWeaponOnDeath || m_state.switchToDroppedBombOnDeath;
			if (wantsRetarget) {
				m_deathRetargetElapsed += dt;
				if (TryDeathRetarget()) {
					provider = MakeProvider();
					m_deathRetargetElapsed = 0.0;
				} else if (m_deathRetargetElapsed < 0.75 && m_haveLastKnownTarget) {
					// Give CS2 a short window to detach/spawn the dropped entity after the
					// pawn health reaches zero. Keep pushing the last pose during the wait.
				} else if (m_state.autoDisableOnDeath) {
					StopPreview("target died and no retarget appeared");
					return;
				}
			} else if (m_state.autoDisableOnDeath) {
				StopPreview("target died");
				return;
			}
		} else if (!m_state.holdLastKnownPosition || !m_haveLastKnownTarget) {
			StopPreview("target disappeared");
			return;
		}
	}

	FollowVec3 rawTarget = m_haveLastKnownTarget ? m_lastKnownTarget : FollowVec3{};
	FollowAngles rawAngles{};
	bool rawOriented = false;
	const bool validNow = provider->IsValid();
	if (validNow) {
		rawOriented = provider->GetWorldTransform(rawTarget, rawAngles);
		if (!IsFiniteOrigin(rawTarget)) {
			StopPreview("target position invalid");
			return;
		}
		if (!demoAdvancing) {
			// Frozen demo: hold still. No velocity (so no spurious prediction / face-travel).
			m_targetVelocity = {};
		} else if (m_haveLastRawTarget && dt > 0.0001) {
			const double distance = FollowDistance(m_lastRawTarget, rawTarget);
			if (distance > kTeleportDistance) {
				m_motionInitialized = false;
				m_targetVelocity = {};
			} else {
				m_targetVelocity = FollowVec3{
					(rawTarget.x - m_lastRawTarget.x) / dt,
					(rawTarget.y - m_lastRawTarget.y) / dt,
					(rawTarget.z - m_lastRawTarget.z) / dt
				};
			}
		}
		m_lastRawTarget = rawTarget;
		m_haveLastRawTarget = true;
		m_lastKnownTarget = rawTarget;
		m_haveLastKnownTarget = true;
	}

	// ============================ ATTACH (ride-along) =========================
	// The camera rides the target: its position is the target/attachment point plus a
	// LOCAL offset, its orientation is the attachment orientation (or direction of
	// travel) plus the rotation trim, and FOV is user-set. No placed camera needed.
	if (m_state.mode == FollowMode::Attach) {
		FollowAngles baseAng = rawAngles;
		if (!rawOriented) {
			const double speed = std::sqrt(
				m_targetVelocity.x * m_targetVelocity.x +
				m_targetVelocity.y * m_targetVelocity.y +
				m_targetVelocity.z * m_targetVelocity.z);
			if (speed > 40.0)
				baseAng = FollowLookAt(FollowVec3{}, m_targetVelocity); // face travel
			else
				// Hold the offset-FREE base, NOT m_outputAngles. m_outputAngles already has the
				// rotation trim folded in, so using it here re-added the trim every frame ->
				// the camera spun continuously (faster at low smoothing). m_baseAngles excludes
				// the trim, so a stationary target produces a stable desired angle.
				baseAng = m_motionInitialized ? m_baseAngles : m_state.cameraAngles;
		}
		m_baseAngles = baseAng; // remember the trim-free base for the next stationary hold
		const FollowVec3 worldOffset = FollowRotateVector(m_state.offset, baseAng);
		const FollowVec3 desiredPos{
			rawTarget.x + worldOffset.x + m_targetVelocity.x * m_state.prediction,
			rawTarget.y + worldOffset.y + m_targetVelocity.y * m_state.prediction,
			rawTarget.z + worldOffset.z + m_targetVelocity.z * m_state.prediction
		};
		const FollowAngles desiredAng = FollowAddAngles(baseAng, m_state.rotationOffset);
		if (!m_motionInitialized) {
			m_smoothedCamPos = desiredPos;
			m_outputAngles = desiredAng;
			m_motionInitialized = true;
		} else {
			m_smoothedCamPos = FollowSmoothPosition(
				m_smoothedCamPos, desiredPos, dt, m_state.positionSmoothing);
			m_outputAngles = FollowSmoothAngles(m_outputAngles, desiredAng, dt,
				m_state.lookSmoothing, m_state.deadzone, m_state.maxTurnSpeed);
			m_outputAngles.roll = desiredAng.roll; // smoothing covers pitch/yaw only
		}
		m_lastCamPos = m_smoothedCamPos;
		m_lastCamAng = m_outputAngles;
		m_lastTargetPos = rawTarget;
		// Stash the rebase basis for ViewSetupAttachOverride: the smoothed camera's offset from the
		// (tick-stepped) target. When the mount has orientation, store it in local axes so render-time
		// rebase rotates it with the fresh bone/player orientation instead of carrying a stale
		// world-space offset across the interpolation boundary.
		const FollowVec3 attachWorldOffset{
			m_smoothedCamPos.x - rawTarget.x,
			m_smoothedCamPos.y - rawTarget.y,
			m_smoothedCamPos.z - rawTarget.z };
		m_attachOffsetFromTarget = rawOriented
			? FollowInverseRotateVector(attachWorldOffset, baseAng)
			: attachWorldOffset;
		m_attachOffsetIsLocal = rawOriented;
		m_attachBaseAngles = baseAng;
		m_attachAngleOffsetFromBase = FollowAngles{
			FollowWrapDegrees(m_outputAngles.pitch - baseAng.pitch),
			FollowWrapDegrees(m_outputAngles.yaw - baseAng.yaw),
			FollowWrapDegrees(m_outputAngles.roll - baseAng.roll)
		};
		m_attachBaseOriented = rawOriented;
		m_attachSteppedTarget = rawTarget;
		m_attachRebaseValid = true;
		const double pitchErr = FollowWrapDegrees(desiredAng.pitch - m_outputAngles.pitch);
		const double yawErr = FollowWrapDegrees(desiredAng.yaw - m_outputAngles.yaw);
		AttachTransformResult resolved = ResolveAttachTransform(
			EntityAt(provider->EntityIndex()), provider->EntityIndex(), m_state.targetType, m_state.attachmentName);
		m_attachDebug.active = true;
		m_attachDebug.valid = validNow && resolved.valid;
		m_attachDebug.oriented = rawOriented;
		m_attachDebug.entityIndex = provider->EntityIndex();
		m_attachDebug.entityHandle = resolved.entityHandle;
		m_attachDebug.entityType = FollowTargetTypeName(m_state.targetType);
		m_attachDebug.className = resolved.className;
		m_attachDebug.modelName = resolved.modelName;
		m_attachDebug.attachId = m_state.attachmentName;
		m_attachDebug.attachKind = resolved.attachKind;
		m_attachDebug.attachIndex = resolved.attachIndex;
		m_attachDebug.source = resolved.source;
		m_attachDebug.rawTarget = rawTarget;
		m_attachDebug.rawAngles = rawAngles;
		m_attachDebug.smoothedTarget = rawTarget;
		m_attachDebug.smoothedAngles = m_outputAngles;
		m_attachDebug.cameraPosition = m_smoothedCamPos;
		m_attachDebug.cameraAngles = m_outputAngles;
		m_attachDebug.distance = FollowDistance(m_smoothedCamPos, rawTarget);
		m_attachDebug.cameraDelta = m_motionInitialized && m_havePrevDebug ? FollowDistance(m_prevDebugCamPos, m_smoothedCamPos) : 0.0;
		m_attachDebug.targetDelta = m_havePrevDebug ? FollowDistance(m_prevDebugTargetPos, rawTarget) : 0.0;
		m_attachDebug.angleDelta = m_havePrevDebug
			? std::sqrt(std::pow(FollowWrapDegrees(m_outputAngles.pitch - m_prevDebugCamAng.pitch), 2.0)
				+ std::pow(FollowWrapDegrees(m_outputAngles.yaw - m_prevDebugCamAng.yaw), 2.0))
			: 0.0;
		m_attachDebug.aimError = std::sqrt(pitchErr * pitchErr + yawErr * yawErr);
		m_attachDebug.jitter = (m_attachDebug.cameraDelta > m_attachDebug.targetDelta + 0.01)
			? (m_attachDebug.cameraDelta - m_attachDebug.targetDelta) : 0.0;
		m_attachDebug.smoothing = m_state.positionSmoothing;
		m_attachDebug.previewTick = frameTick;
		m_attachDebug.demoTick = frameTick;
		m_prevDebugCamPos = m_smoothedCamPos;
		m_prevDebugTargetPos = rawTarget;
		m_prevDebugCamAng = m_outputAngles;
		m_havePrevDebug = true;
		CameraBridge_SetFreeCamEnabled(true);
		CameraBridge_SetCameraPose(
			m_smoothedCamPos.x, m_smoothedCamPos.y, m_smoothedCamPos.z,
			m_outputAngles.pitch, m_outputAngles.yaw, m_outputAngles.roll, m_state.fov);
		if (m_debug && ((m_debugFrame++ % 15) == 0)) {
			advancedfx::Message(
				"[followcam][attachdebug] type=%s entity=%d handle=%llu attach=%s kind=%s valid=%d source=%s "
				"target=(%.1f %.1f %.1f) cam=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f) "
				"dist=%.1f camDelta=%.2f targetDelta=%.2f angleDelta=%.2f mountError=%.2f jitter=%.2f tick=%d fov=%.1f.\n",
				FollowTargetTypeName(m_state.targetType), m_attachDebug.entityIndex,
				(unsigned long long)m_attachDebug.entityHandle, m_attachDebug.attachId.c_str(),
				m_attachDebug.attachKind.c_str(), m_attachDebug.valid ? 1 : 0, m_attachDebug.source.c_str(),
				rawTarget.x, rawTarget.y, rawTarget.z,
				m_smoothedCamPos.x, m_smoothedCamPos.y, m_smoothedCamPos.z,
				m_outputAngles.pitch, m_outputAngles.yaw, m_outputAngles.roll,
				m_attachDebug.distance, m_attachDebug.cameraDelta, m_attachDebug.targetDelta,
				m_attachDebug.angleDelta, m_attachDebug.aimError, m_attachDebug.jitter,
				frameTick, m_state.fov);
		}
		return;
	}

	// ============================ LOCK-ON (turret) ===========================
	FollowVec3 desiredTarget{
		rawTarget.x + m_state.offset.x + m_targetVelocity.x * m_state.prediction,
		rawTarget.y + m_state.offset.y + m_targetVelocity.y * m_state.prediction,
		rawTarget.z + m_state.offset.z + m_targetVelocity.z * m_state.prediction
	};

	if (m_retargeting && m_state.retargetBlendTime > 0.0001) {
		m_retargetElapsed += dt;
		const double t = (std::min)(1.0, m_retargetElapsed / m_state.retargetBlendTime);
		const double smooth = t * t * (3.0 - 2.0 * t);
		desiredTarget = FollowVec3{
			m_retargetFrom.x + (desiredTarget.x - m_retargetFrom.x) * smooth,
			m_retargetFrom.y + (desiredTarget.y - m_retargetFrom.y) * smooth,
			m_retargetFrom.z + (desiredTarget.z - m_retargetFrom.z) * smooth
		};
		if (t >= 1.0) m_retargeting = false;
	}

	if (!m_motionInitialized) {
		m_smoothedTarget = desiredTarget;
		m_outputAngles = m_state.cameraAngles;
		m_motionInitialized = true;
	} else {
		m_smoothedTarget = FollowSmoothPosition(
			m_smoothedTarget, desiredTarget, dt, m_state.positionSmoothing);
	}

	const FollowAngles desiredAngles = FollowLookAt(m_state.cameraPosition, m_smoothedTarget);
	m_outputAngles = FollowSmoothAngles(
		m_outputAngles, desiredAngles, dt, m_state.lookSmoothing,
		m_state.deadzone, m_state.maxTurnSpeed);

	// Rotation sliders trim the aim (pitch/yaw) and set the roll on top of the solve.
	const FollowAngles aimedAngles{
		FollowWrapDegrees(m_outputAngles.pitch + m_state.rotationOffset.pitch),
		FollowWrapDegrees(m_outputAngles.yaw + m_state.rotationOffset.yaw),
		FollowWrapDegrees(m_state.cameraAngles.roll + m_state.rotationOffset.roll)
	};
	m_lastCamPos = m_state.cameraPosition;
	m_lastCamAng = aimedAngles;
	m_lastTargetPos = rawTarget;

	CameraBridge_SetFreeCamEnabled(true);
	CameraBridge_SetCameraPose(
		m_state.cameraPosition.x, m_state.cameraPosition.y, m_state.cameraPosition.z,
		aimedAngles.pitch, aimedAngles.yaw, aimedAngles.roll, m_state.fov);

	if (m_debug && ((m_debugFrame++ % 15) == 0)) {
		advancedfx::Message(
			"[followcam][tick] type=%s entity=%d valid=%d name=%s dt=%.4f "
			"target=(%.1f %.1f %.1f) predicted=(%.1f %.1f %.1f) "
			"angle=(%.2f %.2f) desired=(%.2f %.2f) distance=%.1f freecam=%d.\n",
			FollowTargetTypeName(m_state.targetType), m_state.targetEntityIndex, validNow ? 1 : 0,
			provider->GetDisplayName().c_str(), dt,
			rawTarget.x, rawTarget.y, rawTarget.z,
			m_smoothedTarget.x, m_smoothedTarget.y, m_smoothedTarget.z,
			m_outputAngles.pitch, m_outputAngles.yaw, desiredAngles.pitch, desiredAngles.yaw,
			FollowDistance(m_state.cameraPosition, rawTarget),
			CameraBridge_GetFreeCamEnabled() ? 1 : 0);
	}
}

void FollowCamera::SetOffset(double x, double y, double z) {
	m_state.offset = FollowVec3{ x, y, z };
}

void FollowCamera::SetOffsetAxis(int axis, double value) {
	if (axis == 0) m_state.offset.x = value;
	else if (axis == 1) m_state.offset.y = value;
	else if (axis == 2) m_state.offset.z = value;
}

void FollowCamera::SetRotationOffset(double pitch, double yaw, double roll) {
	m_state.rotationOffset = FollowAngles{ pitch, yaw, roll };
}

void FollowCamera::SetRotationAxis(int axis, double value) {
	if (axis == 0) m_state.rotationOffset.pitch = value;
	else if (axis == 1) m_state.rotationOffset.yaw = value;
	else if (axis == 2) m_state.rotationOffset.roll = value;
}

void FollowCamera::SetFov(double value) {
	m_state.fov = std::clamp(value, 1.0, 170.0);
}

void FollowCamera::SetPreset(const std::string& name) {
	const std::string preset = Lower(name);
	if (preset == "above") SetOffset(0, 0, 48);
	else if (preset == "low" || preset == "lowground") SetOffset(0, 0, -48);
	else if (preset == "shoulder") SetOffset(0, 24, 24);
	else if (preset == "side") SetOffset(0, 64, 12);
	else if (preset == "behind") SetOffset(-64, 0, 16);
	else if (preset == "orbitleft") SetOffset(0, -96, 32);
	else if (preset == "orbitright") SetOffset(0, 96, 32);
	else if (preset == "weapon" || preset == "weaponclose") SetOffset(0, 0, 8);
	else if (preset == "bomb" || preset == "bombclose") SetOffset(0, 0, 12);
	else SetOffset(0, 0, 0);
}

void FollowCamera::ResetFraming() {
	ResetFramingState();
	ResetMotion();
	m_lastMessage = (m_state.mode == FollowMode::Attach)
		? "Attach framing reset to defaults."
		: "Framing reset to defaults.";
	advancedfx::Message("[followcam] %s framing reset to defaults.\n",
		m_state.mode == FollowMode::Attach ? "attach" : "lock-on");
}

void FollowCamera::SetLookSmoothing(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.lookSmoothing = 0.0;
		return;
	}
	m_state.lookSmoothing = std::clamp(value, 0.0, 5.0);
	m_lockOnLookSmoothing = m_state.lookSmoothing;
}
void FollowCamera::SetPositionSmoothing(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.positionSmoothing = 0.0;
		return;
	}
	m_state.positionSmoothing = std::clamp(value, 0.0, 5.0);
	m_lockOnPositionSmoothing = m_state.positionSmoothing;
}
void FollowCamera::SetPrediction(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.prediction = 0.0;
		return;
	}
	m_state.prediction = std::clamp(value, 0.0, 2.0);
	m_lockOnPrediction = m_state.prediction;
}
void FollowCamera::SetDeadzone(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.deadzone = 0.0;
		return;
	}
	m_state.deadzone = std::clamp(value, 0.0, 45.0);
	m_lockOnDeadzone = m_state.deadzone;
}
void FollowCamera::SetMaxTurnSpeed(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.maxTurnSpeed = 0.0;
		return;
	}
	m_state.maxTurnSpeed = std::clamp(value, 0.0, 4000.0);
	m_lockOnMaxTurnSpeed = m_state.maxTurnSpeed;
}

void FollowCamera::ResetTracking() {
	ResetTrackingState();
	ResetMotion();
	m_lastMessage = (m_state.mode == FollowMode::Attach)
		? "Attach tracking reset to stable defaults."
		: "Advanced tracking reset to defaults.";
	advancedfx::Message("[followcam] %s tracking reset to defaults.\n",
		m_state.mode == FollowMode::Attach ? "attach" : "lock-on");
}

void FollowCamera::SetAttachmentName(const std::string& value) {
	if (!value.empty()) {
		m_state.attachmentName = value.substr(0, 64);
		ResetMotion();
	}
}

void FollowCamera::PrintStatus() const {
	advancedfx::Message(
		"[followcam] enabled=%d hasCamera=%d type=%s entity=%d fov=%.1f "
		"offset=(%.1f %.1f %.1f) look=%.3f pos=%.3f prediction=%.3f "
		"deadzone=%.2f maxTurn=%.1f autoDead=%d switchWeapon=%d switchBomb=%d hold=%d debug=%d rtsample=%d.\n",
		m_state.enabled ? 1 : 0, m_state.hasCamera ? 1 : 0,
		FollowTargetTypeName(m_state.targetType), m_state.targetEntityIndex, m_state.fov,
		m_state.offset.x, m_state.offset.y, m_state.offset.z,
		m_state.lookSmoothing, m_state.positionSmoothing, m_state.prediction,
		m_state.deadzone, m_state.maxTurnSpeed,
		m_state.autoDisableOnDeath ? 1 : 0, m_state.switchToDroppedWeaponOnDeath ? 1 : 0,
		m_state.switchToDroppedBombOnDeath ? 1 : 0, m_state.holdLastKnownPosition ? 1 : 0,
		m_debug ? 1 : 0, m_renderTimeSample ? 1 : 0);
}

void FollowCamera::PrintCamPose() const {
	advancedfx::Message(
		"[followcam][campose] enabled=%d mode=%s type=%s pos=(%.2f %.2f %.2f) "
		"ang=(%.2f %.2f %.2f) fov=%.1f target=(%.2f %.2f %.2f).\n",
		m_state.enabled ? 1 : 0,
		m_state.mode == FollowMode::Attach ? "attach" : "lockon",
		FollowTargetTypeName(m_state.targetType),
		m_lastCamPos.x, m_lastCamPos.y, m_lastCamPos.z,
		m_lastCamAng.pitch, m_lastCamAng.yaw, m_lastCamAng.roll, m_state.fov,
		m_lastTargetPos.x, m_lastTargetPos.y, m_lastTargetPos.z);
}

} // namespace Filmmaker
