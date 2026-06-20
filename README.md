# OculusOverlayLayer

A minimal **OpenXR API layer** that draws an always-visible info panel
(clock, NVIDIA GPU usage/temp, and live FPS) inside any OpenXR VR app —
tested with DCS World and iRacing on Meta Quest 3 via Meta Link (Windows 11).

The panel is a transparent quad composition layer, world- or head-locked,
and toggled with a configurable hotkey. All look/position settings live in
`overlay.ini` (no rebuild needed).

## Build
1. Open `OculusOverlayLayer.sln` in Visual Studio 2022 (Desktop C++ workload).
2. Configuration: **Debug** (or Release), Platform: **x64**.
3. Project Properties → C/C++ → Code Generation → Runtime Library:
   **Multi-threaded Debug (/MTd)** for Debug, **Multi-threaded (/MT)** for Release.
   (Static CRT — required so the DLL loads inside other processes.)
4. Build. Output: `x64\Debug\OculusOverlayLayer.dll`.

## Install
1. Copy `OculusOverlayLayer.dll` into the `dist\` folder (next to the JSON).
2. Run `dist\install.ps1` (registers the implicit OpenXR layer for the current user).
3. Edit `dist\overlay.ini` to taste.
4. In the Meta PC app, set Meta as the active OpenXR runtime, then launch your sim in VR.

To remove: run `dist\uninstall.ps1`.

## Config (overlay.ini)
Position, size, colors, opacity, world/head lock, redraw rate, and the
show/hide `Hotkey` (e.g. `Ctrl+Alt+F2`). See the file for all keys.