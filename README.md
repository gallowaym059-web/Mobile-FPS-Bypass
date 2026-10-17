# Mobile FPS Bypass (Android32 + Android64)

Geode mod for Geometry Dash 2.2081 on Android.

## What it does
- Sets the cocos2d animation interval to 1/FPS. On Android this feeds the Java renderer's frame limiter, which is the real 60 FPS cap.
- "Disable V-Sync" calls `eglSwapInterval(0)` on the GL thread, the Android equivalent of the PC DLL's V-Sync toggle.
- Re-applies both every frame if the game or another mod changes them back.
- Draggable launcher bubble (tap = open/close, shows live FPS) and a draggable panel with smooth, frame-rate independent motion.
- Panel: enable toggle, +/- (hold to accelerate), presets 30/60/90/120/144/240/360/480/560/640/720/890/1000 (range 10-1000), V-Sync toggle, hide-button-in-level toggle, measured FPS readout.

## Build
1. Push this folder to a GitHub repo.
2. Open the Actions tab, run "Build Android32 + Android64" (or just push).
3. Download "Build Output" and install the `.geode` file.

## Notes
- Your screen still only shows frames at its refresh rate. On a 60Hz panel the bypass will mostly show up as a higher "Measured" number.
- Physics in GD 2.2 is time-based, so higher FPS does not change gameplay speed.