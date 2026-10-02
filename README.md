# Konami Viper — static recompilation

Native PC ports of Konami's **Viper** arcade games: **Thrill Drive 2** and **GTI Club 2**
(_GTI Club: Corso Italiano_ / _Driving Party: Racing in Italy_).

The original PowerPC code is **statically recompiled** into C, so the game runs as native code
on your machine. A small shared runtime reimplements the arcade board: Voodoo3 graphics, sound,
I/O, CF card and timekeeper. This is not an emulator.

> **No game files are included.** You build the ports from your own dumps of the game and of
> the board BIOS (see [Required files](#required-files)).

<p align="center">
  <img src="docs/images/classic-thrilldrive2.png" width="49%" alt="Thrill Drive 2 racing across a bridge in London">
  <img src="docs/images/classic-gticlub2.png" width="49%" alt="GTI Club 2 attract mode in an Italian coastal town">
</p>

## Two ways to play

Every game can run in two modes, from the same executable.

### Classic: the arcade cabinet

```sh
./td2
```

The faithful arcade experience. The game behaves as on the original cabinet:

- insert coins and press START;
- attract mode, rankings and TEST MODE are all the original ones;
- the game runs at its original 30 fps, with the original sound.

On first launch, the program calibrates the steering wheel and pedals for
you, as an operator would.

### Enhanced: like a PC game

```sh
./td2 --enhanced
```

The same game, set up like a PC game: menus, options (even graphic improvement ones) and pause instead of coins and operator
menus.

<p align="center">
  <img src="docs/images/enhanced-td2-menu.png" width="32%" alt="Thrill Drive 2, enhanced mode at 16:9: attract menu with START GAME, OPTIONS, CREDITS, QUIT">
  <img src="docs/images/enhanced-td2-options.png" width="32%" alt="Thrill Drive 2, enhanced mode at 16:9: DISPLAY options with window mode, 2X resolution, 16:9 aspect ratio and fps counter">
  <img src="docs/images/enhanced-td2-pause.png" width="32%" alt="Thrill Drive 2, enhanced mode at 16:9: pause menu during a race in London">
  <br>
  <img src="docs/images/enhanced-gticlub2-menu.png" width="32%" alt="GTI Club 2, enhanced mode at 16:9: attract menu over a demo race">
  <img src="docs/images/enhanced-gticlub2-options.png" width="32%" alt="GTI Club 2, enhanced mode at 16:9: DISPLAY options">
  <img src="docs/images/enhanced-gticlub2-pause.png" width="32%" alt="GTI Club 2, enhanced mode at 16:9: pause menu during a race">
</p>
<p align="center"><sub>Thrill Drive 2 (top) and GTI Club 2 (bottom) in enhanced mode, 2X resolution, 16:9.</sub></p>

|                     | Classic                   | Enhanced                                                                                                 |
| ------------------- | ------------------------- | -------------------------------------------------------------------------------------------------------- |
| **Starting a game** | Insert coins, press START | START GAME from the menu                                                                                 |
| **Main screen**     | Attract mode              | Menu: START GAME, OPTIONS, CREDITS, QUIT over the attract mode                                           |
| **Options**         | None                      | OPTIONS: course difficulty, language, sound, window or fullscreen, resolution, aspect ratio, fps counter |
| **Pause**           | None                      | Esc: RESUME or MAIN MENU                                                                                 |
| **Ranking name**    | Chosen with the wheel     | Typed on the keyboard                                                                                    |
| **Resolution**      | 512×384, as the original  | 512×384 or 1024×768 (1X or 2X)                                                                           |
| **Aspect ratio**    | 4:3, as the original      | 4:3, 16:10, 16:9 or 21:9 (the 3D scene widens; the HUD stays in the centre)                              |
| **TEST MODE**       | F2                        | None (TEST, SERVICE and COIN are ignored)                                                                |
| **Save file**       | `td2_nvram.bin`           | `td2_enhanced_nvram.bin`, separate from the classic one                                                  |

More about the enhanced mode in [Enhanced mode in detail](#enhanced-mode-in-detail).

## Games

| Game                                      | Version      | `GAME=`      | Executable     | Status          |
| ----------------------------------------- | ------------ | ------------ | -------------- | --------------- |
| **Thrill Drive 2** (2001)                 | EBB (Europe) | `thrild2`    | `./td2`        | Playable        |
| Thrill Drive 2                            | JAA (Japan)  | `thrild2j`   | `./td2j`       | Boots and races |
| Thrill Drive 2                            | AAA (Asia)   | `thrild2a`   | `./td2a`       | Boots and races |
| **GTI Club: Corso Italiano** (2000)       | JAB (Japan)  | `gticlub2`   | `./gticlub2`   | Playable        |
| **Driving Party: Racing in Italy** (2000) | EAA (Europe) | `gticlub2ea` | `./gticlub2ea` | Boots and races |

"Boots and races" means tested with scripted inputs but not yet played by hand. Every version
supports both modes. The `GAME=` value is the game's MAME set name. Per-version notes are in
[Game notes](#game-notes).

## Build and run

Tested on macOS (Apple Silicon). Linux should work with the same steps but is untested.
Windows is not supported yet.

**1. Install the tools.** You need a C/C++ compiler, `make`, Python 3, SDL2 and `chdman` (from
MAME's tools).

On **macOS**, with [Homebrew](https://brew.sh) (the compiler and `make` come with Xcode's
command-line tools, `xcode-select --install`):

```sh
brew install sdl2 rom-tools
```

On **Linux** (Debian/Ubuntu, untested):

```sh
sudo apt install build-essential python3 libsdl2-dev mame-tools
```

**2. Add your game files.** Copy your dumps into `roms/`: the BIOS set into `roms/kviper/`, and
each game into the folder named after its MAME set, for example `roms/thrild2/`. Each folder has
a `README.txt` that lists the files it expects. Then check them:

```sh
make check GAME=thrild2
```

**3. Build.** One command extracts the game, recompiles it and builds the executable:

```sh
make -j8 game GAME=thrild2
```

This takes about a minute. It creates `./td2` (the executable for each game is in the
[Games](#games) table).

**4. Play.**

```sh
./td2              # classic
./td2 --enhanced   # enhanced
```

You can also start the game by double-clicking the executable, it will start in classic mode.

**On first launch** the game calibrates its controls automatically, before the
window opens. This takes a few seconds; for GTI Club 2 JAB and Thrill Drive 2 JAA and AAA it
takes a little longer, because of the force-feedback wheel test.

### Controls

| Action                                                 | Keyboard                                      | Gamepad              |
| ------------------------------------------------------ | --------------------------------------------- | -------------------- |
| Steer                                                  | ← / → or A / D                                | Left stick           |
| Accelerator                                            | ↑ or W                                        | R2 (or A)            |
| Brake                                                  | ↓ or S                                        | L2 (or B)            |
| Handbrake (GTI Club 2 JAB, Thrill Drive 2 JAA and AAA) | Space                                         | X                    |
| Shift up / down                                        | E / Q                                         | R1 / L1              |
| Insert coin (classic)                                  | 5                                             | Back                 |
| Start                                                  | 1                                             | Start                |
| Test / Service (classic)                               | F2 / 9                                        | —                    |
| Menus (enhanced)                                       | Arrows or WASD, Enter or 1, Backspace         | D-pad, A or Start, B |
| Pause (enhanced)                                       | Esc                                           | Guide                |
| Ranking name (enhanced)                                | Type it; Backspace, Enter ends; ← / → browse  | D-pad ← / →, then R2 |
| Fullscreen                                             | F11                                           | —                    |
| Quit                                                   | Esc (enhanced: Esc in the main menu, or QUIT) | Guide, as Esc        |

---

## Required files

The layout of `roms/` is the same as a MAME rompath. You only need `kviper/` plus the folders of
the games you want to build.

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

- **CHD hashes:** for a CHD, the hash is the CHD's internal SHA1, as shown by `chdman info`.
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
- **Profiles:** each version is described by `games/<id>/game.json`: the expected files, the
  modules to recompile, the input defaults and the calibration. Versions of the same game
  inherit the main profile. `make games` lists them.
- **Cleaning:** `make clean GAME=<id>` removes the build objects and the executable.
  `make distclean GAME=<id>` also removes `work/<id>/`, `generated/<id>/` and the saved NVRAM,
  which contain everything derived from that game's data.

## Saves and options

**Saves:** the calibration and the game's settings (volume, difficulty and so on) are saved to
`<executable>_nvram.bin`, next to the executable, for example `td2_nvram.bin`. To redo the
calibration, delete that file.

**Command-line options:**

```
./td2 --enhanced         enhanced mode
./td2 --scale 3          window scale (default 2, or the size of the last run)
./td2 --volume 8         audio gain (default 16)
./td2 --headless --seconds 30        no window/audio, runs as fast as possible (testing)
./td2 --frames DIR --frame-every 60  dump video frames as PPM (with --headless)
./td2 --help             all options
```

The environment variables for debugging are described in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Enhanced mode in detail

- **Menus:** drawn with each game's own font, in English or Italian. They follow the language
  chosen in the game; with Japanese they are in English.
- **Options**, on three pages. Left and right change a value.
  - **Game:** the difficulty of each course, and the language (English or Italian; Thrill
    Drive 2 JAA and AAA also offer Japanese).
  - **Sound:** attract sound, music in game, music and effects volume.
  - **Display:** window or fullscreen (F11 also switches it), the rendering resolution, and an
    fps counter. 2X renders the 3D scenes and the HUD at 1024×768. It needs about twice the CPU
    time, so be sure to check the fps counter on slower machines.
  - **Aspect ratio:** 4:3 (the original), 16:10, 16:9 or 21:9. A wider format shows more of the
    3D scene on the left and right, with the same vertical field of view; the window widens to
    match. The HUD and the 2D screens keep their 4:3 layout in the centre, and a few 2D effects
    (the noise on Thrill Drive 2's crash screen) cover only that part.
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
- **Separate saves:** the enhanced mode keeps its own NVRAM, `<executable>_enhanced_nvram.bin`,
  so it never changes the classic mode's settings. Delete that file to set it up again.
- **Fast boot:** the game boots at full speed, muted, behind a LOADING screen, until the
  attract mode starts.

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

## Documentation and repository layout

Technical details (boot chain, file formats, recompiler and runtime design, reverse-engineering
notes) are in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). The changes between versions are
listed in [CHANGELOG.md](CHANGELOG.md).

| Path              | Contents                                                                                                                                              |
| ----------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `games/`          | One profile per game (`game.json`): required files and SHA1s, modules, hints for hand-written code, inputs, calibration, enhanced-mode data           |
| `roms/`           | Where you put your own dumps (ignored by git)                                                                                                         |
| `tools/`          | Asset pipeline (CHD → FAT16 → game files → Konami LZSS / internal filesystem), game profiles, development tools (MAME debugger scripts, disassembler) |
| `recomp/`         | Static recompiler: PowerPC 603e decoder, control-flow analysis (including jump tables), C emitter                                                     |
| `runtime/`        | Runtime shared by every game: CPU context and exceptions, kernel task switching, event scheduler, Viper hardware, SDL frontend, enhanced mode         |
| `runtime/voodoo/` | Voodoo3 core from MAME, with a small compatibility layer                                                                                              |
| `docs/`           | Architecture and reverse-engineering notes, README screenshots                                                                                        |

## Legal

- This is an unofficial, non-commercial fan project for preservation and research. It is not
  affiliated with, endorsed by, or sponsored by Konami.
- "Konami", "Thrill Drive", "GTI Club" and "Driving Party" are trademarks of Konami Group
  Corporation. All other trademarks belong to their owners.
- This repository contains **no copyrighted game code, ROM, BIOS, CHD or NVRAM data**. The build
  extracts the game from files you supply (that must be dumped from your own hardware) and translates it locally. The screenshots in
  `docs/images/` were captured from the ports and are included only to illustrate the project.
- You may use this project only with dumps you are legally entitled to use, for example dumps
  of hardware you own. Do not ask for game files in this project's issues or discussions, and
  do not link to them.
- Do not redistribute anything the build produces from the game data: `work/`, `generated/`,
  the executables (`td2`, `td2j`, `gticlub2`, …) and the `*_nvram.bin` files.
- **License:** the original code in this repository is released under the
  [BSD-3-Clause license](LICENSE). The license covers only this project's code. It grants no
  rights to the games.
- **Third-party code:** `runtime/voodoo/` contains the 3dfx Voodoo emulation core, `poly.h` and
  `rgbutil` from [MAME](https://github.com/mamedev/mame).
  - This code is distributed under the BSD-3-Clause license, and its copyright belongs to its
    original authors (see the header of each file).
  - Some device logic in `runtime/hw.c` follows MAME's implementations: the M48T58 timekeeper
    (`timekpr.cpp`), the Konami K056230 LANC and `konami/viper.cpp`. That logic is also covered
    by BSD-3-Clause.
  - MAME is used here as a hardware reference.
- The software is provided "as is", without warranty of any kind.
