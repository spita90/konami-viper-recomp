# Experimental track explorer

Optional drone tour and free-roam cameras for **GTI Club 2 JAB (Japanese)** in enhanced mode. Other game profiles have no explorer hooks.

Build with `python3 recomp/recomp.py gticlub2` then `make GAME=gticlub2 -j8`.
Start a game and choose a course, then press **F6** to toggle the drone tour. You can also press F6 before the race starts to arm it.

- **Accelerator / brake** (A / B, triggers, or Up / Down): hold to increase/decrease cruise speed; release to keep it steady.
- **Left / right shoulder** (L / R, or Q / E): lower/raise the drone; release to hold height. Steering and gyro do not change drone height.
- **Steering stick / gyro** (or Left / Right): look up to 90 degrees left/right of the route heading. Centre the input to look forward again.
- **Gyro tilt up / down**: look up to 45 degrees above/below the normal viewing angle. L3 recentres both gyro axes; R3 enables/disables gyro as usual.
- **[ / ]**: decrease/increase speed, from stationary to 160 metres/second (default 40).
- **− / =**: lower/raise the drone, 3–60 metres above the road (default 12).
- **F6** again: return to the normal camera.
- **Escape / Home**: the existing pause menu, including return to the main menu.

The overlay displays live cruise speed in km/h and selected height above the road in metres.

## Free roam

Press **F7** to enter free roam from the normal camera or the F6 tour. It starts
at the current camera position and orientation, with no route or terrain following.
Press **F7** again to return to the normal camera, or **F6** to switch to the drone
tour. Either mode key switches directly to that mode; pressing its own key again exits.

- **Accelerator / brake**: move forward/backward along the viewing direction;
  release to stop. Triggers retain proportional speed control.
- **Steering stick / gyro / Left / Right**: turn left/right. Releasing holds
  the new heading instead of returning to the track heading.
- **Gyro tilt up / down**: pitch the camera up/down; centre to hold the angle.
- **Shift up / down shoulders (E / Q)**: move vertically up/down, independent
  of viewing direction; release to hold altitude.
- **[ / ]**: adjust maximum movement speed. The overlay shows this speed and
  world altitude, rather than clearance above the road.

The existing gyro enable/recentre controls and pause menu still apply. Free roam
shares the tour's race-clock suspension and cancellation on returning to the menu.
It has no collision or automatic height adjustment, so it can pass through scenery
or below the ground. Like the tour, this is available only in enhanced GTI Club 2 JAB.
For scripted checks, `RT_ENH_MENU` accepts `seconds:free` to toggle free roam.

The drone follows the selected course's linked road centreline, looks ahead through turns, follows terrain elevation, and loops continuously. Race-state transitions are held during exploration; the race clock's origin advances to exclude time spent exploring. Returning to the main menu cancels an active tour. Settings are not persisted.

This is a camera experiment, not an editor: the game continues simulating traffic and cars, its normal HUD remains visible, and the drone does not collide with buildings, bridges or tunnels. Routes with overhead scenery can clip at high altitudes. Only the JAB Town route has been visually checked; the other selected-course routes use the same path reader but need playtesting.

## Implementation notes

`runtime/track_explorer.c` contains the experiment. Its two hooks are installed only by `games/gticlub2/game.json`:

- `0x8ec84`, before the race renderer applies the camera: read route root `0x8c0188`, follow 0x44-byte linked nodes and their 32-byte path points, and write the camera at `0x8c1cf8`.
- `0xb403c`, after world rendering: override the local race-state register with an out-of-range state, using the game's existing no-transition exit without changing its stored state.

Ground height uses the game's tile lookup (`0x5576c`) and collision-plane interpolation (`0x566f0`) on a copied PPC register context. The temporary guest stack is saved and restored, and the live CPU budget is untouched. No ROM-derived data is committed.

For headless checks, `RT_TRACK_EXPLORER=1` arms the tour. `RT_ENH_MENU` accepts `seconds:drone` to toggle it. Use disposable settings/NVRAM copies. A full lap logs `completed full course loop`; normal frame dumps include the control hint.

Earlier drone validation on the development branch: rebuilt JAB, ran a 210-second headless session, completed the 4,992.1 m Town loop, toggled the drone off at 195 seconds, and inspected the restored car camera and running race timer. Inspected intermediate terrain-following frames as well. `git diff --check` passed. The build retains the pre-existing `cpu.c` unused `nmatch` warning.

The ROM-free camera regression test covers forward/reverse movement, yaw and pitch,
independent height, both mode transitions, exit, and cancellation:

```sh
cc -std=c11 -Iruntime tests/test_track_explorer.c -lm -o /tmp/test_track_explorer
/tmp/test_track_explorer
```
