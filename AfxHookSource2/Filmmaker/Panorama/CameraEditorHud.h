#pragma once

// CameraEditorHud: a dedicated "Camera Editor Mode" workspace shell rendered with
// CS2's native Panorama in the in-game HUD (CSGOHud) context.
//
// This is a PRESENTATION SHELL over the existing camera-editing back-end; it adds no
// new editing logic. While enabled it:
//   * frames the live game as a "preview" in the top-left (a right-side property
//     inspector + a bottom timeline + a dimmed letterbox make the open area read as a
//     viewport -- note the preview is a CROP of the full-screen frame, not a scaled
//     copy, since Panorama only composites overlays on top of the full game render);
//   * docks a BOTTOM panel under the preview whose mode is selectable (BottomMode):
//       - CameraTimeline (default): the custom camera timeline (CameraTimelineHud) overlay;
//       - Graph: the experimental graph curve editor overlay;
//       - Native: kept only for the `editor curveeditor native` console command -- shows no
//         overlay (just the bottom tab bar). The native CS2 demo bar is HIDDEN the whole time
//         the editor is open, so it can never bleed through behind the overlays.
//     A persistent BOTTOM tab bar (drawn by CameraEditorJs where CS2's old CAM EDITOR / MOUSE
//     buttons sat) flips between Camera Timeline and Graph; all are driven by the same
//     "mirv_filmmaker editor curveeditor ..." console commands;
//   * surfaces selected-camera settings (FOV, roll, interpolation, segment speed,
//     easing, freeze/live, speed mode) as Slider / stepper / cycle controls that issue
//     those same commands;
//   * hides the rest of the gameplay HUD (radar/health/ammo/scoreboard) AND the native demo
//     bar for a clean workspace -- the actual hide is centralised in CameraTimelineHud's JS
//     via the "hosted" state flag, so a single script owns native-HUD visibility.
//
// Mirrors the proven MovieHud / CameraTimelineHud bridge pattern: a PanoramaBridge
// pinned to the in-game HUD panel, a panel built ONCE from embedded JS, and a small
// state JSON pushed each frame.
//
//   C++ -> JS : attribute "state" (camera readouts + selected-key settings), then
//               $.CamEditor.render().
//   JS  -> C++: buttons / sliders issue "mirv_filmmaker ..." console commands.

#include "PanoramaBridge.h"

#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

class CameraEditorHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame. Runs LAST so
	// its host orchestration (timeline hosting + HUD hide) wins over the other panels.
	void RunFrame();

	void SetEnabled(bool v) { m_enabled = v; }
	void Toggle() { m_enabled = !m_enabled; }
	bool Enabled() const { return m_enabled; }

	// TRUE scaled preview viewport (render-layer): when on, the whole rendered frame is
	// scaled down into the preview rect via CViewportScaler instead of showing a crop.
	// Only takes effect while the editor is open and not recording. Safe to toggle anytime.
	void SetScale(bool v) { m_scaleEnabled = v; }
	void ToggleScale() { m_scaleEnabled = !m_scaleEnabled; }
	bool ScaleEnabled() const { return m_scaleEnabled; }

	enum class BottomMode { Native = 0, CameraTimeline = 1, Graph = 2 };

	// Bottom editor selection. Native = CS2's own demo timeline stays visible; CameraTimeline
	// and Graph are explicit overlays opened by buttons.
	void SetBottomMode(BottomMode mode) { m_bottomMode = mode; }
	void SetUseTimeline(bool v) { m_bottomMode = v ? BottomMode::CameraTimeline : BottomMode::Graph; }
	void ToggleUseTimeline() {
		m_bottomMode = (m_bottomMode == BottomMode::CameraTimeline) ? BottomMode::Graph : BottomMode::CameraTimeline;
	}
	bool UseTimeline() const { return m_bottomMode == BottomMode::CameraTimeline; }
	BottomMode GetBottomMode() const { return m_bottomMode; }

	// Game-HUD visibility while the editor is open. HideAll = clean workspace (the original
	// auto-hide, default); InGame = keep radar + HP/ammo but strip the spectator observer
	// panel (player avatar/name bar); ShowAll = full spectator HUD. The actual show/hide lives
	// in CameraTimelineHud's JS, which reads this through the "hudView" state field.
	enum class HudView { HideAll = 0, InGame = 1, ShowAll = 2 };
	void SetHudView(HudView v) { m_hudView = v; }
	void CycleHudView() { m_hudView = (HudView)(((int)m_hudView + 1) % 3); }
	HudView GetHudView() const { return m_hudView; }

	// Viewport/HUD debug overlay: on-screen readout of window/render-target/viewport geometry so
	// the custom editor viewport can be compared 1:1 against the normal game viewport.
	void SetDebugOverlay(bool v) { m_debugOverlay = v; }
	void ToggleDebugOverlay() { m_debugOverlay = !m_debugOverlay; }
	bool DebugOverlay() const { return m_debugOverlay; }

	// The scaled-preview rect (normalised window fractions, x0 y0 x1 y1) the world blit uses,
	// plus whether the scaled viewport is live. The timeline HUD reads these to scale the native
	// game HUD into the SAME rect so the HUD lines up with the shrunk world preview.
	bool ScaledHudActive() const { return m_scaleEnabled && m_previewValid; }
	bool PreviewRect(float& x0, float& y0, float& x1, float& y1) const {
		x0 = m_previewX0; y0 = m_previewY0; x1 = m_previewX1; y1 = m_previewY1; return m_previewValid;
	}

	std::string DebugStateJson() { return BuildStateJson(); }

	void RequestEval(const std::string& js) { m_evalQueue.push_back(js); }

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	std::string BuildStateJson();
	void OnEnter(); // one-shot: host the timeline, hide MovieHud (no free-cam / no jump on open)
	void OnExit();  // one-shot: un-host the timeline, restore MovieHud, stop scrub
	void UpdateScaleRequest(); // publish the preview rect to the render-layer viewport scaler

	PanoramaBridge m_bridge;
	void* m_hudPanel = nullptr; // HUD context we built against (rebuild if it changes)
	void* m_root = nullptr;     // #CamEditorRoot
	short m_symState = -1;
	short m_symPreviewRect = -1; // "previewrect" -- JS publishes the preview rect fractions
	short m_symDebugPanels = -1; // "debugpanels" -- JS publishes measured HUD/editor rects
	bool m_built = false;
	bool m_enabled = false;     // Camera Editor Mode on/off
	bool m_scaleEnabled = false; // true scaled-preview viewport (render-layer blit)
	BottomMode m_bottomMode = BottomMode::Native;
	HudView m_hudView = HudView::HideAll; // game-HUD visibility while the editor is open
	bool m_debugOverlay = false;         // viewport/HUD debug readout overlay
	// Last preview rect parsed from the JS-published "previewrect" (normalised window fractions).
	float m_previewX0 = 0, m_previewY0 = 0, m_previewX1 = 0, m_previewY1 = 0;
	bool m_previewValid = false;
	bool m_wasEnabled = false;  // for enter/exit edge detection
	bool m_prevMovieHud = false; // MovieHud visibility to restore on exit
	std::string m_lastState;
	std::vector<std::string> m_evalQueue;
};

CameraEditorHud& CameraEditorHudRef();

// Console command entry: handles "mirv_filmmaker editor ...". argc/args/cmd are forwarded
// from FilmmakerCommand.cpp's dispatcher (args->ArgV(0) == "mirv_filmmaker", ArgV(1) == "editor").
void CameraEditor_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
