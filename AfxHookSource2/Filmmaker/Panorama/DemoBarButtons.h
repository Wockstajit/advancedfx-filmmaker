#pragma once

// DemoBarButtons: replaces CS2's native demo-bar timescale DROPDOWN (#TimeScale,
// which opens a popup menu) with a row of inline speed buttons (0.1x .. 4x) in
// the same place (#SpeedControls). Each button runs `demo_timescale <x>`.
//
// It injects into the in-game HUD Panorama context (same PanoramaBridge +
// RunScript mechanism as MovieHud) and only touches the native bar while a demo
// plays. The native #TimeScale is HIDDEN (not deleted) and restored on teardown,
// so disabling this (`mirv_filmmaker speedbar off`) always brings the native
// dropdown back. If the native #SpeedControls can't be found, it does nothing —
// the native dropdown stays fully functional (graceful fallback).

#include "PanoramaBridge.h"

namespace Filmmaker {

class DemoBarButtons {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame.
	void RunFrame();

	void SetEnabled(bool e) { m_enabled = e; }
	void Toggle() { m_enabled = !m_enabled; }
	bool Enabled() const { return m_enabled; }

private:
	bool Inject();        // find #SpeedControls, hide #TimeScale, add buttons
	void RestoreNative(); // delete our row + un-hide #TimeScale (context must be valid)

	PanoramaBridge m_bridge;
	void* m_hudPanel = nullptr; // HUD context we injected against
	void* m_anchor = nullptr;   // native #SpeedControls (rebuild detection)
	bool m_injected = false;
	bool m_enabled = true;
};

DemoBarButtons& DemoBarButtonsRef();

} // namespace Filmmaker
