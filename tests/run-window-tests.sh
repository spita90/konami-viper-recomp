#!/bin/sh
# The saved window (runtime/window_state.h, runtime/enhanced.c). Tested:
# - fitting to the displays: a window on a second monitor (negative coordinates) stays there; on
#   a monitor that is gone it is centred on one that is connected; partly off screen it is
#   pulled inside; larger than the display it is shrunk to it
# - classic mode: no settings file, no window restored; the saved file holds only the window
#   (no enhanced-mode options); read back, the same window, negative coordinates included; the
#   same window again does not rewrite the file
# - out of range values are not restored: width under 320, x beyond +-1000000, height over 16384
# - enhanced mode: saving the window keeps fullscreen, show_fps, render_scale and aspect; read
#   back, both the options and the window
# Not covered (they need a real window): the window following the aspect ratio, the save 0.5 s
# after the last move or resize.
set -eu
cd "$(dirname "$0")/.."
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT HUP INT TERM
# game_config.h from a profile with an enhanced mode; no ROM files are needed
python3 - "$TEST_DIR" <<'EOF'
import sys
sys.path.insert(0, 'tools')
import game
game.write_config_header(game.load('gticlub2'), sys.argv[1])
EOF
case $(uname -s) in
    Darwin) DEAD_STRIP=-Wl,-dead_strip ;;
    *) DEAD_STRIP=-Wl,--gc-sections ;;
esac
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Wno-unused-function \
    -Wno-missing-field-initializers -ffunction-sections -fdata-sections -Iruntime -I"$TEST_DIR" \
    $(sdl2-config --cflags) tests/test_window_state.c $(sdl2-config --libs) -lm $DEAD_STRIP -o "$TEST_DIR/test"
"$TEST_DIR/test" "$TEST_DIR/settings.ini"
