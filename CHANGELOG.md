# Changelog

All notable changes to this project are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). While the version is below 1.0.0, a new minor
version can change the build, the profiles or the command-line options.

## [Unreleased]

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

[0.9.0]: https://github.com/spita90/konami-viper-recomp/compare/99658cb...HEAD
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
