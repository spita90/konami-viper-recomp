# Changelog

All notable changes to this project are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). While the version is below 1.0.0, a new minor
version can change the build, the profiles or the command-line options.

## [Unreleased]

### Added

- Steering wheels on Windows (`runtime/wheel.c`, DirectInput): every USB game device attached
  is found by itself (wheel bases, pedals, shifters, handbrakes, button boxes) and remembered
  by its USB ids and name. The games' own force feedback (the cabinet's K-type motor command)
  plays as a constant force on the wheel's motor; a base that is two devices of one name
  (Fanatec) is handled, the motor being looked for among them.
- INPUT MAPPING (OPTIONS → CONTROLS and the pause menu): every action can be bound, with up to
  four bindings each from any keys and devices, kept in `<executable>_controls.ini` for both
  modes. CONTROLS also has the wheel's force feedback strength, device and direction (with a
  detection), a test, the wheel's range and dead zone.

### Changed

- The keyboard is read through the bindings each frame instead of key events; the default keys
  are the same.

## [0.22.0] - 2026-11-24

### Added

- iOS app (iPhone and iPad), contributed by @vkedwardli in pull request #26: thank you!
  `make ios GAME=<id>` stages the game data and generates an Xcode project (`ios/`). It has the
  GPU renderer on OpenGL ES 3.0, and on-screen touch controls: a steering stick, pedals, shifting,
  VIEW and pause in a race, tapping the menu items, and the name entry. It also has tilt steering
  (OPTIONS > CONTROLS > STEERING), pauses and saves in the background, and keeps its saves in
  Documents. See [ios/README.md](ios/README.md).
- Android app: `python3 android/prepare.py <id> --install` builds the APK on a Mac or Linux with
  the Android SDK and installs it on the phone connected (`android/`). The same touch controls,
  menus and options as the iOS app, the GPU renderer on OpenGL ES 3.0, tilt steering from the
  accelerometer. See [android/README.md](android/README.md).
- Link play on the phones: the apps link as the desktop ports do, over the Internet with a session
  code, together with the desktop ports. The JOIN screen is a list to tap there: the code (a tap
  brings up the keyboard), PASTE, JOIN. The iOS app also races with the iPhones and iPads next to
  it with no router, over a shared Wi-Fi network or directly, phone to phone (Network.framework,
  peer to peer): HOST A GAME also shows the game's tag for the phones nearby, and JOIN A GAME lists
  the games nearby.
- iOS app, link play: the touch controls' ✕ (a gamepad's Back) in a linked race asks LEAVE / CLOSE
  THE SESSION in a small panel while the race goes on. STAY is the default, it goes away by itself
  after 5 seconds, and confirming goes back to the main menu. A player who drops out of a game
  is shown as PLAYER N DISCONNECTED to the others.
- The apps' icons, one per game: `docs/images/icon-thrild2.png`, `docs/images/icon-gticlub2.png`.
- `python3 ios/prepare.py <id> --build ipa`: the iOS app as an unsigned IPA, for a sideloading
  tool that signs it with your Apple ID.
- `make P2P=1` (macOS, tests): the iOS app's link to the phones nearby in a desktop build
  (`<binary>_p2p`), beside its Internet link play, to try it on a Mac.
- `--calibrate FILE`: the first-run calibration alone, into that NVRAM file (the phone apps').
- **Enhanced mode, DISPLAY → FILTERING** (ORIGINAL / DISABLED), back from 0.9.3 to 0.19.x as
  TEXTURE FILTER, on request (issue #27): DISABLED turns off the filtering of enlarged textures,
  for hard texel edges, and scales the picture to the window without smoothing, with both
  renderers and in the apps. The filtering of distant textures and the mipmaps stay the game's.
  Applied at once and saved in the port settings (`filtering`). ORIGINAL, the default, and the
  classic mode give the same frames as before. The software renderer keeps its specialised
  rasterizers with DISABLED too (about 300 KB more code).
- **Option pages that scroll:** a page with more rows than the screen holds (GTI Club 2's
  DISPLAY page with FILTERING, its font being larger) shows those that fit, following the cursor,
  with an arrow above or below where there are more; BACK stays below them.

### Fixed

- `tools/gpu_replay.cpp` links again with the GPU renderer (the frame statistics of 0.21.0).

Apart from FILTERING, the desktop builds do not change: what the apps add is behind defines only
their builds set.

## [0.21.0] - 2026-11-22

### Added

- **Enhanced mode, resolution up to 6X** (issue #22): DISPLAY → RESOLUTION offers 5X and 6X again
  with the HARDWARE renderer, for 4K screens (6X renders 2304 lines, above 4K's 2160) on fast
  GPUs: on a Radeon RX 9070 XT, Thrill Drive 2 at 21:9 takes 1.1 ms of GPU work per vsync at 6X,
  0.6 ms at 4X, with the same CPU time. AUTO still stops at 3X, so 4X to 6X come only when
  chosen. With 60 or 120 FPS each picture in between is drawn at that scale too: on a screen
  faster than 120 Hz, 6X can then miss vsyncs even on a fast GPU (on that Radeon at 180 Hz, one
  in four or five; 5X misses none), so check the fps counter.
- **Sponsor button on the repository** (`.github/FUNDING.yml`) and a "Support the project"
  section in the README, as the SUPPORT page of the enhanced mode.

### Changed

- **Enhanced mode, frames in between: the frame drawn ahead waits for the swap.** Each vsync
  draws the next frame in between ahead of its time; it was queued to the GPU before the swap,
  so the swap waited for it too. It is now queued after: 5X at 120 FPS on a 180 Hz screen keeps
  every vsync (it missed 3–6 a second), and 6X misses fewer.
- **`RT_FPS_STATS=1` says where the time goes:** a second line each second gives the swaps (how
  long, how far apart they return), how long the game's thread waited for the lists, how far it
  fell behind real time, the CPU time of the frames in between, and the audio frames missing.
  `RT_GL_FINISH=1` waits for the GPU after each swap, to compare (it made no difference on
  Windows).

### Fixed

- **Enhanced mode, frames in between on 60 Hz screens** (issue #20, Windows on a ROG Ally and on
  a 60 Hz desktop): about 36 pictures a second were shown instead of 60, one or two vsyncs apart.
  The frontend takes the game's lists once a vsync, and a picture due at the vsync right after its
  game frame came too late for it. The pictures now run one vsync later (17 ms at 60 Hz, 6 ms at
  180 Hz) and come on every vsync they are meant for: 60 at 60 Hz, and on faster screens evenly
  spaced too (120 FPS at 180 Hz: all 2 vsyncs apart).
- **No crash at the end of a run with frames in between or with `RT_GPU_BENCH`:** when the
  process ended from the game's thread (`--seconds`, a fatal error), `exit()` tore down the
  OpenGL driver while the main thread was still drawing (a segmentation fault, now and then).
  The thread that draws now stops outside OpenGL first.

## [0.20.0] - 2026-11-19

### Added

- **Enhanced mode, high frame rates** (issue #20): DISPLAY → FRAME RATE offers ORIGINAL (the
  games' own 29 fps), 60 or 120 FPS, in Thrill Drive 2 and GTI Club 2, every version, with the
  HARDWARE renderer. The pictures between two of the game's own are drawn from the same scene
  with every car, wheel and object moved to where it is at that moment: the game itself, its
  speed and multiplayer do not change. They come on every vsync, every other one or every third,
  always evenly spaced: on a 144 Hz screen 120 gives 72 and 60 gives 48, so set the screen to 120
  or 60 Hz for those rates exactly; 120 is offered when a screen connected is that fast. The
  picture runs about one game frame (35 ms) behind, and each picture in between costs about
  1.6 ms of one core on an Apple M4 Pro plus a drawing on the GPU: at 4X, check the fps counter,
  which counts the pictures shown.
- **Enhanced mode, resolution up to 4X** (issues #16 and #22): DISPLAY → RESOLUTION offers AUTO
  and 1X to 4X with the HARDWARE renderer (3X fits 1080p, 4X 1440p); the SOFTWARE renderer stays
  at 1X–2X. AUTO, the new default, picks the smallest scale that covers the picture's height in
  the window or on the screen, up to 3X, and follows resizes and fullscreen (with the SOFTWARE
  renderer it is 1X). A saved 2X is kept; the old default 1X becomes AUTO.
- **Enhanced mode, GTI Club 2 draws its scenery twice as far:** buildings, trees and roadside
  objects keep their detailed models twice as far before switching to the simpler ones, and the
  farthest ones appear twice as far (the distances are in each course's data, scaled once it is
  loaded). Ver JAB and EAA; the classic mode keeps the original distances, and the cars keep
  their own detail.

### Changed

- **The same volumes in every version:** the starting NVRAMs come from cabinets set anywhere
  from 1 to 20 (Thrill Drive 2 EBB 3, AAA 10, JAA 20; GTI Club 2 JAB 1–2, EAA 6–8), and with the
  port's ×16 gain AAA, JAA and EAA clipped. Music and effects volume are now 18 (60%) in every
  version, set at boot while they are still the dump's, and the gain is ×1 (`--volume`): EBB
  sounds as before, JAB a little louder, the others no longer distort. Volumes changed in
  OPTIONS → SOUND or in TEST MODE are kept.
- **GTI Club 2 (ver EAA): sound in the attract mode all the time,** as ver JAB and Thrill Drive
  2 (its starting NVRAM plays it once every four cycles), after the first-run calibration. An
  NVRAM saved before this version keeps its setting.
- **Enhanced mode, SUPPORT in the main menu** (SOSTIENI in Italian), in place of CREDITS: it
  opens the CREDITS page, which now ends with SUPPORT, opening the project's GitHub Sponsors page
  in the browser, and BACK.
- **Enhanced mode, GPU renderer:** the menu and fps-counter layer is drawn at most 768 lines
  high and stretched over the picture, so higher resolutions do not upload a huge layer every
  frame.

### Removed

- **Enhanced mode, DISPLAY → TEXTURE FILTER** (ORIGINAL / NEAREST): textures and the window
  always take the game's own filtering. A `texture_filter` line left in the port settings is
  ignored, and the software renderer is about 300 KB smaller.

## [0.19.0] - 2026-11-18

### Added

- **Gamepad rumble** (pull request #5): in the enhanced mode the gamepad vibrates with the
  force-feedback motor of the cabinet's steering wheel. The motor's force is stretched over the
  pad's range where the games use it (forces 3 to 11 span 25% to 100% at 1.0X), and the side
  the wheel pulls to leads on the pad's motor of that side. RUMBLE STRENGTH, OFF to 4.0X, is in
  OPTIONS → CONTROLS and in the pause menu, next to STICK RESPONSE. F8 (or Capture on a Switch
  Pro controller) tests it. On Linux some pads expose rumble only in their Xbox (XInput) mode:
  see the guide.
- **Enhanced mode, deluxe cabinet:** every version declares the cabinet with the
  force-feedback steering wheel and the handbrake, as Thrill Drive 2 JAA and AAA and GTI Club 2
  JAB have. Thrill Drive 2 EBB and GTI Club 2 EAA now have the handbrake (B, Space) and drive
  the motor, so they rumble too. Saves from earlier versions get it at the next launch; the
  classic mode keeps each version's own cabinet.

### Changed

- **Thrill Drive 2 (ver AAA):** the attract mode has sound by default, as ver EBB since 0.17.0
  (`calibration.nvram_set` `0x94` = `0x8A`, music volume kept). Ver JAA's starting NVRAM already
  has it. An NVRAM saved before this version keeps its setting.

## [0.18.0] - 2026-11-18

### Added

- **Gamepad, stick response** (pull request #7): OPTIONS → CONTROLS, and CONTROLS in the
  pause menu, choose how the left stick turns the wheel: LINEAR, SOFT (quadratic) or EXTRA
  SOFT (cubic, the default: half of the stick's travel turns the wheel an eighth). The
  deadzone is rescaled, so the wheel no longer jumps when the stick leaves it. Saved in
  `<executable>_enhanced_settings.ini`.
- **Gamepad, right stick as pedals** (pull request #7): up accelerates and down brakes,
  proportionally, for controllers with digital triggers (Nintendo Switch Pro). Of the
  triggers, the right stick and the buttons, the strongest pedal wins.
- **Gamepad, D-pad steering:** left and right steer, ramping as the keyboard does.
- **Gamepad, Back pauses** in the enhanced mode (and goes back in the menus), as Guide does:
  Steam and the consoles often keep Guide for themselves. Back never quits; in the classic
  mode it is still COIN.
- `RT_STICK_DEADZONE`, `RT_STICK_CURVE`, `RT_TRIGGER_DEADZONE` and `RT_STEER_SPEED` tune the
  gamepad; F9 logs the stick and the steering sent to the game.

### Changed

- **Gamepad, steering speed:** the stick turns the wheel at the keyboard's speed, centre to
  full lock in a quarter of a second, as a real wheel turns. It set the wheel at once, so a
  quick flick to full lock flipped GTI Club 2's car over.
- **Gamepad, brake and handbrake buttons:** X brakes and B is the handbrake (they were B and
  X). The triggers are unchanged, and B still goes back in the menus.
- **Menus:** left and right repeat while held (pull request #7).

### Fixed

- **Gamepad, held buttons** (pull request #7): an accelerator or brake button pressed while a
  menu or the pause took the press now counts once play resumes. Only one controller is read;
  unplugging it releases its controls and takes the next one, and the window losing the focus
  releases everything held.

## [0.17.0] - 2026-11-18

### Changed

- **Thrill Drive 2 (ver EBB):** the attract mode has sound by default, in the classic and the
  enhanced mode. The starting NVRAM has SOUND IN ATTRACT MODE: COMPLETE OFF; the first-run
  calibration sets ALL THE TIME (`calibration.nvram_set` `0x94` = `0x83`, music volume kept).
  TEST MODE or OPTIONS → SOUND can still change it. An NVRAM saved before this version keeps
  its setting. Versions JAA and AAA are unchanged for now.

## [0.16.2] - 2026-11-18

### Changed

- **RecompLauncher:** a shorter description of the folder to pick, with the files each game
  needs.

## [0.16.1] - 2026-11-18

### Fixed

- **RecompLauncher's Play started the setup instead of the game** ("'viper-setup --list' shows
  what each game needs"): the launcher started `viper-setup` without the tile's arguments.
  `--play` is now in the arguments it always passes, and `viper-setup --play` finds the game
  anywhere among the arguments (without one, it starts Thrill Drive 2, or the first game built).

## [0.16.0] - 2026-11-17

### Added

- **RecompLauncher support:** the packages carry a `recomplauncher.json`, so
  [RecompLauncher](https://recomplauncher.com) can install the port, build Thrill Drive 2 and
  GTI Club 2 from the player's folder of MAME files, and start each game from its own tile, in
  the enhanced or the classic mode. For it, `viper-setup` has two new options: `--prepare`
  (progress lines, no prompt at the end) and `--play GAME` (starts a game it built).

### Changed

- **The Linux package is a `.tar.gz`** (it was a `.tar.xz`), the format RecompLauncher installs
  on Linux. It is extracted the same way.

## [0.15.1] - 2026-11-17

### Fixed

- **Thrill Drive 2, EXTRA modes SPEED and POLICE:** on SELECT A CAR the brake switched to the
  special cars, getting around the mode's car (the bus, the police car). The brake is now held
  released there, like the steering.
- **Multiplayer between different versions of a game** (for example Thrill Drive 2 EBB and
  JAA, issue #19): the lobby showed the player as linked, but the games ignored each other and
  each raced alone, on its own course. The host now refuses a player with another version, who
  sees THE HOST PLAYS ANOTHER GAME VERSION. All the players need the same version of the port
  too.

## [0.15.0] - 2026-11-13

### Changed

- **Enhanced mode: free play is set at every boot**, in the NVRAM (profile field
  `enhanced.free_play`), instead of by a TEST MODE pass on first launch. The first launch is
  shorter, and an NVRAM without free play is repaired (see Fixed).
- **The first-run calibration runs in the classic mode** (`--classic`), whatever the mode of the
  launch; the calibrated NVRAM is the same. Its output goes to `<binary>_first_run.log` next to
  the game, kept only when the calibration fails, and the console names that file.
- **README:** the controls say that START changes the view during a race, as on the cabinets.

### Fixed

- **Enhanced mode without a menu:** when the first-launch TEST MODE pass failed (it happened on
  a Windows PC), the game stayed without free play. The menu then never appeared, and COIN and
  TEST are ignored in this mode, so the attract mode played on and no key did anything.
  Starting the new version is enough: the NVRAM is repaired at boot.
- **F11 in the classic mode** switched to fullscreen for a frame and back: the enhanced mode's
  DISPLAY setting, which the classic mode does not have, undid it.

## [0.14.0] - 2026-11-05

### Added

- **EXTRA modes** (enhanced mode, main menu EXTRA): alternative rules for a race.
  - **Gravity** (GTI Club 2): every 10 seconds the gravity turns light (0.1 to 0.5 times the
    normal one) or heavy (2 to 4 times), in the air and in the grip on the road, with 5-second
    moments at zero G; the CPU cars fly by it too, and the time is 1.2 times longer.
  - **Speed** (both games): a bomb arms itself at 50 km/h (30 mph with the speedometer in mph)
    and explodes after 2 seconds below it, ending the race. In Thrill Drive 2 the car is the bus.
  - **Pass the bomb** (GTI Club 2): a bomb passes from car to car on contact; when it explodes,
    that car goes up in flames (the game's own fire) and stands still for 4 seconds. The time is 1.2 times longer.
  - **Elimination** (GTI Club 2): every 45 seconds the last car is out; the goal is only a
    checkpoint until one car is left, and the winner goes on to the next course.
  - **Police** (Thrill Drive 2): in the police car, the CPU cars are on the run; staying right on
    one for 3 seconds in a row arrests it.
- **OPTIONS → GAME → SPEEDOMETER** (all versions): km/h or mph, the TEST MODE setting.
- `RT_DUMP_RAM_EVERY` and `RT_DUMP_RAM_RANGE`: RAM dumps every N frames, for finding the game's
  variables.

## [0.13.0] - 2026-11-03

### Added

- **A download for Windows x86-64**, as for Linux and macOS: `viper-setup.exe` builds the games
  on the player's computer with the zig it carries and SDL2's MinGW package (`SDL2.dll` goes
  next to the games). Double-clicked, it waits for Enter before closing its window.

### Changed

- **The build from the sources no longer needs chdman** (`rom-tools`, `mame-tools`): `make check`
  and `make extract` build `tools/chdextract.c` (libchdr) into `build/tools/` and read the CHDs
  with it, as the packages do.
- **One description of the build:** the sources, flags and libraries of the executables are in
  `tools/buildspec.py`, used by the Makefile (through `build/make-vars.mk`) and by the packages'
  launcher. On x86-64 both build for the computer's own CPU without AVX-512
  (`-march=native -mno-avx512f`) instead of `-mfma -mavx2`: with AVX-512, gcc changes a few
  pixels of the software renderer, and the builds are compared bit for bit.
- **`viper-setup` starts at once and says what it is doing:** it is now a folder (its Python in
  `engine/`) instead of a single file, which unpacked itself to a temporary folder at every
  start, slowly where an antivirus scans it; it prints its version at once, and a line before
  the first-time tool build and before linking.

### Removed

- **`make WIN=1`**, the Windows cross-build from macOS: the Windows package replaces it.

### Fixed

- **Linux package without a system SDL2:** the games now use the package's copy, as intended.
  In 0.12.0 the check for a system SDL2 found the copy PyInstaller had put inside `viper-setup`,
  so the games were linked without the path to the package's copy and did not start.
- **Windows: a click in the console window no longer stops the program.** With the console's
  QuickEdit mode (on by default) a click starts a selection, and Windows suspends the program at
  its next output until a key is pressed: `viper-setup.exe` looked stuck, and the game would
  stop at its next log line. Both turn QuickEdit off while they run, and back as it was at exit.
- **Windows: the restart after changing the game options** (enhanced mode) no longer fails when
  the game's path has spaces (`C:\Users\Name Surname\...`): the arguments are quoted.

## [0.12.0] - 2026-11-03

### Added

- **Downloads for Linux x86-64 and macOS on Apple Silicon**, with no toolchain to install: a
  package holds a launcher, `viper-setup`, the runtime's sources and a C compiler (zig), and no
  game code. From the player's dumps, found anywhere under `roms/` or a `--roms` folder, loose or
  in MAME's zips, it builds the games on their computer, in about a minute, for that computer's
  CPU (a folder moved to another computer is rebuilt). The GitHub release workflow makes the
  packages (`dist/build-release.sh`).
- **Idle loop skipping:** Thrill Drive 2 and GTI Club 2 spend about half of their CPU time in the
  idle task's polling loop. The passes of that loop that would find nothing to do are now charged
  without running them: the game itself runs exactly the same (registers and frames compared over
  a race, in all five versions), with about 40% less CPU time for the game code; with the GPU
  renderer that is most of the program's CPU time. The profiles mark the loop (`"idle"` in
  `game.json`, checked against the code's CRC-32, so a version with other code ignores it);
  `RT_IDLE=0` turns it off.
- **CHDs read without chdman:** `tools/chdextract.c`, on libchdr, does what the build needs of
  chdman (the CHD's SHA1, the disk image). The package uses it; `tools/game.py` and
  `tools/extract.py` use it when `VIPER_CHDEXTRACT` names it, otherwise chdman as before.

### Changed

- **The enhanced mode is now the default:** the game starts in enhanced mode, also when the
  executable is double-clicked; `--classic` starts the classic mode, the cabinet as it was.
  `--enhanced` is still accepted. The two modes keep their saves apart as before
  (`<executable>_enhanced_nvram.bin` and `<executable>_nvram.bin`): who played in classic mode
  until now goes through the enhanced mode's first-launch setup once, and finds the classic
  saves untouched with `--classic`.
- **Link play, smoother over the Internet:** the other players' cars stutter much less.
  - Each player sends one packet per game frame instead of two identical ones, and the host sends
    its own frame and the ones it passes on in a single packet per player: about half the
    traffic, and a third of the packets with 4 players.
  - The buffer against the network's delays adapts to the connection, from 1 to 4 frames
    (35–140 ms); a packet that arrives out of order is no longer thrown away, and a lost one costs
    one frame instead of two hitches.
  - The packets are marked as real-time traffic, which most home routers send first over Wi-Fi.
  - **Not compatible with 0.11.1:** all the players need this version. A player who joins a host
    with another version is told so (THE HOST HAS ANOTHER VERSION).
  - `--net-buffer N` now counts game frames (default 1), not packets.

## [0.11.1] - 2026-10-10

### Fixed

- **Hardware rendering on older or slower GPUs:** the game no longer runs far ahead of a GPU that
  cannot keep up, which then showed the frames in bursts while the fps counter still read 30;
  the game now slows down evenly, and the counter shows it. The shader also does about a quarter
  less work per pixel (the draw's state in uniforms, the triangle's data read per vertex, the W
  and depth values only where used): a GeForce 820M laptop went from unplayable to smooth.
- **Link play, the host gone:** when the host quits or drops during a game, the other players go
  back to the main menu after about 5 seconds, with a notice that says why (THE HOST CLOSED THE
  SESSION / NO ANSWER FROM THE HOST), instead of staying in a race where the clock and the
  traffic stood still.
- **Link play on macOS, hosting:** a network error opening the port on the router (blocked by
  security software, for example) made the game vanish without a message (SIGPIPE); now the game
  carries on and the log says why the port did not open. With NAT-PMP, a router that refuses the
  default public port (a second host in the same home) is asked for the next ones, as with UPnP.

### Added

- The log says why the window closed (`frontend: …`), and which chip draws the graphics at
  startup (`renderer: …`, in 0.11.0 too).
- `tools/gpu_replay.cpp` times the GPU work of each list; `RT_GPU_DEFINES` compiles profiling
  variants of the shader; `RT_GPU_BENCH` prints the GPU time per vblank.

## [0.11.0] - 2026-10-09

### Added

- **Hardware rendering (enhanced mode, both games):** the GPU draws the Voodoo's 3D graphics, with
  OpenGL 3.3 on macOS, Windows and Linux. It is the default in the enhanced mode; OPTIONS →
  DISPLAY → RENDERER switches back to SOFTWARE (`renderer` in the settings file), and a computer
  without OpenGL 3.3 falls back to it by itself. The classic mode always draws in software.
  - The fragment shader runs the Voodoo's pixel pipeline with the rasterizer's own arithmetic:
    opaque pixels match the software renderer; translucent ones can differ slightly.
  - Thrill Drive 2's rear-view mirror, reflections, fog and motion blur (also in widescreen) work.
  - GTI Club 2, 90 s of attract on an Apple M4 Pro: 27% less time at 1X and 39% at 2X, with 63%
    and 78% less CPU time; 2X costs the same as 1X.
  - On Windows laptops with two GPUs it runs on the dedicated one.
  - At startup the terminal shows which chip draws the graphics (`renderer: hardware, <GPU>` or
    `renderer: software (CPU)`), also after changing RENDERER.
- Development tools for it: `RT_GPU_CAPTURE` with `tools/gpu_replay.cpp` and
  `tools/gpu_compare.py` (GPU frames against the software ones of the same run), `RT_GPU_BENCH`
  (headless benchmark on the GPU), `tools/ppm2png.py`.

### Changed

- The software renderer's frames are unchanged (bit-identical on both games, classic and
  enhanced, 1X and 2X).

## [0.10.1] - 2026-10-08

### Changed

- **More render threads by default on 4- and 8-thread CPUs:** one per hardware thread minus one,
  up to 4 (it was a third of them, so a 4-core CPU got one). `RT_RENDER_THREADS` still overrides it.
- **Faster software rendering (about 1.5× per thread), both games:** the Voodoo rasterizer has
  specialised versions for the 150 mode combinations that draw 99% of the pixels, measured in
  attract mode and in races on every course; before, every pixel took the generic version,
  which decodes the modes at each pixel. The frames are bit-identical.
- **Bilinear texture filter inlined into the rasterizers:** about 15% less time per thread.
- **TEXTURE FILTER NEAREST keeps the faster rendering:** the table also has the variants of each
  combination that this option produces.

### Added

- `make EXTRA=-DRT_RASTER_STATS`: logs the pixels drawn per rasterizer mode combination, to
  rebuild the table.

## [0.10.0] - 2026-10-07

### Added

- **Link play (enhanced mode, MULTIPLAYER), both games:** up to four players race together over
  the Internet, as linked cabinets did; the CPU cars fill the rest of the grid.
  - HOST A GAME opens the host's UDP port on the home router by itself (NAT-PMP or UPnP) and
    shows a session code of 7 characters; the others type or paste it in JOIN A GAME. No
    server, nothing to install, nothing to set up on the joining side.
  - The lobby shows the four players; the host starts the race once one at least is linked, and
    the others' games join it by themselves. While a session is open, the lobby replaces the
    main menu; there is no pause in link play.
  - No restart: a player takes its NETWORK ID live, as the game's TEST MODE does, and takes ID 1
    back on leaving.
  - Network drops are concealed: the game does not see a short drop at all; a player lost for
    good stays on the track standing still (GTI Club 2, which would otherwise stop the race with
    NETWORK ERROR) or leaves the race (Thrill Drive 2); one whose connection comes back within
    30 s goes on racing linked.
  - A player in the host's own home finds it on the local network, on every network interface
    and also when the router gave the host another external port; a second host in the same
    home takes the next free port.
  - Command line: `--net-host`, `--net-join CODE`, `--net-port`, `--net-buffer`; `--net-id` with
    `--net-peer` and `--realtime` for tests.
- **Third-party code:** miniupnpc 2.3.3 and libnatpmp (BSD-3-Clause), in `third_party/`,
  compiled into the executable.
- **Windows cross-build (experimental):** `make WIN=1 GAME=<id>` builds a self-contained
  `<bin>.exe` on macOS with mingw-w64 and the SDL2 MinGW package. Tested in link play on one PC.

### Changed

- **NETWORK ID:** the profile's `network.id_addr` replaces `nvram_force`; the ID is still set to
  1 at every boot, or to `--net-id`.
- **Fps counter (enhanced mode):** counts frames per real second, so a host too slow for the game
  shows its real frame rate instead of 29.
- **Session code entry:** pasting replaces what was typed; Enter does nothing until the code is
  complete.

## [0.9.3] - 2026-10-02

### Added

- **Enhanced mode, DISPLAY → TEXTURE FILTER:** ORIGINAL (the game's filtering, the default) or
  NEAREST, which turns off the filtering of enlarged textures, for hard texel edges, and scales
  the window without smoothing. The filtering of distant textures and the mipmaps stay the
  game's. Applied at once and saved in the port settings. With ORIGINAL, and in classic mode,
  the frames are the same as before, bit for bit.

## [0.9.2] - 2026-10-02

### Added

- **The window keeps its position and size** from one run to the next, in both modes, in the
  port settings. Fullscreen, maximized and minimized bounds are not kept, and a window saved on
  a monitor that is gone opens on one that is connected. With a widescreen aspect ratio, a
  window that already has it is left as it is. `--scale N` sets the size and keeps the saved
  position.

### Changed

- **Port settings, one file per mode, as the NVRAM:** `<executable>_settings.ini` for the
  classic mode (the window) and `<executable>_enhanced_settings.ini` for the enhanced mode
  (display options and the window). **Rename an existing `<executable>_settings.ini` to
  `<executable>_enhanced_settings.ini`** to keep the enhanced-mode display options.
- **Run `make -j8 game` again** for each game: the profiles' generated header has new
  constants, and the plain build stops without them (README, "Build in detail").

## [0.9.1] - 2026-10-02

### Changed

- **Gamepad Guide (Home) works like Esc:** in enhanced mode it pauses, goes back in the menus
  and quits from the main menu, where before it did nothing there; in classic mode it quits.

## [0.9.0] - 2026-10-02

### Added

- **Enhanced mode, course select (Thrill Drive 2 and GTI Club 2) and transmission select
  (GTI Club 2):** ← → (A D, the D-pad or the stick) step from one choice to the next, and the
  choice stays where it is. On a cabinet the wheel position picks it, so with a key it sprang
  back to the centre one as soon as the key was released.

- **Enhanced mode, Thrill Drive 2 in Italian:** four typos of the game's texts are fixed:
  "per Trasmissione Manuale" (was "Tasmissone Maniale"), "emergenza" (was "emargenza"),
  "Costo totale dei danni" (was "Dei"), "TECNICA." (was "TECNICA ."). The textures are
  rebuilt in VRAM from letters of the same textures; the classic mode keeps the originals.

### Fixed

- **NETWORK ID 1 (GTI Club 2 JAB, Thrill Drive 2 EBB), both modes:** the NVRAM dumps come
  from cabinet 2 of a linked set, so the race HUD said PLAYER 2 (and GTI Club 2's rank list
  2P). The ID is now set to 1 at every boot (`nvram_force`), also in NVRAMs already saved.

### Changed

- **Voodoo screen-to-screen blits** copy a row at a time (`memmove`), without a temporary
  buffer: about 90 times cheaper, same picture.

## [0.8.2] - 2026-10-02

### Fixed

- **Thrill Drive 2: motion blur.** The attract mode and the CRASHED replay showed full-screen
  multicoloured noise where a cabinet shows a motion blur. The game copies each frame into
  textures with 2D screen-to-screen blits, which MAME's Voodoo core leaves unimplemented; the
  Voodoo core now performs them (also at 2X). GTI Club 2 uses no 2D blits.

### Changed

- **Enhanced mode, CREDITS:** the static recompilation is credited to SPITA90.

## [0.8.1] - 2026-10-02

### Added

- **All file names:** every file of the five versions now has its real name
  (`tools/names.txt`), found by hashing candidate paths, so nothing is extracted to `_unk/`.
  **Run `make extract` again** for each game: the enhanced-mode font now comes from
  `game/mdldata/COMMON_tex.zin` (GTI Club 2) and `game/gldata/VRAM_tex.zin` (Thrill Drive 2).
- `RT_CF_LOG=1` logs every CF read command, to see which game files are loaded.
- **Docs:** hidden and unused content of both games (ARCHITECTURE.md, section 5d): cut car,
  unused title logos, test data, factory tools.

### Changed

- `make extract` empties `work/<id>/fs/` first, so files extracted under old names do not stay.

## [0.8.0] - 2026-10-02

### Added

- **Enhanced mode, rankings name entry from the keyboard** (Thrill Drive 2 EBB, JAA, AAA; GTI
  Club 2 JAB, EAA): the initials are typed instead of being chosen with the steering wheel.
  Backspace deletes, Enter ends the name; Left/Right (or the D-pad) and the accelerator still
  pick and take a letter, for a gamepad. Two profile hooks, `name_index` and `name_confirm`,
  and the profile's `name_entry` section describe each game's routine.

### Changed

- **Keyboard:** WASD drives too (A/D steer, W accelerator, S brake), next to the arrows, and
  moves through the enhanced-mode menus. The gear shift moves from A/Z to **E** (up) and **Q**
  (down). A control held by two keys or buttons (e.g. ↑ and the gamepad's A) is released only
  when both are.

### Fixed

- **macOS:** holding a letter key (WASD while driving) no longer opens the system's accent
  picker. Text input is now on only during the enhanced mode's name entry.
- **Enhanced mode, macOS:** after a restart for new settings the game window takes the focus
  back, so it gets the keyboard without a click (`SDL_RaiseWindow` with `SDL_FORCE_RAISEWINDOW`).

## [0.7.0] - 2026-10-01

### Changed

- **GTI Club 2:** PROMOTION MODE is on by default, in the classic and the enhanced mode. The
  first-run calibration sets it (`calibration.nvram_set` `0x85` = `0x80`); ver EAA's starting
  NVRAM already had it, ver JAB's did not. TEST MODE can still turn it off in the classic mode.
  An NVRAM saved before this version keeps its setting.

## [0.6.1] - 2026-10-01

### Fixed

- **Enhanced mode, 2X:** stray coloured pixels along the top and left edges of the menu
  letters. The bilinear font sampling truncated the coordinate −1/4 to 0 instead of flooring
  it, so it extrapolated and the alpha overflowed.

### Changed

- **README:** the enhanced-mode screenshots show both games at 2X and 16:9.

## [0.6.0] - 2026-10-01

### Added

- `make game GAME=<id>` extracts, recompiles and builds a game in one step.
- **Widescreen** (enhanced mode, DISPLAY → ASPECT RATIO): 4:3, 16:10, 16:9 or 21:9, at 1X and
  2X, for Thrill Drive 2 (EBB, JAA, AAA) and GTI Club 2 (JAB, EAA). The game draws more of the
  scene on each side (Hor+); the HUD stays in the 4:3 centre. `aspect` in the settings file.
  - Hooks after every write of the gl library's projection and viewport widen the frustum
    (culling and clipping included) while keeping each pixel where it was; the Voodoo core
    renders the displayed buffers with a margin on each side.
  - Untextured fades over the whole 4:3 picture are stretched to the full width.
  - The window keeps its height and takes the new width.
- **Profiles:** `enhanced.widescreen` (gl state addresses); a hook name may mark several
  addresses (`GAME_ENH_HOOKS`).

### Fixed

- **2X resolution:** flickering road decals (zebra crossings, the start line, lane markings).
  The rasterizer measures every parameter from the whole pixel holding vertex A; at 2X that
  pixel is a different fraction of a native pixel for each triangle, so coplanar decals and the
  road got slightly different depths and the depth test flipped between frames. The start values
  now follow the native ones exactly. Compared with 1X over a race, the pixels that differ
  strongly from the native picture are about halved, and what remains is edges.

### Changed

- **Enhanced mode:** COIN is ignored too, like TEST and SERVICE (the game is on free play).
- **Enhanced mode:** a colon missing from the game font (Thrill Drive 2) is drawn as two full
  stops.
- **README:** reorganised around the two modes, classic (the arcade cabinet) and enhanced (like
  a PC game), with screenshots in `docs/images/` and step-by-step build instructions. The
  reference material follows below.

## [0.5.0] - 2026-09-30

### Added

- **Thrill Drive 2 ver JAA (`thrild2j`) and ver AAA (`thrild2a`):** boot and race, with
  automatic calibration.
  - Their cabinet is the GTI Club 2 JAB one (K-type force-feedback wheel and a handbrake), so
    the profiles use GTI Club 2 JAB's calibration script and handbrake input.
  - JAA's first-run calibration sets the currency to Japanese yen (the starting NVRAM has U.S.
    dollars, and TEST MODE cannot change it).
  - Enhanced mode, with Japanese as a third language.
- **Profiles:** `calibration.nvram_set` writes NVRAM bytes after the calibration.

### Changed

- **Profiles:** `nvram_options` moved from `enhanced` to the top level.

### Removed

- **Thrill Drive 2 ver EAA (`thrild2c`):** profile and ROM folder removed. MAME marks the only
  known CF dump as bad, and its NVRAM has never been dumped.

## [0.4.0] - 2026-09-30

### Added

- **Rendering resolution** (enhanced mode, DISPLAY page): 512×384 or 1024×768 internal.
  - The displayed colour buffers are rendered at twice the resolution in host memory, with
    scaled vertices and gradients. Off-screen render targets stay native.
  - The emulated timing is unchanged.
  - The default mode is bit-identical to before.
- **Debugging for the frame rate and resolution study:**
  - `RECOMP_CYCLE_SCALE=k` recompiles with k cycles per instruction (a k times slower CPU);
  - `RT_VOODOO_SWAP_INTERVAL=n` forces the Voodoo swap interval, and `RT_VOODOO_TEXLOG` also logs
    the first swap commands;
  - `RT_VOODOO_FBSTATS=1` prints how the game uses the framebuffer: CMDFIFO packet types, 2D
    blits, LFB accesses by MB, colour buffer addresses.

## [0.3.0] - 2026-09-30

### Added

- **Enhanced mode** (`--enhanced`): an optional mode that makes the ports behave like PC games,
  for Thrill Drive 2 EBB and GTI Club 2 JAB and EAA. The default mode is unchanged.
  - **Free play:** set up on first launch through TEST MODE, after the calibration. The
    "FREE PLAY" and "PRESS START BUTTON" captions are hidden, and TEST MODE cannot be opened.
  - **Attract menu:** START GAME, OPTIONS, CREDITS, QUIT, drawn with each game's own font.
  - **Options:**
    - GAME: course difficulty and language.
    - SOUND: attract sound, music in game, music and effects volume.
    - DISPLAY: window or fullscreen, and an fps counter.
  - **Applying the options:** game options are written to the game's NVRAM, and the game
    restarts to apply them. Display options go to `<executable>_settings.ini`.
  - **Menu language:** the menus are in English or Italian, following the game's language option.
  - **Pause:** Esc (or the gamepad's Guide button) pauses a game, with RESUME and MAIN MENU.
    MAIN MENU returns to the attract mode without closing the window.
  - **Fast boot:** the game boots unpaced and muted behind a loading screen, and reaches the
    attract mode in a few seconds.
  - **Separate saves:** the enhanced mode keeps its own NVRAM, `<executable>_enhanced_nvram.bin`.
- **Recompiler hooks:** profile-driven hooks, where the generated code calls `rt_hook()`
  before chosen instructions.
- **Game profiles:** an `enhanced` section with the setup script, strings to hide, hooks, the
  menu font, the NVRAM option block and the option fields.
- **Debugging:**
  - `RT_VOODOO_VRAMDUMP=path:frame` dumps the Voodoo memory at a given frame;
  - `RT_VOODOO_TEXLOG` now prints timestamps;
  - `RT_ENH_LOG`, `RT_ENH_MENU`, `RT_ENH_BLANK` and `RT_NVRAM_POKE` log and script the enhanced
    mode in headless tests.
- `--settings FILE` selects the enhanced-mode settings file.

### Changed

- Unverified profiles (`thrild2j`, `thrild2a`, `thrild2c`) do not inherit the enhanced mode.

## [0.2.1] - 2026-09-30

### Fixed

- **Voodoo texture corruption:** noise on walls, street lights, headlights, fog and lens flares,
  also present in MAME's Voodoo core. With multibase textures, the hardware adds each LOD's
  offset within the mipmap chain to its base register, and the core now does the same.

### Added

- `RT_VOODOO_TEXLOG=1` logs each new texture setup.

## [0.2.0] - 2026-09-29

### Added

- **GTI Club 2:** GTI Club: Corso Italiano (ver JAB) and Driving Party: Racing in Italy
  (ver EAA) boot and are playable, with automatic calibration.
  - **Handbrake:** JAB has one and calibrates it. EAA has none, as its cabinet configuration
    says.
  - **K-type force-feedback wheel:** the motor register is modelled, so the motor-driven
    steering calibration passes (`RT_FFB_WHEEL`, set by the first-run calibration).
- `coverage.py` applies the profile hints.
- Breakpoint output includes r0.

### Fixed

- **Boot word r31** is built from IN2 like the BIOS does. Before, GTI Club 2 EAA showed its
  password lock screen.
- **Default paths** are resolved against the executable's directory, so a game can be started
  from the file manager.

## [0.1.0] - 2026-09-29

### Added

- **First public release:** static recompilation of Konami Viper games. Thrill Drive 2
  (ver EBB) is playable at 30 fps with sound, and steering and pedals are calibrated
  automatically.

[Unreleased]: https://github.com/spita90/konami-viper-recomp/compare/v0.22.0...HEAD
[0.22.0]: https://github.com/spita90/konami-viper-recomp/compare/v0.21.0...v0.22.0
[0.21.0]: https://github.com/spita90/konami-viper-recomp/compare/v0.20.0...v0.21.0
[0.20.0]: https://github.com/spita90/konami-viper-recomp/compare/v0.19.0...v0.20.0
[0.19.0]: https://github.com/spita90/konami-viper-recomp/compare/v0.18.0...v0.19.0
[0.18.0]: https://github.com/spita90/konami-viper-recomp/compare/v0.17.0...v0.18.0
[0.17.0]: https://github.com/spita90/konami-viper-recomp/compare/v0.16.2...v0.17.0
[0.16.2]: https://github.com/spita90/konami-viper-recomp/compare/v0.16.1...v0.16.2
[0.16.1]: https://github.com/spita90/konami-viper-recomp/compare/v0.16.0...v0.16.1
[0.16.0]: https://github.com/spita90/konami-viper-recomp/compare/v0.15.1...v0.16.0
[0.15.1]: https://github.com/spita90/konami-viper-recomp/compare/v0.15.0...v0.15.1
[0.15.0]: https://github.com/spita90/konami-viper-recomp/compare/v0.14.0...v0.15.0
[0.14.0]: https://github.com/spita90/konami-viper-recomp/compare/v0.13.0...v0.14.0
[0.13.0]: https://github.com/spita90/konami-viper-recomp/compare/v0.12.0...v0.13.0
[0.12.0]: https://github.com/spita90/konami-viper-recomp/compare/5cca0f5...v0.12.0
[0.11.1]: https://github.com/spita90/konami-viper-recomp/compare/52d4260...5cca0f5
[0.11.0]: https://github.com/spita90/konami-viper-recomp/compare/0e2bedc...52d4260
[0.10.1]: https://github.com/spita90/konami-viper-recomp/compare/00c16fb...0e2bedc
[0.10.0]: https://github.com/spita90/konami-viper-recomp/compare/ae5a011...00c16fb
[0.9.3]: https://github.com/spita90/konami-viper-recomp/compare/5d7ac70...ae5a011
[0.9.2]: https://github.com/spita90/konami-viper-recomp/compare/786d3bb...5d7ac70
[0.9.1]: https://github.com/spita90/konami-viper-recomp/compare/adb36c8...786d3bb
[0.9.0]: https://github.com/spita90/konami-viper-recomp/compare/99658cb...adb36c8
[0.8.2]: https://github.com/spita90/konami-viper-recomp/compare/07e7a08...99658cb
[0.8.1]: https://github.com/spita90/konami-viper-recomp/compare/f5a9a22...07e7a08
[0.8.0]: https://github.com/spita90/konami-viper-recomp/compare/01f29bd...f5a9a22
[0.7.0]: https://github.com/spita90/konami-viper-recomp/compare/c041b6b...01f29bd
[0.6.1]: https://github.com/spita90/konami-viper-recomp/compare/9b71187...c041b6b
[0.6.0]: https://github.com/spita90/konami-viper-recomp/compare/bd64498...9b71187
[0.5.0]: https://github.com/spita90/konami-viper-recomp/compare/aaa8758...bd64498
[0.4.0]: https://github.com/spita90/konami-viper-recomp/compare/cf5280a...aaa8758
[0.3.0]: https://github.com/spita90/konami-viper-recomp/compare/b10d12a...cf5280a
[0.2.1]: https://github.com/spita90/konami-viper-recomp/compare/298d2e4...b10d12a
[0.2.0]: https://github.com/spita90/konami-viper-recomp/compare/cf317cf...298d2e4
[0.1.0]: https://github.com/spita90/konami-viper-recomp/commit/cf317cf
