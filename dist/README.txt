Konami Viper - static recompilation
===================================

Native PC ports of Thrill Drive 2 and GTI Club 2. https://github.com/spita90/konami-viper-recomp

This download holds no game code. The ports are built on your computer, from your own dumps of
the games: the first time it takes about a minute, then the games start at once.

1. Copy your files into the roms/ folder, in any layout: loose files or MAME's zips, in
   subfolders or not. roms/FILES.txt lists the files each game needs, with their SHA1:
     - the Viper BIOS: 941b01.u25 and ds2430.u3 (MAME set "kviper")
     - the game's CF card (.chd) and NVRAM (.u39)

2. Run the setup. It finds your files, checks them, and builds every game it can.

   Windows: double-click viper-setup.exe. The program is not signed, so the first time Windows
   may show "Windows protected your PC": click "More info", then "Run anyway".

   Linux and macOS: open a terminal in this folder. On macOS only, once (this package is not
   signed by Apple, and macOS blocks its programs until then):

     xattr -dr com.apple.quarantine .

   Then:

     ./viper-setup

   From a terminal your files may also stay where they are: viper-setup --roms /path/to/roms
   viper-setup --list shows which games are found and which files are missing.

3. Play: td2 (Thrill Drive 2), gticlub2 (GTI Club 2), and so on: the setup prints the names.

   Windows: double-click td2.exe. Linux and macOS: ./td2 in the terminal.
   That is the enhanced mode (menus and options, like a PC game). For the arcade cabinet, with
   coins and TEST MODE, add --classic: td2.exe --classic, or ./td2 --classic.

   Controls, options and multiplayer: see the README on the project's page.

After an update of this download, or on another computer, run the setup again: it rebuilds the
games.

SDL2: on Linux the games use your system's SDL2 (libSDL2-2.0.so.0) if there is one, otherwise
the copy in engine/sdl2/; on macOS that copy; on Windows SDL2.dll, next to the games.

Legal: an unofficial fan project, not affiliated with Konami. Use only dumps you are entitled to
use. Do not share the executables the setup makes (td2, gticlub2, ...): they contain the game.
The project's code is under the BSD-3-Clause license (LICENSE.txt); engine/ also holds zig
(MIT), SDL2 (zlib), libchdr, miniupnpc and libnatpmp (BSD), each with its license.
