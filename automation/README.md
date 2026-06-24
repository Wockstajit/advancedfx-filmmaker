# Automation

Repository automation, verification harnesses, launchers, configs, screenshots, and automation-only test drivers belong in this directory.

- `launch-cs2-netcon.ps1` consistently launches CS2 windowed at 1920x1080.
- `capture-main-monitor.ps1` captures the complete primary monitor.
- `AutomationCommon.ps1` creates isolated timestamped run directories and metadata.
- `verify-followcam.ps1` runs the live CS2/netcon Follow Camera verification.
- `verify-grenade-tracking.ps1` discovers a nearby grenade, seeks before its throw, and proves reacquisition.
- `follow-camera-math-tests.cpp` is the deterministic Follow Camera math test driver built by CMake.

Generated screenshots and logs are written to `automation/runs/<automation>/<timestamp>/`.
Historical screenshots that were already committed are under `legacy-screenshots/`.
