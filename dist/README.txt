Konami Viper - static recompilation
===================================

Native PC ports of Thrill Drive 2 and GTI Club 2. https://github.com/spita90/konami-viper-recomp

This download holds no game code. The ports are built on your computer, from your own dumps of
the games: the first time it takes about a minute, then the games start at once.

1. Copy your files into the roms/ folder, in any layout: loose files or MAME's zips, in
   subfolders or not. roms/FILES.txt lists the files each game needs, with their SHA1:
     - the Viper BIOS: 941b01.u25 and ds2430.u3 (MAME set "kviper")
     - the game's CF card (.chd) and NVRAM (.u39)

2. Open a terminal in this folder.

   macOS only, once: this package is not signed by Apple, so macOS blocks the programs in it
   until you allow them. Type:

     xattr -dr com.apple.quarantine .

   Then run the setup:

     ./viper-setup

   It finds your files, checks them, and builds every game it can. Your files may also stay
   where they are: ./viper-setup --roms /path/to/your/roms
   ./viper-setup --list shows which games are found and which files are missing.

3. Play:

     ./td2              Thrill Drive 2, enhanced mode (menus and options, like a PC game)
     ./td2 --classic    the arcade cabinet, coins and TEST MODE included
     ./gticlub2         GTI Club 2 (and so on: the setup prints the names)

   Controls, options and multiplayer: see the README on the project's page.

After an update of this download, or on another computer, run ./viper-setup again: it rebuilds
the games.

SDL2: on Linux the games use your system's SDL2 (libSDL2-2.0.so.0) if there is one, otherwise
the copy in engine/sdl2/; on macOS always that copy.

Legal: an unofficial fan project, not affiliated with Konami. Use only dumps you are entitled to
use. Do not share the executables the setup makes (td2, gticlub2, ...): they contain the game.
The project's code is under the BSD-3-Clause license (LICENSE.txt); engine/ also holds zig
(MIT), SDL2 (zlib), libchdr, miniupnpc and libnatpmp (BSD), each with its license.
