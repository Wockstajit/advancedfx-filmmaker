#include "DemoBarButtons.h"

#include "DemoBarButtonsJs.h"

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel + PanoramaUIPanel offsets

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// Recursive id search (same approach as MovieHud's FindChildById), bounded by
// depth. Kept local so this module stays decoupled from MovieHud.
void* FindChildById(void* panel, const char* id, int depth = 0) {
	if (!panel || depth > 64)
		return nullptr;
	unsigned char* childrenField = (unsigned char*)panel + CS2::PanoramaUIPanel::children;
	const int count = *(int*)childrenField;
	void** arr = *(void***)(childrenField + 8);
	if (!arr || count <= 0 || count > 100000)
		return nullptr;
	for (int i = 0; i < count; ++i) {
		void* child = arr[i];
		if (!child) continue;
		char* cid = *(char**)((unsigned char*)child + CS2::PanoramaUIPanel::panelId);
		if (cid && 0 == std::strcmp(cid, id))
			return child;
	}
	for (int i = 0; i < count; ++i) {
		void* child = arr[i];
		if (!child) continue;
		if (void* found = FindChildById(child, id, depth + 1))
			return found;
	}
	return nullptr;
}

} // namespace

DemoBarButtons& DemoBarButtonsRef() {
	static DemoBarButtons s_instance;
	return s_instance;
}

bool DemoBarButtons::Inject() {
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kDemoBarButtonsJs))
		return false;
	// Confirm our row actually got created. If the native panel structure
	// changed and the JS couldn't find #SpeedControls, this stays false and we
	// retry next frame (native dropdown remains usable in the meantime).
	return FindChildById(m_bridge.ContextPanel(), "MirvSpeedRow") != nullptr;
}

void DemoBarButtons::RestoreNative() {
	// Delete our button row and un-hide the native dropdown. Only run while the
	// context panel is valid (a demo is playing); the caller guarantees that.
	if (m_injected && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var c=$.GetContextPanel();"
			"var r=c.FindChildTraverse('MirvSpeedRow'); if(r) r.DeleteAsync(0);"
			"var t=c.FindChildTraverse('TimeScale'); if(t) t.visible=true;})();");
	}
	m_injected = false;
	m_anchor = nullptr;
}

void DemoBarButtons::RunFrame() {
	m_bridge.Init(); // resolve engine + RunScript (idempotent)

	bool playingDemo = false;
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			playingDemo = pDemo->IsPlayingDemo();
	}
	if (!playingDemo) {
		// Demo gone -> the native bar is destroyed with the HUD; just drop state
		// (no script: the context panel may already be invalid).
		m_injected = false;
		m_anchor = nullptr;
		m_hudPanel = nullptr;
		return;
	}

	unsigned char* hud = AfxHookSource2_GetPanoramaHudPanel();
	if (!hud) {
		m_injected = false;
		m_anchor = nullptr;
		m_hudPanel = nullptr;
		return;
	}
	if (hud != m_hudPanel) { // HUD (re)created -> reinject against the new context
		m_hudPanel = hud;
		m_injected = false;
		m_anchor = nullptr;
	}
	m_bridge.SetContextPanel(hud);

	if (!m_enabled) { // toggled off while a demo plays -> restore native dropdown
		RestoreNative();
		return;
	}

	// Locate the native speed-controls container. Until the demo bar is built,
	// do nothing -> the native dropdown stays fully functional (graceful fallback).
	void* anchor = FindChildById(hud, "SpeedControls");
	if (!anchor) {
		m_injected = false;
		m_anchor = nullptr;
		return;
	}
	if (anchor != m_anchor) { // demo bar (re)built -> (re)inject
		m_anchor = anchor;
		m_injected = false;
	}

	if (!m_injected)
		m_injected = Inject();
}

} // namespace Filmmaker
