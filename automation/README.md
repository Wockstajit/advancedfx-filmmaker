# Automation

Repository automation, verification harnesses, launchers, configs, screenshots, and automation-only test drivers belong in this directory.

- `live-ui-editing.md` — guide: change the Panorama UI live over netcon (no rebuild), read panel state, and screenshot just the game window. **Start here for UI iteration.**
- `launch-cs2-netcon.ps1` consistently launches CS2 windowed at 1920x1080.
- `capture-main-monitor.ps1` captures the complete primary monitor.
- `capture-game-window.ps1` captures only the CS2 window (client area by default) at full resolution — use this for reading UI detail.
- `AutomationCommon.ps1` creates isolated timestamped run directories and metadata.
- `verify-followcam.ps1` runs the live CS2/netcon Follow Camera verification.
- `verify-grenade-tracking.ps1` discovers a nearby grenade, seeks before its throw, and proves reacquisition.
- `verify-editor-viewport-debug.ps1` is a static (no-launch) source check for the camera-editor viewport work: no auto free-cam/jump on open, HUD scales 1:1 with the world blit, and the `editor debug` overlay + render-layer instrumentation are wired end to end.
- `verify-editor-viewport-live.ps1` runs the live HUD scaling check, prints per-HUD pixel bounds/areas, and writes closed-viewer vs camera-editor comparison JSON/CSV.
- `live.bat "<demo.dem>"` launches CS2 plus the browser dashboard and optionally loads the supplied demo path.
- `follow-camera-math-tests.cpp` is the deterministic Follow Camera math test driver built by CMake.

Generated screenshots and logs are written to `automation/runs/<automation>/<timestamp>/`.
Historical screenshots that were already committed are under `legacy-screenshots/`.
