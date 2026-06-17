#include "PanoramaBridge.h"

#include "../../DeathMsg.h" // CS2::PanoramaUIEngine/UIPanel offsets + engine/runscript accessors

namespace Filmmaker {

void PanoramaBridge::Init() {
	if (!m_engine)
		m_engine = AfxHookSource2_GetPanoramaUiEngine();
	if (!m_runScript)
		m_runScript = AfxHookSource2_GetPanoramaRunScript();
	// In auto mode, prefer the main-menu panel as the build context: it is the
	// window that owns the top navbar, so the "Demos" button can attach there.
	// Fall back to the in-game HUD panel (valid while a map/demo is loaded) so
	// the page can still be built/opened when no main menu exists.
	if (m_autoContext) {
		if (unsigned char* mainMenu = AfxHookSource2_GetPanoramaMainMenuPanel())
			m_contextPanel = mainMenu;
		else if (unsigned char* hud = AfxHookSource2_GetPanoramaHudPanel())
			m_contextPanel = hud;
	}
}

short PanoramaBridge::MakeSymbol(const char* name) const {
	if (!m_engine || !*m_engine || CS2::PanoramaUIEngine::makeSymbol == 0)
		return -1;
	typedef short(__fastcall* makeSymbol_t)(unsigned char* engine, int type, const char* name);
	const auto makeSymbol = *(makeSymbol_t*)((*(unsigned char**)(*m_engine)) + CS2::PanoramaUIEngine::makeSymbol);
	return makeSymbol(*m_engine, 0, name);
}

std::string PanoramaBridge::GetAttributeString(void* uiPanel, short symbol, const char* defaultValue) const {
	if (!uiPanel || CS2::PanoramaUIPanel::getAttributeString == 0)
		return defaultValue ? defaultValue : "";
	typedef const char* (__fastcall* getAttributeString_t)(void* panel, short attr, const char* def);
	const auto fn = *(getAttributeString_t*)(*(unsigned char**)uiPanel + CS2::PanoramaUIPanel::getAttributeString);
	const char* v = fn(uiPanel, symbol, defaultValue ? defaultValue : "");
	return v ? std::string(v) : std::string();
}

void PanoramaBridge::SetAttributeString(void* uiPanel, short symbol, const char* value) const {
	if (!uiPanel || CS2::PanoramaUIPanel::setAttributeString == 0)
		return;
	typedef void* (__fastcall* setAttributeString_t)(void* panel, short attr, const char* value);
	const auto fn = *(setAttributeString_t*)(*(unsigned char**)uiPanel + CS2::PanoramaUIPanel::setAttributeString);
	fn(uiPanel, symbol, value ? value : "");
}

bool PanoramaBridge::RunScript(const std::string& source) {
	if (!CanRunScript() || !m_contextPanel)
		return false;

	// void RunScript(CUIEngine* this, CUIPanel* ctx, const char* src, const char* originFile, uint64 line)
	typedef void(__fastcall* runScript_t)(void* engine, void* ctx, const char* src, const char* originFile, uint64_t line);
	const auto fn = (runScript_t)m_runScript;
	fn(*m_engine, m_contextPanel, source.c_str(), "filmmaker", ++m_line);
	return true;
}

} // namespace Filmmaker
