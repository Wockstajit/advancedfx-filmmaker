#pragma once

// CameraPath: a Black Ops 2 Theater-style "dolly camera" path system, built as a
// front-end over HLAE's existing camera path engine (g_CamPath in main.cpp) +
// keyframe drawer (g_CampathDrawer).
//
// This class is now a THIN FACADE. The heavy lifting lives in three modules it
// owns and wires together:
//   * CamMarkers  - the authoritative marker list + JSON sidecar (plain data).
//   * CamPathEval - rebuilds g_CamPath on the speed-INDEPENDENT shape axis and
//                   exposes PURE pose evaluators (EvalAtTick/Timing/DemoTime).
//   * CamPlayback - the predictive Live clock + Freeze wall-clock + output
//                   low-pass + the deterministic SCRUB mode.
// The facade keeps the editing-mode state machine, hover picking, drawer styling,
// the transient notice, persistence orchestration, and the public API the rest of
// the filmmaker code already calls (FilmmakerCommand, MovieMode, MarkerHud,
// CameraTimelineHud, main.cpp's suspend hook).
//
// Freeze vs Live is a GLOBAL mode: all markers share m_timing.
//   * Freeze: the demo is paused and the dolly glides on wall-clock time.
//   * Live:   the demo keeps playing and the camera follows recorded demo time.
//
// Threading: every mutating method runs on the main/UI thread (invoked from the
// mirv_filmmaker console dispatch). Fields read off-thread by MovieMode::OnKey
// (WndProc thread) are atomic: m_hovered, m_mode and m_menuOpen.

#include "CamMarkers.h"
#include "CamPathEval.h"
#include "CamPlayback.h"

#include <atomic>
#include <string>
#include <vector>

namespace Filmmaker {

class CameraPath {
public:
	enum class SpeedMode { Manual = 0, Constant = 1, PerSegment = 2 };
	enum class Timing { Live = 0, Freeze = 1 };
	enum class Interp { Linear = 0, Bezier = 1 }; // "Bezier" == natural-cubic "Smooth" (UI label)
	enum class Mode { Editing = 0, Reposition = 1, PreviewArmed = 2, PreviewPlaying = 3 };

	// Main/UI thread, once per frame (from Filmmaker::RunMainThreadFrame).
	void RunFrame();

	// --- editing actions (main thread; dispatched from the console cmd) ---
	void PlaceMarker();                 // K
	void DeleteIndex(int index);        // L (aimed) / menu
	void DeleteAll(bool confirmed);     // menu (with confirm)
	void SelectIndex(int index, bool teleport);
	void SelectDelta(int delta);        // menu arrows (teleports to the camera)
	void OpenMenu(int index);           // F (aimed)
	void CloseMenu();

	// --- reposition (move the selected marker interactively) ---
	void BeginReposition();             // menu "Reposition" button
	void PlaceReposition();             // left-click while repositioning
	void CancelReposition();            // X / Esc while repositioning

	// --- camera-path playback ---
	void ArmPreview();                  // J: jump to first marker + pause; wait for Space
	void StartPreviewPlay();            // Space: begin playback (BO2 arm flow)
	void PlayPath();                    // timeline play: Live, no jump, dolly only in marker range
	void Play() { ArmPreview(); }       // console/dashboard alias for the J arm step
	void StopPreview();                 // X: stop + return to editing
	void TogglePreviewHud();            // Tab: hide/show HUD during playback

	// --- settings ---
	void SetSpeedMode(SpeedMode m);
	void SetTiming(Timing t);
	void SetInterp(Interp i);
	void CycleSpeedMode();
	void ToggleTiming();
	void CycleInterp();
	void SetSelectedSpeedMul(float mul); // selected marker's OUTGOING segment speed
	void SetConstSpeed(float mul);       // global constant-speed multiplier
	void CycleConstSpeed();
	void SetAutoSnap(bool on);           // teleport viewer to a marker when selected?
	void ToggleAutoSnap();

	// --- camera timeline / curve editor (plan Part C/D) ---
	// Scrub to a tick. seek=true also moves the demo world (demo_gototick) -- do this on
	// slider RELEASE. seek=false only previews the CAMERA along the path (pure EvalAtTick,
	// no engine seek) -- do this while DRAGGING so the camera glides smoothly.
	void ScrubToTick(double tick, bool seek = true);
	void StopScrub();                    // leave scrub mode (resume normal control)
	bool IsScrubbing() const { return m_play.Scrubbing(); }
	double ScrubTick() const { return m_play.ScrubTick(); }
	void MoveKey(int index, int newTick);                 // retime a marker (curve drag)
	void SetChannelValue(int index, int channel, double v); // 0..6 = x,y,z,pitch,yaw,roll,fov
	void SetEaseIndex(int index, Ease e);
	void SetSpeedMulIndex(int index, float mul);
	// Pure pose at a demo tick (curve sampling + scrub readout). false => off-path.
	bool EvalPoseAtTick(double tick, double out[7]) const;

	// --- reads for the UI/HUD (main thread) ---
	int Count() const { return m_data.Count(); }
	int Selected() const { return m_data.Selected(); }
	int Hovered() const { return m_hovered.load(); }
	int HoveredAtomic() const { return m_hovered.load(); } // safe from WndProc thread
	Mode GetMode() const { return (Mode)m_mode.load(); }   // safe from WndProc thread
	bool IsPlaying() const { return m_play.Playing(); }
	bool MenuOpen() const { return m_menuOpen.load() && GetMode() == Mode::Editing; }
	bool WantsCursor() const { return MenuOpen(); } // suspend free-cam look + show cursor
	bool HudHidden() const { return m_hudHidden; }
	bool PreviewHudHidden() const;                  // HUD masked during preview
	SpeedMode GetSpeedMode() const { return m_speedMode; }
	Timing GetTiming() const { return m_timing; }
	Interp GetInterp() const { return m_interp; }
	float SelectedSpeedMul() const;
	float ConstSpeed() const { return m_constSpeed; }
	bool AutoSnap() const { return m_autoSnap; }
	bool SelectedIsLast() const { return m_data.SelectedIsLast(); }
	int PlaySegment() const { return m_play.Segment(); } // current segment while playing (0-based)
	const char* Notice() const;                     // transient on-screen message ("" if none)
	const char* SpeedModeName() const;
	const char* TimingName() const;
	const char* InterpName() const;
	const char* ModeName() const;

	// Read-only marker access for the timeline / curve editor.
	const std::vector<CamMarker>& Markers() const { return m_data.All(); }

	// Persistence (per-demo JSON sidecar next to the .dem).
	void Save();
	void Load();

private:
	void RebuildCamPath();   // m_eval.Rebuild(m_data, settings)
	void MarkDirty() { m_dirty = true; }
	void EnsureDrawState();  // toggle the in-world gizmos with marker presence
	void SyncDrawerStyle();  // push Freeze/Live colour + aimed-highlight to the drawer
	void UpdateHover();      // crosshair pick -> m_hovered
	void PushPose(const CamMarker& m); // push a marker pose to the camera this frame
	double NowSec();         // monotonic seconds (QPC) for the notice timer
	void Notify(const char* msg);      // raise a transient on-screen message
	void SetMode(Mode m) { m_mode.store((int)m); }
	std::wstring SidecarPath() const;
	PathSettings MakeSettings() const; // gather the persisted settings for eval/save
	CamPlayback::Context MakeContext() const; // cosmetic/mode context for the driver

	CamMarkers m_data;
	CamPathEval m_eval;
	CamPlayback m_play;

	SpeedMode m_speedMode = SpeedMode::Manual;
	Timing m_timing = Timing::Live;
	Interp m_interp = Interp::Linear; // Linear default so 2-3 markers can play
	float m_constSpeed = 1.0f;        // global multiplier for Constant speed mode
	bool m_autoSnap = false;          // selecting a marker teleports the viewer to it (default OFF)

	// Read off-thread by OnKey (key routing) -> atomic.
	std::atomic<int> m_hovered{ -1 };
	std::atomic<int> m_mode{ (int)Mode::Editing };
	// Read off-thread (OnKey ESC + main.cpp GetSuspendMirvInput) -> atomic.
	std::atomic<bool> m_menuOpen{ false };
	bool m_hudHidden = false;       // Tab during preview hides the HUD overlays
	bool m_drawOn = false;
	bool m_dirty = false;

	std::string m_notice;          // transient on-screen message
	double m_noticeExpireSec = 0.0;// NowSec() after which the notice clears

	std::wstring m_demoPath;       // current demo path; empty => no auto-save
};

CameraPath& CameraPathRef();

} // namespace Filmmaker
