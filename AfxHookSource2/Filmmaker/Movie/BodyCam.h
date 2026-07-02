#pragma once

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// One-click "chest cam" preset: rides the FollowCamera system's existing Attach mode and
// virtual "chest" attachment point (FollowTargetProviders.cpp) against whichever player is
// CURRENTLY spectated, with a small forward offset (clears the player's own model geometry)
// and a wider FOV. Pure orchestration over FollowCamera's already-public API -- this file
// adds no new camera math and does not modify FollowCamera.{h,cpp}.
//
// Not a separate camera mode: engaging Body Cam just drives the same shared FollowCamera
// state the Camera Editor's own Follow inspector edits, so the two are always in sync (e.g.
// opening the editor while Body Cam is on shows it selected there too, and hand-tuning it
// from the inspector is expected to work normally).
bool BodyCam_Active();
void BodyCam_Set(bool enable);
void BodyCam_Toggle();

// Per-frame safety net (call from the main-thread pump). Body Cam's underlying FollowCamera
// preview can stop on its OWN -- target death, demo end, target disappeared -- without going
// through BodyCam_Set(false). Because Body Cam runs with the Camera Editor CLOSED, nothing
// would then release the latched free-cam camera control, leaving the OS cursor captured. This
// detects Body Cam dropping out from under us and restores the normal view exactly once.
void BodyCam_RunFrame();

// Console command entry: handles "mirv_filmmaker bodycam ...". argc/args/cmd are forwarded
// from FilmmakerCommand.cpp's dispatcher (args->ArgV(0) == "mirv_filmmaker", ArgV(1) == "bodycam").
void BodyCam_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
