#include "BodyCam.h"

#include "FollowCamera.h"
#include "CameraBridge.h"          // release free-cam camera control on stop (see RestoreView)
#include "../Filmmaker.h"          // CameraEditor_Active (editor owns free-cam lifecycle itself)

#include "../../ClientEntitySystem.h" // AfxGetSpectatedPawnIndex
#include "../../MirvTime.h"           // detect whether a demo is actually loaded
#include "../../../shared/AfxConsole.h"

#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {
	constexpr double kChestForwardOffset = 8.0; // local-space units; clears the player's own chest geometry
	constexpr double kFov = 100.0;              // wide, action-cam-ish FOV
	constexpr double kLookSmoothing = 0.06;     // slight handheld lag instead of a perfectly rigid mount
	constexpr double kPositionSmoothing = 0.03;

	// Every FollowCamera preview turns free-cam camera control ON (so its computed pose overrides
	// the game view) but NOTHING in FollowCamera turns it back off -- that is normally the Camera
	// Editor's job when it closes. Body Cam runs from the lightweight Config panel with the editor
	// CLOSED, so when it stops we must restore free-cam ourselves; otherwise camera control stays
	// latched and the OS cursor is captured for mouse-look forever (mouse "stuck to the center",
	// even back at the main menu). Skip while the editor is open -- it manages its own free-cam.
	void RestoreViewAfterBodyCam() {
		if (CameraEditor_Active()) return;
		CameraBridge_SetFreeCamEnabled(false);
		if (g_pEngineToClient) {
			// Back to a normal in-eye spectator view + drop the UI cursor, matching MovieMode's
			// own free-cam exit (spec_mode 2 = in-eye, camtl cursor off).
			g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 2", true);
			g_pEngineToClient->ExecuteClientCmd(0, "mirv_filmmaker camtl cursor off", true);
		}
	}

	// True between a successful BodyCam_Set(true) and the moment Body Cam stops (by either path).
	// Lets BodyCam_RunFrame notice a stop that happened inside FollowCamera (death/demo end) and
	// restore the view once, without re-restoring every idle frame.
	bool s_bodyCamEngaged = false;
}

bool BodyCam_Active() {
	const FollowCameraState& st = FollowCameraRef().State();
	return st.enabled && st.mode == FollowMode::Attach && st.targetType == FollowTargetType::Player
		&& st.attachmentName == "chest";
}

void BodyCam_Set(bool enable) {
	FollowCamera& follow = FollowCameraRef();
	if (!enable) {
		if (BodyCam_Active()) follow.StopPreview("body cam off");
		RestoreViewAfterBodyCam(); // release free-cam even if it was already stopped, so the
		                           // OS cursor / mouse-look never stays latched (see helper).
		s_bodyCamEngaged = false;
		return;
	}

	const int target = AfxGetSpectatedPawnIndex();
	if (target < 0) {
		advancedfx::Warning("[bodycam] refused: no spectated player (switch to a player's POV first).\n");
		return;
	}

	follow.SetTargetType(FollowTargetType::Player);
	follow.SetMode(FollowMode::Attach);
	follow.SetAttachmentName("chest");
	follow.SetOffset(kChestForwardOffset, 0.0, 0.0);
	follow.SetRotationOffset(0.0, 0.0, 0.0);
	follow.SetFov(kFov);
	follow.SetLookSmoothing(kLookSmoothing);
	follow.SetPositionSmoothing(kPositionSmoothing);
	follow.SelectEntity(target);
	follow.Preview();

	if (follow.State().enabled)
		s_bodyCamEngaged = true;
	else
		advancedfx::Warning("[bodycam] failed to engage -- see the [followcam] warning above.\n");
}

void BodyCam_Toggle() { BodyCam_Set(!BodyCam_Active()); }

void BodyCam_RunFrame() {
	// Body Cam stopped from under us (FollowCamera's own death/demo-end/target-lost handling)
	// while we still thought it was engaged -> restore the view once so free-cam control never
	// stays latched. The normal explicit-off path clears the latch itself before we get here.
	if (s_bodyCamEngaged && !BodyCam_Active()) {
		RestoreViewAfterBodyCam();
		s_bodyCamEngaged = false;
	}
}

void BodyCam_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 3) ? args->ArgV(2) : "toggle";
	if (0 == _stricmp(arg, "state")) {
		advancedfx::Message("[bodycam][state] %s\n", BodyCam_Active() ? "on" : "off");
		return;
	}
	if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "1")) BodyCam_Set(true);
	else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "0")) BodyCam_Set(false);
	else BodyCam_Toggle();
	advancedfx::Message("mirv_filmmaker: body cam %s (needs a spectated player).\n", BodyCam_Active() ? "ON" : "off");
}

} // namespace Filmmaker
