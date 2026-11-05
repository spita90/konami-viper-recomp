# Guide

The details behind the [README](../README.md): the enhanced mode's options, saves, the notes on
each version, the files and their SHA1, and building from the sources step by step. The
technical design is in [ARCHITECTURE.md](ARCHITECTURE.md).

- [Enhanced and classic mode compared](#enhanced-and-classic-mode-compared)
- [Enhanced mode](#enhanced-mode)
- [Saves and command-line options](#saves-and-command-line-options)
- [Game notes](#game-notes)
- [Required files and their SHA1](#required-files-and-their-sha1)
- [Build in detail](#build-in-detail)
- [Repository layout](#repository-layout)

## Enhanced and classic mode compared

|                     | Enhanced                                                                                                           | Classic                   |
| ------------------- | ------------------------------------------------------------------------------------------------------------------ | ------------------------- |
| **Starting a game** | START GAME from the menu                                                                                           | Insert coins, press START |
| **Main screen**     | Menu: START GAME, MULTIPLAYER, OPTIONS, CREDITS, QUIT over the attract mode                                        | Attract mode              |
| **Options**         | OPTIONS: course difficulty, language, sound, window or fullscreen, resolution, aspect ratio, renderer, fps counter | None                      |
| **Pause**           | Esc: RESUME or MAIN MENU (not in multiplayer)                                                                      | None                      |
| **Ranking name**    | Typed on the keyboard                                                                                              | Chosen with the wheel     |
| **Multiplayer**     | MULTIPLAYER: up to 4 players over the Internet, with a session code                                                | Linked cabinets only      |
| **Resolution**      | 512×384 or 1024×768 (1X or 2X)                                                                                     | 512×384, as the original  |
| **Aspect ratio**    | 4:3, 16:10, 16:9 or 21:9 (the 3D scene widens; the HUD stays in the centre)                                        | 4:3, as the original      |
| **Graphics**        | Drawn by the GPU (OpenGL 3.3), or by the CPU as in classic                                                         | Drawn by the CPU          |
| **TEST MODE**       | None (TEST, SERVICE and COIN are ignored)                                                                          | F2                        |
| **Save file**       | `td2_enhanced_nvram.bin`, separate from the classic one                                                            | `td2_nvram.bin`           |

## Enhanced mode

- **Menus:** drawn with each game's own font, in English or Italian. They follow the language
  chosen in the game; with Japanese they are in English.
- **Options**, on three pages. Left and right change a value.
  - **Game:** the difficulty of each course, the language (English or Italian; Thrill
    Drive 2 JAA and AAA also offer Japanese), and the speedometer, km/h or mph.
  - **Sound:** attract sound, music in game, music and effects volume.
  - **Display:** window or fullscreen (F11 also switches it), the rendering resolution, and an
    fps counter. 2X renders the 3D scenes and the HUD at 1024×768. With the software renderer it
    needs about twice the CPU time, so be sure to check the fps counter on slower machines.
  - **Renderer:** HARDWARE (the default) draws the 3D graphics on the GPU, with OpenGL 3.3, on the
    dedicated GPU of a laptop that has two: much less CPU time, and 2X costs the same as 1X.
    SOFTWARE draws them on the CPU, as the classic mode, pixel for pixel as the original
    hardware. The pictures are the same, except translucent surfaces, whose colours can differ
    slightly. Changing it restarts the game; a computer without OpenGL 3.3 stays on SOFTWARE.
  - **Aspect ratio:** 4:3 (the original), 16:10, 16:9 or 21:9. A wider format shows more of the
    3D scene on the left and right, with the same vertical field of view; the window widens to
    match. The HUD and the 2D screens keep their 4:3 layout in the centre, and a few 2D effects
    (the noise on Thrill Drive 2's crash screen) cover only that part.
  - **Texture filter:** ORIGINAL (the game's original bilinear filtering) or NEAREST, which turns off the
    filtering of enlarged textures, for hard texel edges, and scales the window without
    smoothing. The filtering of distant textures and the mipmaps stay the game's.
- **Saving the options:** game and sound options are the game's own TEST MODE settings, stored
  in its NVRAM. The game reads them only at boot, so on leaving OPTIONS it restarts, which takes
  a few seconds. Display options are saved in `<executable>_enhanced_settings.ini`
  (`--settings FILE` to use another file).
- **Pause:** Esc, or the gamepad's Guide button, pauses a game, with RESUME and MAIN MENU. MAIN
  MENU brings the game back to the attract mode in a few seconds, behind a loading screen. In the
  attract menu, Esc goes back from a submenu, and quits from the main menu.
- **Ranking name:** when a result enters the rankings, type the initials on the keyboard
  instead of turning the wheel. Backspace deletes, Enter ends the name early. Left and right
  still browse the letters, and the accelerator (or START) takes the one shown, so a gamepad
  works too (D-pad left and right, then R2 or A).
- **Multiplayer:** up to four players race together over the Internet, as linked cabinets did;
  the CPU cars fill the rest of the grid (Thrill Drive 2 races 4 cars, GTI Club 2 6).
  - **Hosting:** MULTIPLAYER → HOST A GAME. The game opens its port on your router by itself
    (NAT-PMP or UPnP, which most home routers have) and shows a **session code** of 7
    characters: send it to the other players. The lobby lists the four players; START GAME
    starts the race for everyone once at least one player has joined. CLOSE THE SESSION ends
    it.
  - **Joining:** MULTIPLAYER → JOIN A GAME, then type the code or paste it (Cmd+V or Ctrl+V)
    and press Enter. You become PLAYER 2, 3 or 4 and wait in the lobby; when the host starts,
    your game starts too. LEAVE THE SESSION takes you back to the main menu.
  - No server and nothing to install. If the router does not open the port (UPnP and NAT-PMP
    off, or a provider's CGNAT), the lobby says so: open UDP port 24700 by hand to host.
    Joining needs nothing on your side.
  - While a session is open, the lobby is the main menu, and there is no pause. A player whose
    connection drops stays on the track standing still (GTI Club 2) or leaves the race
    (Thrill Drive 2), and races on if the connection comes back.
- **Extra modes** (all versions): EXTRA in the main menu lists alternative rules for a race; choosing one
  starts a game with them, and they end with that game. The normal START GAME plays the game as
  it is.
  - **Gravity** (GTI Club 2): every 10 seconds the gravity changes, light (0.1 to 0.5 times the
    normal one) and heavy (2 to 4 times) in turn, and one change in three starts with 5 seconds
    at zero G. Light, the car grips less and bumps throw it far; heavy, it sticks to the
    road and lands hard; at zero G it drifts slowly upwards
    once a bump lifts it. The CPU cars fly by the same gravity. The time is 1.2 times longer
    (at the start and at every checkpoint).
  - **Speed** (both games): as in the film, a bomb arms itself once the car reaches 50 km/h
    (30 mph with the speedometer in mph); from then on 2 seconds below it set it off, and the
    race ends there. In Thrill Drive 2 you drive the bus: the car select shows it for 3 seconds,
    time to choose the transmission (shift up for manual), and the wheel does not change it.
  - **Pass the bomb** (GTI Club 2): 3 seconds after the start a car gets a bomb with a 25-second
    fuse; touching another car passes it on (not straight back for 5 seconds). The marker over
    the car with the bomb blinks, and the top of the screen says which car has it (by its marker:
    TOP, 2ND, ...), ahead or behind, and how far. The time is 1.2 times longer. When it explodes, that car goes up in
    flames and stands still for 4 seconds (yours: no accelerator, brake held), and soon another car gets a new
    bomb.
  - **Elimination** (GTI Club 2): every 45 seconds the last car is out and stops. If it is you,
    your race ends. The goal does not end the race: you race on round the course until one car
    is left (the line gives a third of its usual time). Win, and the game takes you to its next
    course, with all the cars again.
  - **Police** (Thrill Drive 2): you are the police, in the white and blue police car (the car
    select shows it for 3 seconds, time to choose the transmission), the CPU cars are on the run. After a 6-second head start, staying right on one (within about 2.5 m)
    for 3 seconds in a row arrests it (the percentage shows how far the arrest is; moving away starts it
    again), and it stops for the rest of the race. With all of them arrested the
    race ends. The course's own police car does not show up in this mode.
- **Separate saves:** the enhanced mode keeps its own NVRAM, `<executable>_enhanced_nvram.bin`,
  so it never changes the classic mode's settings. Delete that file to set it up again.
- **Fast boot:** the game boots at full speed, muted, behind a LOADING screen, until the
  attract mode starts.

## Saves and command-line options

**Saves:** the calibration and the game's settings (volume, difficulty and so on) are saved to
`<executable>_nvram.bin`, next to the executable, for example `td2_nvram.bin`. To redo the
calibration, delete that file.

**Command-line options:**

```
./td2 --classic          classic mode (the default is the enhanced mode)
./td2 --scale 3          window scale (default 2, or the size of the last run)
./td2 --volume 8         audio gain (default 16)
./td2 --headless --seconds 30        no window/audio, runs as fast as possible (testing)
./td2 --frames DIR --frame-every 60  dump video frames as PPM (with --headless)
./td2 --help             all options
```

The environment variables for debugging are described in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Game notes

**Thrill Drive 2 EBB** (`thrild2`)

- Boots like the original board; attract mode, coin-up, menus and TEST MODE work.
- Fully playable at the original 30 fps, with correct sound.

**Thrill Drive 2 JAA and AAA** (`thrild2j`, `thrild2a`)

- JAA is in Japanese with prices in yen; AAA is in English with prices in Hong Kong dollars.
- They share the same CF card, and the region comes from the NVRAM. The game code is the same
  as EBB's.
- Their cabinet is the GTI Club 2 JAB one: a handbrake and a force-feedback steering wheel
  ("MOTOR TYPE: K-TYPE"). The automatic calibration uses the GTI Club 2 JAB script, handbrake
  included.
- The first-run calibration also sets JAA's currency to Japanese yen: its starting NVRAM has
  U.S. dollars, and its TEST MODE cannot change it.
- Ver EAA (MAME `thrild2c`) is not supported: MAME marks its only known CF dump as bad.

**GTI Club 2 JAB** (`gticlub2`)

- Playable, including the handbrake.
- Its cabinet has a force-feedback steering wheel ("MOTOR TYPE: K-TYPE"). The game calibrates
  the steering by turning the wheel with the motor, and the step fails if the wheel does not
  move. During the first-run calibration the runtime simulates a motorised wheel, so the step
  passes. In play the motor is ignored and the wheel follows your keyboard or gamepad.
- The first-run calibration takes a few seconds, because of the motor test.

**GTI Club 2 EAA** (`gticlub2ea`)

- Boots like the original board; attract mode, coin-up, car and course selection, races and
  TEST MODE work.
- MAME notes that this version needs DIP switch SW:3 on, otherwise it shows "GAME MODE LOCKED!
  PLEASE SET THE PASSWORD". The profile sets it.
- The EAA cabinet has no handbrake: the version code in its NVRAM selects a cabinet
  configuration without one.

## Required files and their SHA1

For a build from the sources, the layout of `roms/` is the same as a MAME rompath: you only need
`kviper/` plus the folders of the games you want to build. The release packages' `viper-setup`
finds the files in any layout, loose or in MAME's zips, by their name and SHA1.

```
roms/
├── kviper/        941b01.u25, ds2430.u3             Viper BIOS set, shared by every game
├── thrild2/       a41b02.chd, a41ebb_nvram.u39      Thrill Drive 2 (ver EBB)
├── thrild2j/      a41a02.chd, a41jaa_nvram.u39      Thrill Drive 2 (ver JAA)
├── thrild2a/      a41a02.chd, a41aaa_nvram.u39      Thrill Drive 2 (ver AAA)
├── gticlub2/      941b02.chd, 941jab_nvram.u39      GTI Club: Corso Italiano (ver JAB)
└── gticlub2ea/    941a02.chd, 941eaa_nvram.u39      Driving Party: Racing in Italy (ver EAA)
```

| File                                    | What it is                                                   | SHA1                                       |
| --------------------------------------- | ------------------------------------------------------------ | ------------------------------------------ |
| `kviper/941b01.u25`                     | Viper board BIOS (GM941B01)                                  | `66ff268d5bf78fbfa48cdc3e1b08f8956cfd6cfb` |
| `kviper/ds2430.u3`                      | DS2430A 1-Wire EEPROM                                        | `ed7cd9b2763b3e377df9663943160f9871f65105` |
| `thrild2/a41b02.chd`                    | Thrill Drive 2 (EBB) CF card                                 | `0426f4bb9001cf457f44e2c22e3d7575b8049aa3` |
| `thrild2/a41ebb_nvram.u39`              | Thrill Drive 2 (EBB) NVRAM (M48T58)                          | `e14ea2ba95b72edf0a3331ab82c192760bfdbce3` |
| `thrild2j/` or `thrild2a/` `a41a02.chd` | Thrill Drive 2 (JAA/AAA) CF card, shared: one copy is enough | `bbb71e23bddfa07dfa30b6565a35befd82b055b8` |
| `thrild2j/a41jaa_nvram.u39`             | Thrill Drive 2 (JAA) NVRAM                                   | `085f40816befde993069f56fdd5f8bd6ccfcf301` |
| `thrild2a/a41aaa_nvram.u39`             | Thrill Drive 2 (AAA) NVRAM                                   | `768bcd46a6ad20948f60f5e0ecd2f7b9c2901061` |
| `gticlub2/941b02.chd`                   | GTI Club 2 (JAB) CF card                                     | `943bc9b1ea7273a8382b94c8a75010dfe296df14` |
| `gticlub2/941jab_nvram.u39`             | GTI Club 2 (JAB) NVRAM                                       | `2753dda42cdd81af22dc6780678f1ddeb3c62013` |
| `gticlub2ea/941a02.chd`                 | GTI Club 2 (EAA) CF card                                     | `dd180ad92dd344b38f160e31833077e342cee38d` |
| `gticlub2ea/941eaa_nvram.u39`           | GTI Club 2 (EAA) NVRAM                                       | `92e0ce01049308f459985d466fbfcfac82f34a47` |

- **CHD hashes:** for a CHD, the hash is the CHD's internal SHA1, as shown by `chdman info`
  (or `build/tools/chdextract info`, which the build uses).
- **Checking your files:** `make check GAME=<id>` compares your files with this table. It also
  runs automatically before extraction.
- **Files you don't need:** `941a01.u25`, the other BIOS revision in `kviper`, is not used.
- **"Needs redump" is fine:** MAME flags `ds2430.u3` and the GTI Club 2 NVRAM dumps as "needs
  redump". This is expected.

## Build in detail

`make game` runs three steps, which you can also run one at a time:

```sh
make extract GAME=thrild2     # roms/thrild2/ -> work/thrild2/       (CF image, kernel, game modules)
make recomp  GAME=thrild2     # work/thrild2/ -> generated/thrild2/  (recompiled PowerPC code)
make -j8     GAME=thrild2     # generated/thrild2/ + runtime/ -> ./td2
```

- **After an update:** if `make -j8 GAME=<id>` stops with an error (for example a `GAME_...`
  constant that is not declared), run all three steps again with `make -j8 game GAME=<id>`.
  The files in `generated/<id>/` come from the profiles and the tools, so a new version can
  need them regenerated.
- **Several games:** each game builds into its own `work/<id>/`, `generated/<id>/` and
  `build/<id>/`, so you can build several side by side. `GAME` defaults to `thrild2`.
- **Sources and flags:** `tools/buildspec.py` lists them for the Makefile (it writes
  `build/make-vars.mk`) and for the release packages' launcher, so the two builds stay the same.
- **Profiles:** each version is described by `games/<id>/game.json`: the expected files, the
  modules to recompile, the input defaults and the calibration. Versions of the same game
  inherit the main profile. `make games` lists them.
- **Cleaning:** `make clean GAME=<id>` removes the build objects and the executable.
  `make distclean GAME=<id>` also removes `work/<id>/`, `generated/<id>/` and the saved NVRAM,
  which contain everything derived from that game's data.

## Repository layout

| Path              | Contents                                                                                                                                              |
| ----------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `games/`          | One profile per game (`game.json`): required files and SHA1s, modules, hints for hand-written code, inputs, calibration, enhanced-mode data           |
| `roms/`           | Where you put your own dumps (ignored by git)                                                                                                         |
| `tools/`          | Asset pipeline (CHD → FAT16 → game files → Konami LZSS / internal filesystem), game profiles, development tools (MAME debugger scripts, disassembler) |
| `recomp/`         | Static recompiler: PowerPC 603e decoder, control-flow analysis (including jump tables), C emitter                                                     |
| `runtime/`        | Runtime shared by every game: CPU context and exceptions, kernel task switching, event scheduler, Viper hardware, SDL frontend, enhanced mode         |
| `runtime/voodoo/` | Voodoo3 core from MAME, with a small compatibility layer                                                                                              |
| `docs/`           | Architecture and reverse-engineering notes, README screenshots                                                                                        |
| `dist/`           | Release packages: the launcher that builds the games on the player's computer (`viper-setup`), the packaging script                                   |
