#include "CameraPath.h"

#include "CameraBridge.h"
#include "../Filmmaker.h"            // CurrentDemoPath()
#include "../../MirvTime.h"

#include "../../../shared/AfxConsole.h"

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>
#include <cmath>
#include <sstream>

// Engine pointer (same one MovieMode uses) for demo pause/seek during preview/scrub.
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {
	constexpr double kPi = 3.14159265358979323846;
	constexpr double kAimDot = 0.990; // ~8 deg crosshair cone for marker picking

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
	void DemoCmd(const char* c) {
		if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, c, true);
	}
}

CameraPath& CameraPathRef() {
	static CameraPath s_instance;
	return s_instance;
}

// --- name / read helpers ---

const char* CameraPath::SpeedModeName() const {
	switch (m_speedMode) {
	case SpeedMode::Constant: return "Constant";
	case SpeedMode::PerSegment: return "Per-Segment";
	default: return "Manual";
	}
}
const char* CameraPath::TimingName() const { return m_timing == Timing::Freeze ? "Freeze" : "Live"; }
// "Bezier" is the natural-cubic mode; the UI label is "Smooth" (no draggable handles).
const char* CameraPath::InterpName() const { return m_interp == Interp::Bezier ? "Smooth" : "Linear"; }
const char* CameraPath::ModeName() const {
	switch (GetMode()) {
	case Mode::Reposition: return "Reposition";
	case Mode::PreviewArmed: return "PreviewArmed";
	case Mode::PreviewPlaying: return "PreviewPlaying";
	default: return "Editing";
	}
}

bool CameraPath::PreviewHudHidden() const {
	Mode m = GetMode();
	return m_hudHidden && (m == Mode::PreviewArmed || m == Mode::PreviewPlaying);
}

float CameraPath::SelectedSpeedMul() const {
	int sel = m_data.Selected();
	if (!m_data.ValidIndex(sel)) return 1.0f;
	return m_data.At(sel).speedMul;
}

std::wstring CameraPath::SidecarPath() const {
	// "<demo>.dem" + ".campath.json" -> "<demo>.dem.campath.json"
	return m_demoPath + L".campath.json";
}

PathSettings CameraPath::MakeSettings() const {
	PathSettings s;
	s.speedMode = (int)m_speedMode;
	s.timing = (int)m_timing;
	s.interp = (int)m_interp;
	s.constSpeed = m_constSpeed;
	s.autoSnap = m_autoSnap;
	return s;
}

CamPlayback::Context CameraPath::MakeContext() const {
	CamPlayback::Context c;
	c.timing = (int)m_timing;
	c.speedMode = (int)m_speedMode;
	c.interpName = InterpName();
	c.speedModeName = SpeedModeName();
	c.constSpeed = m_constSpeed;
	return c;
}

// --- internal frame helpers ---

void CameraPath::RebuildCamPath() {
	m_eval.Rebuild(m_data, MakeSettings());
}

void CameraPath::EnsureDrawState() {
	bool want = !m_data.Empty();
	if (want != m_drawOn) {
		CameraBridge_SetPathDrawEnabled(want);
		m_drawOn = want;
	}
}

void CameraPath::SyncDrawerStyle() {
	CameraBridge_SetMarkerStyle(!m_data.Empty(), m_timing == Timing::Freeze, m_hovered.load());
}

double CameraPath::NowSec() {
	LARGE_INTEGER freq, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	return (double)now.QuadPart / (double)freq.QuadPart;
}

void CameraPath::Notify(const char* msg) {
	m_notice = msg ? msg : "";
	m_noticeExpireSec = NowSec() + 3.5;
}

const char* CameraPath::Notice() const { return m_notice.c_str(); }

void CameraPath::PushPose(const CamMarker& m) {
	CameraBridge_SetCameraPose(m.x, m.y, m.z, m.pitch, m.yaw, m.roll, m.fov);
}

// --- editing actions ---

void CameraPath::PlaceMarker() {
	CamMarker m;
	double o[3], a[3], fov;
	CameraBridge_GetCurrentCamera(o, a, fov);
	m.x = o[0]; m.y = o[1]; m.z = o[2];
	m.pitch = a[0]; m.yaw = a[1]; m.roll = a[2]; m.fov = fov;
	int tick = 0; if (g_MirvTime.GetCurrentDemoTick(tick)) m.tick = tick;
	double dt = 0.0; if (g_MirvTime.GetCurrentDemoTime(dt)) m.time = dt;
	m.speedMul = 1.0f; m.ease = Ease::None;

	int idx = m_data.Add(m);
	RebuildCamPath();
	EnsureDrawState();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d placed @ tick %d (%.1f %.1f %.1f) fov %.1f.\n",
		idx, m.tick, m.x, m.y, m.z, m.fov);
}

void CameraPath::DeleteIndex(int index) {
	if (!m_data.ValidIndex(index)) {
		advancedfx::Message("[campath] delete: no marker #%d.\n", index);
		return;
	}
	m_data.DeleteIndex(index);
	if (m_data.Empty()) m_menuOpen = false;
	RebuildCamPath();
	EnsureDrawState();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d deleted (%d left).\n", index, Count());
}

void CameraPath::DeleteAll(bool confirmed) {
	if (!confirmed) {
		advancedfx::Message("[campath] deleteall: pass 'confirm' to clear all markers.\n");
		return;
	}
	int c = Count();
	m_data.DeleteAll();
	m_menuOpen = false;
	m_play.Stop();
	SetMode(Mode::Editing);
	RebuildCamPath();
	EnsureDrawState();
	MarkDirty();
	advancedfx::Message("[campath] all markers deleted (%d).\n", c);
}

void CameraPath::SelectIndex(int index, bool teleport) {
	if (m_data.Empty()) return;
	m_data.SetSelected(index);
	int sel = m_data.Selected();
	// Only move the viewer to the marker when Auto-Snap is on.
	if (teleport && m_autoSnap) {
		CameraBridge_SetFreeCamEnabled(true);
		PushPose(m_data.At(sel));
	}
	advancedfx::Message("[campath] selected marker #%d%s.\n", sel,
		(teleport && !m_autoSnap) ? " (auto-snap off, viewer not moved)" : "");
}

void CameraPath::SelectDelta(int delta) {
	if (m_data.Empty()) return;
	int base = (m_data.Selected() < 0) ? 0 : m_data.Selected();
	SelectIndex(base + delta, /*teleport*/ true);
}

void CameraPath::OpenMenu(int index) {
	if (m_data.Empty()) return;
	SetMode(Mode::Editing);
	SelectIndex(index, /*teleport*/ false);
	CameraBridge_SetFreeCamEnabled(true);
	m_menuOpen = true;
	EnsureDrawState();
	advancedfx::Message("[campath] edit menu open for marker #%d.\n", m_data.Selected());
}

void CameraPath::CloseMenu() { m_menuOpen = false; }

// --- settings ---

void CameraPath::SetSpeedMode(SpeedMode m) { m_speedMode = m; RebuildCamPath(); MarkDirty(); advancedfx::Message("[campath] speed mode -> %s.\n", SpeedModeName()); }
void CameraPath::SetTiming(Timing t) { m_timing = t; MarkDirty(); advancedfx::Message("[campath] timing -> %s (all cameras).\n", TimingName()); }
void CameraPath::SetInterp(Interp i) { m_interp = i; RebuildCamPath(); MarkDirty(); advancedfx::Message("[campath] interpolation -> %s.\n", InterpName()); }
void CameraPath::CycleSpeedMode() { SetSpeedMode((SpeedMode)(((int)m_speedMode + 1) % 3)); }
void CameraPath::ToggleTiming() { SetTiming(m_timing == Timing::Live ? Timing::Freeze : Timing::Live); }
void CameraPath::CycleInterp() { SetInterp(m_interp == Interp::Linear ? Interp::Bezier : Interp::Linear); }

void CameraPath::SetSelectedSpeedMul(float mul) {
	int sel = m_data.Selected();
	if (!m_data.ValidIndex(sel)) return;
	if (m_data.SelectedIsLast()) {
		Notify("Last marker has no outgoing segment.");
		advancedfx::Message("[campath] marker #%d is the last marker: no outgoing segment.\n", sel);
		return;
	}
	if (mul < 0.2f) mul = 0.2f; else if (mul > 1.0f) mul = 1.0f;
	m_data.At(sel).speedMul = mul;
	RebuildCamPath();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d outgoing segment speed x%.2f.\n", sel, mul);
}

void CameraPath::SetConstSpeed(float mul) {
	if (mul < 0.2f) mul = 0.2f; else if (mul > 1.0f) mul = 1.0f;
	m_constSpeed = mul;
	RebuildCamPath();
	MarkDirty();
	advancedfx::Message("[campath] constant speed x%.2f (whole path).\n", m_constSpeed);
}

void CameraPath::CycleConstSpeed() {
	// 1.0 -> 0.8 -> 0.5 -> 0.2 -> 1.0
	float next;
	if (m_constSpeed > 0.9f) next = 0.8f;
	else if (m_constSpeed > 0.65f) next = 0.5f;
	else if (m_constSpeed > 0.35f) next = 0.2f;
	else next = 1.0f;
	SetConstSpeed(next);
}

void CameraPath::SetAutoSnap(bool on) {
	m_autoSnap = on;
	MarkDirty();
	advancedfx::Message("[campath] auto-snap to selected camera -> %s.\n", on ? "ON" : "OFF");
}
void CameraPath::ToggleAutoSnap() { SetAutoSnap(!m_autoSnap); }

// --- reposition ---

void CameraPath::BeginReposition() {
	int sel = m_data.Selected();
	if (!m_data.ValidIndex(sel)) {
		advancedfx::Message("[campath] reposition: select/aim at a marker first.\n");
		return;
	}
	m_menuOpen = false;
	CameraBridge_SetFreeCamEnabled(true);
	SetMode(Mode::Reposition);
	advancedfx::Message("[campath] repositioning marker #%d: move + left-click to place (X/Esc cancel).\n", sel);
}

void CameraPath::PlaceReposition() {
	if (GetMode() != Mode::Reposition) return;
	int sel = m_data.Selected();
	if (!m_data.ValidIndex(sel)) { SetMode(Mode::Editing); return; }
	CamMarker& m = m_data.At(sel);
	double o[3], a[3], fov;
	CameraBridge_GetCurrentCamera(o, a, fov);
	m.x = o[0]; m.y = o[1]; m.z = o[2];
	m.pitch = a[0]; m.yaw = a[1]; m.roll = a[2]; m.fov = fov;
	int tick = 0; if (g_MirvTime.GetCurrentDemoTick(tick)) m.tick = tick;
	double dt = 0.0; if (g_MirvTime.GetCurrentDemoTime(dt)) m.time = dt;
	RebuildCamPath();
	MarkDirty();
	SetMode(Mode::Editing);
	advancedfx::Message("[campath] marker #%d repositioned @ tick %d (%.1f %.1f %.1f).\n",
		sel, m.tick, m.x, m.y, m.z);
}

void CameraPath::CancelReposition() {
	if (GetMode() != Mode::Reposition) return;
	SetMode(Mode::Editing);
	advancedfx::Message("[campath] reposition cancelled.\n");
}

// --- camera-path playback ---

void CameraPath::ArmPreview() {
	const int n = m_data.Count();
	if (n < 2) {
		Notify("Need at least 2 camera markers to play path.");
		advancedfx::Message("[campath] preview refused: need >=2 markers (have %d).\n", n);
		return;
	}

	m_data.SetSelected(0);     // marker #1 is always the start of the rebuilt path
	RebuildCamPath();
	if (!m_eval.CanEval()) {
		Notify("Camera path could not be built.");
		advancedfx::Message("[campath] preview refused: path not evaluable (add markers).\n");
		return;
	}

	m_menuOpen = false;
	m_hudHidden = false;
	m_play.Stop();

	// Seek to the first marker and pause so the start frame is stable while composing.
	{
		std::ostringstream oss; oss << "demo_gototick " << m_data.Front().tick;
		DemoCmd(oss.str().c_str());
	}
	if (DemoIsPlaying() && !DemoIsPaused()) DemoCmd("demo_pause");

	CameraBridge_SetFreeCamEnabled(true);
	m_play.Reset();
	SetMode(Mode::PreviewArmed);

	advancedfx::Message(
		"[campath] preview ARMED: jumped to first marker (tick %d). Press SPACE to play, X to cancel.\n",
		m_data.Front().tick);
}

void CameraPath::StartPreviewPlay() {
	if (GetMode() != Mode::PreviewArmed) {
		ArmPreview();
		if (GetMode() != Mode::PreviewArmed) return; // arm failed (e.g. <2 markers)
	}
	RebuildCamPath();
	if (!m_eval.CanEval()) { advancedfx::Message("[campath] cannot evaluate path.\n"); return; }

	m_play.StartPlay(m_eval.Duration());
	SetMode(Mode::PreviewPlaying);

	if (m_timing == Timing::Live) { if (DemoIsPaused()) DemoCmd("demo_resume"); }

	advancedfx::Message(
		"[campath] PLAY: %d markers, speedMode=%s, interp=%s, timing=%s, duration=%.2fs.\n",
		Count(), SpeedModeName(), InterpName(), TimingName(), m_eval.Duration());
}

// Timeline "play": play the path from the demo's CURRENT position without jumping to
// the first marker. In Live timing the dolly only takes over once the playhead reaches
// the first marker's tick (CamPlayback range-gating), so the camera isn't forced onto
// the path before its first keyframe; in Freeze it glides on wall-clock as usual.
void CameraPath::PlayPath() {
	const int n = m_data.Count();
	if (n < 2) {
		Notify("Need at least 2 camera markers to play path.");
		advancedfx::Message("[campath] play refused: need >=2 markers (have %d).\n", n);
		return;
	}
	RebuildCamPath();
	if (!m_eval.CanEval()) {
		Notify("Camera path could not be built.");
		return;
	}
	m_menuOpen = false;
	m_hudHidden = false;
	// Freeze plays self-contained on wall-clock and needs the free cam on now; Live
	// range-gating toggles the free cam itself as the playhead enters/leaves the range.
	if (m_timing == Timing::Freeze) CameraBridge_SetFreeCamEnabled(true);
	m_play.StartPlay(m_eval.Duration(), /*rangeGated*/ m_timing == Timing::Live);
	SetMode(Mode::PreviewPlaying);
	if (m_timing == Timing::Live) { if (DemoIsPaused()) DemoCmd("demo_resume"); }
	advancedfx::Message(
		"[campath] timeline PLAY: %d markers, timing=%s, interp=%s (no jump; dolly active within marker range).\n",
		Count(), TimingName(), InterpName());
}

void CameraPath::StopPreview() {
	bool was = (GetMode() == Mode::PreviewArmed || GetMode() == Mode::PreviewPlaying);
	m_play.Stop();
	m_hudHidden = false;
	SetMode(Mode::Editing);
	if (was) advancedfx::Message("[campath] preview stopped (back to editing).\n");
}

void CameraPath::TogglePreviewHud() {
	Mode m = GetMode();
	if (m != Mode::PreviewArmed && m != Mode::PreviewPlaying) return;
	m_hudHidden = !m_hudHidden;
	advancedfx::Message("[campath] preview HUD %s.\n", m_hudHidden ? "hidden" : "shown");
}

// --- camera timeline / curve editor ---

void CameraPath::ScrubToTick(double tick, bool seek) {
	if (m_data.Count() < 2) {
		Notify("Need at least 2 markers to scrub the path.");
		return;
	}

	// Deterministic scrub happens while PAUSED so the same tick always lands the same
	// pose. The pure EvalAtTick path (TickScrub) bypasses the clock + pose low-pass, so
	// dragging glides the CAMERA smoothly with no engine seek; the world is moved only on
	// release (seek=true) because demo_gototick is an expensive engine seek (choppy if
	// done every tick while dragging). Rebuild/pause/free-cam only when ENTERING scrub so
	// per-drag previews stay cheap.
	if (!m_play.Scrubbing()) {
		RebuildCamPath();
		if (!m_eval.CanEval()) return;
		if (DemoIsPlaying() && !DemoIsPaused()) DemoCmd("demo_pause");
		CameraBridge_SetFreeCamEnabled(true);
	}
	if (seek) {
		int it = (int)(tick + 0.5);
		if (it < 0) it = 0;
		std::ostringstream oss; oss << "demo_gototick " << it; DemoCmd(oss.str().c_str());
	}
	m_play.BeginScrub(tick);
}

void CameraPath::StopScrub() { m_play.EndScrub(); }

void CameraPath::MoveKey(int index, int newTick) {
	if (!m_data.ValidIndex(index)) return;
	const int n = m_data.Count();
	// Keep markers ordered: clamp between the neighbours' ticks.
	int lo = (index > 0) ? m_data.At(index - 1).tick + 1 : 0;
	if (newTick < lo) newTick = lo;
	if (index < n - 1) { int hi = m_data.At(index + 1).tick - 1; if (newTick > hi) newTick = hi; }
	if (newTick < 0) newTick = 0;

	CamMarker& m = m_data.At(index);
	m.tick = newTick;
	float ipt = g_MirvTime.interval_per_tick_get();
	if (ipt > 0.0f) m.time = (double)newTick * ipt;
	RebuildCamPath();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d moved to tick %d.\n", index, newTick);
}

void CameraPath::SetChannelValue(int index, int channel, double v) {
	if (!m_data.ValidIndex(index)) return;
	CamMarker& m = m_data.At(index);
	switch (channel) {
	case 0: m.x = v; break;
	case 1: m.y = v; break;
	case 2: m.z = v; break;
	case 3: m.pitch = v; break;
	case 4: m.yaw = v; break;
	case 5: m.roll = v; break;
	case 6: m.fov = v; break;
	default: return;
	}
	RebuildCamPath();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d channel %d -> %.3f.\n", index, channel, v);
}

void CameraPath::SetEaseIndex(int index, Ease e) {
	if (!m_data.ValidIndex(index)) return;
	m_data.At(index).ease = e;
	MarkDirty(); // easing is applied at eval time; no geometry rebuild needed
	advancedfx::Message("[campath] marker #%d ease -> %d.\n", index, (int)e);
}

void CameraPath::SetSpeedMulIndex(int index, float mul) {
	if (!m_data.ValidIndex(index)) return;
	if (index == m_data.Count() - 1) {
		Notify("Last marker has no outgoing segment.");
		return;
	}
	if (mul < 0.2f) mul = 0.2f; else if (mul > 1.0f) mul = 1.0f;
	m_data.At(index).speedMul = mul;
	RebuildCamPath();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d outgoing segment speed x%.2f.\n", index, mul);
}

bool CameraPath::EvalPoseAtTick(double tick, double out[7]) const {
	CamPathEval::Pose p = m_eval.EvalAtTick(m_data, tick);
	if (!p.valid) return false;
	out[0] = p.x; out[1] = p.y; out[2] = p.z;
	out[3] = p.pitch; out[4] = p.yaw; out[5] = p.roll;
	out[6] = p.fov;
	return true;
}

// --- hover picking ---

void CameraPath::UpdateHover() {
	const int n = m_data.Count();
	if (n == 0) { m_hovered.store(-1); return; }

	double o[3], a[3], fov;
	CameraBridge_GetCurrentCamera(o, a, fov);
	double pr = a[0] * kPi / 180.0; // pitch
	double yr = a[1] * kPi / 180.0; // yaw
	double fx = std::cos(pr) * std::cos(yr);
	double fy = std::cos(pr) * std::sin(yr);
	double fz = -std::sin(pr);

	int best = -1;
	double bestDot = kAimDot;
	for (int i = 0; i < n; ++i) {
		const CamMarker& mk = m_data.At(i);
		double dx = mk.x - o[0];
		double dy = mk.y - o[1];
		double dz = mk.z - o[2];
		double len = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (len < 1.0) { best = i; bestDot = 1.0; continue; }
		double dot = (dx * fx + dy * fy + dz * fz) / len;
		if (dot > bestDot) { bestDot = dot; best = i; }
	}
	int prev = m_hovered.exchange(best);
	if (prev != best && best >= 0)
		advancedfx::Message("[campath] aiming at marker #%d.\n", best);
}

// --- per-frame ---

void CameraPath::RunFrame() {
	// Auto-load when the active demo changes (path tracked by Filmmaker::Watch).
	std::wstring cur = CurrentDemoPath();
	if (cur != m_demoPath) {
		m_demoPath = cur;
		m_data.DeleteAll();
		m_menuOpen = false;
		m_play.Stop();
		m_hudHidden = false;
		SetMode(Mode::Editing);
		Load();              // no-op if no sidecar
		RebuildCamPath();
		EnsureDrawState();
	}

	// Fail-safe: if a mode/scrub was left active when the demo stopped, return to a
	// clean editing state so MirvInput isn't left suspended with no closable panel.
	if (GetMode() != Mode::Editing || m_menuOpen || m_play.Playing() || m_play.Scrubbing()) {
		if (!DemoIsPlaying()) {
			m_menuOpen = false;
			m_play.Stop();
			m_hudHidden = false;
			SetMode(Mode::Editing);
		}
	}

	// Expire the transient notice banner.
	if (!m_notice.empty() && NowSec() >= m_noticeExpireSec) m_notice.clear();

	UpdateHover();

	Mode mode = GetMode();
	if (mode == Mode::PreviewArmed && !m_data.Empty()) {
		PushPose(m_data.Front()); // hold the camera at the start frame
	} else if (mode == Mode::PreviewPlaying) {
		if (m_play.TickPlay(m_data, m_eval, MakeContext())) {
			advancedfx::Message("[campath] play reached end of path (%d markers).\n", m_data.Count());
			StopPreview();
		}
	} else if (m_play.Scrubbing()) {
		m_play.TickScrub(m_data, m_eval); // deterministic tick-perfect preview (paused)
	}

	SyncDrawerStyle();

	if (m_dirty) { Save(); m_dirty = false; }
}

// --- persistence (delegates the schema to CamMarkers) ---

void CameraPath::Save() {
	if (m_demoPath.empty()) return; // unknown demo -> memory only
	m_data.Save(SidecarPath(), MakeSettings(), m_data.Selected());
}

void CameraPath::Load() {
	if (m_demoPath.empty()) return;
	PathSettings st = MakeSettings(); // seed with current defaults
	int sel = m_data.Selected();
	if (m_data.Load(SidecarPath(), st, sel)) {
		m_speedMode = (SpeedMode)st.speedMode;
		m_timing = (Timing)st.timing;
		m_interp = (Interp)st.interp;
		m_constSpeed = st.constSpeed;
		m_autoSnap = st.autoSnap;
		advancedfx::Message("[campath] loaded %d markers from sidecar.\n", Count());
	}
}

} // namespace Filmmaker
