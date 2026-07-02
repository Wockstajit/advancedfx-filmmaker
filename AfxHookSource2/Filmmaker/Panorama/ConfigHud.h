#pragma once

// ConfigHud: a lightweight CONFIG panel rendered with CS2's native Panorama in the in-game
// HUD (CSGOHud) context -- the Camera Editor's little sibling.
//
// It reuses the editor's inspector look (see ConfigHudJs.h) but carries ONLY general
// UI / game-display configuration:
//   * the shared "Game UI" visibility picker (same HudView state the Camera Editor uses;
//     the actual show/hide stays centralised in CameraTimelineHud's JS, which honours the
//     picker while EITHER the editor is hosted OR this panel is open);
//   * a "Show UI Defaults" reset;
//   * the native demo-playback display toggles (X-Ray / True View / DOA / mismatch).
//
// Unlike the Camera Editor it does NOT host the camera timeline, does NOT hide the native
// demo bar (the regular game timeline stays visible), and has no camera tools at all.
// Mutually exclusive with the Camera Editor: opening one closes the other.
//
// Mirrors the proven MovieHud / CameraEditorHud bridge pattern: a PanoramaBridge pinned to
// the in-game HUD panel, a panel built ONCE from embedded JS, a small state JSON per frame.
//
//   C++ -> JS : attribute "state" ({enabled, cursor, hudView}), then $.ConfigHud.render().
//   JS  -> C++: buttons issue "mirv_filmmaker ..." console commands.

#include "PanoramaBridge.h"

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

class ConfigHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame.
	void RunFrame();

	void SetEnabled(bool v) { m_enabled = v; }
	void Toggle() { m_enabled = !m_enabled; }
	bool Enabled() const { return m_enabled; }

	std::string DebugStateJson() { return BuildStateJson(); }

	// Scaled-preview rect (normalised window fractions) + whether it is active this frame.
	// Read by CameraTimelineHud so the JS squishes the native game HUD into the same rect
	// the D3D world blit uses (identical contract to CameraEditorHud's version).
	bool ScaledHud(float& x0, float& y0, float& x1, float& y1) const {
		if (!(m_enabled && m_previewValid)) return false;
		x0 = m_previewX0; y0 = m_previewY0; x1 = m_previewX1; y1 = m_previewY1;
		return true;
	}

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	std::string BuildStateJson();
	void OnEnter(); // one-shot: turn the UI cursor on so the panel is immediately clickable
	void OnExit();  // one-shot: tear down the chrome (HudView is shared state; not reset here)
	void UpdateScaleRequest(); // forward the JS-published "previewrect" to the D3D viewport scaler

	PanoramaBridge m_bridge;
	void* m_hudPanel = nullptr; // HUD context we built against (rebuild if it changes)
	void* m_root = nullptr;     // #ConfigHudRoot
	short m_symState = -1;
	short m_symPreviewRect = -1;
	// Last preview rect parsed from the JS-published "previewrect" (normalised window fractions).
	bool m_previewValid = false;
	float m_previewX0 = 0, m_previewY0 = 0, m_previewX1 = 0, m_previewY1 = 0;
	bool m_built = false;
	bool m_enabled = false;
	bool m_wasEnabled = false;  // for enter/exit edge detection
	bool m_prevMovieHud = false; // movie-director card visibility to restore on exit
	std::string m_lastState;
};

ConfigHud& ConfigHudRef();

// True while the Config panel is open. Free-function form so CameraTimelineHud can publish
// a "configOpen" state field without pulling in this header's class (same pattern as
// CameraEditor_HudViewName in Filmmaker.cpp).
bool ConfigHud_Enabled();

// Scaled-preview rect + active flag while the Config panel is open (free-function form for
// CameraTimelineHud, mirroring CameraEditor_ScaledHud).
bool ConfigHud_ScaledHud(float& x0, float& y0, float& x1, float& y1);

// Console command entry: handles "mirv_filmmaker config ...". argc/args/cmd are forwarded
// from FilmmakerCommand.cpp's dispatcher (args->ArgV(0) == "mirv_filmmaker", ArgV(1) == "config").
void ConfigHud_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
