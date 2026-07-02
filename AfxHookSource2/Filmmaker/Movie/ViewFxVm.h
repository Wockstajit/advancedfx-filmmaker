#pragma once

#include <cstddef>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Experimental TRUE viewmodel ROTATION -- the ARC9 "RotateAroundAxis on the viewmodel"
// equivalent that the offsets-only viewmodel hook (ViewModel.cpp) cannot do. CS2 computes the
// viewmodel pose in its CalcViewModelView equivalent and writes it into the vm entity's
// CGameSceneNode; we rotate that node AFTER the engine's write. Because it is not known a
// priori WHICH write site survives to the rendered frame (what runs after what inside CS2's
// frame), this module exposes several candidate SITES and FIELDS, selected at runtime via
// "mirv_filmmaker viewfx vmtest ..." so the working combination can be found empirically in a
// live demo (paused-demo screenshot A/B). Once confirmed, the deadzone free-aim uses it.
//
// Sites:
//   0 = off
//   1 = inside ViewModel.cpp's setViewmodel hook, right after the original returns (we are
//       INSIDE the engine's vm-calc; cheap, but the caller may overwrite the node afterwards)
//   2 = main-thread FrameStageNotify pump (Filmmaker::RunMainThreadFrame)
//   3 = render-thread present pump (Filmmaker::RunFrame)
//   4 = detour of the CALLER of the setViewmodel helper (CS2's CalcViewModelView itself),
//       post-original -- the strongest timing bet; installed lazily on first selection.
// Fields:
//   1 = CGameSceneNode::m_angRotation (local QAngle, schema-resolved)
//   2 = CGameSceneNode::m_nodeToWorld orientation quaternion (offset auto-detected by norm)
//   3 = both

// Site-1 tap: called by ViewModel.cpp's setViewmodel hook after the original returns.
void ViewFxVm_OnViewmodelCalc();

// Per-frame pumps for sites 2/3.
void ViewFxVm_MainPump();
void ViewFxVm_RenderPump();

// ViewModel.cpp shares the resolved helper address here so site 4 can locate its caller.
void ViewFxVm_NoteViewmodelHelper(size_t helperAddr);

// Deadzone integration: ViewFx pushes the current free-aim (degrees, where the TRUE aim sits
// relative to the lagged camera) every tracker step. Applied as a real rotation when a
// working site/field is configured AND mode is "deadzone".
void ViewFxVm_SetFreeAim(double pitchDeg, double yawDeg);

// True while vm rotation is engaged for the deadzone -- lets ViewFx suppress the fake
// translation hint (rotation replaces it).
bool ViewFxVm_RotationActive();

// Console: handles "mirv_filmmaker viewfx vmtest ..." (argc/args offset like ViewFx_RunCommand;
// args->ArgV(2) == "vmtest").
void ViewFxVm_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
