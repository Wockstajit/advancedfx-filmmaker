#pragma once

// Thin wrapper around CS2's Panorama UI engine for the filmmaker menu.
//
// Reuses addresses already resolved in DeathMsg.cpp:
//   - the UI engine pointer,
//   - get/setAttributeString + makeSymbol,
//   - CUIEngine::RunScript (function resolved from panorama.dll), and
//   - the in-game HUD panel (used as the RunScript context to create the UI).
//
// All calls are null-guarded: if RunScript did not resolve, every method is a
// safe no-op and the console-driven demo browser keeps working.

#include <cstdint>
#include <string>

namespace Filmmaker {

class PanoramaBridge {
public:
	// Grabs the engine + RunScript pointers and (in auto mode) the HUD context.
	void Init();

	bool HasEngine() const { return m_engine != nullptr && *m_engine != nullptr; }
	bool HasRunScript() const { return m_runScript != nullptr; }
	bool CanRunScript() const { return HasEngine() && HasRunScript(); }

	// Pins the context panel that scripts run inside (disables HUD auto-context).
	void SetContextPanel(void* uiPanel) { m_contextPanel = uiPanel; m_autoContext = false; }
	void* ContextPanel() const { return m_contextPanel; }

	// Runs Panorama JavaScript in the context panel. No-op unless CanRunScript()
	// and a context panel are set.
	bool RunScript(const std::string& source);

	// Attribute IO on a CUIPanel* (reuses the resolved get/setAttributeString).
	std::string GetAttributeString(void* uiPanel, short symbol, const char* defaultValue = "") const;
	void SetAttributeString(void* uiPanel, short symbol, const char* value) const;

	// Creates / interns a Panorama symbol by name (reuses the resolved makeSymbol).
	short MakeSymbol(const char* name) const;

private:
	unsigned char** m_engine = nullptr;     // AfxHookSource2_GetPanoramaUiEngine()
	void* m_runScript = nullptr;            // AfxHookSource2_GetPanoramaRunScript()
	void* m_contextPanel = nullptr;
	bool m_autoContext = true;              // when set, follow the HUD panel
	uint64_t m_line = 0;                    // non-zero -> RunScript skips caching
};

} // namespace Filmmaker
