#!/bin/sh
# The enhanced-mode Texture Filter option (runtime/enhanced.c). Tested:
# - the DISPLAY page has the TEXTURE FILTER row, ORIGINAL by default
# - Left/Right switch ORIGINAL <-> NEAREST, and each change reaches the renderer at once
#   (voodoo_set_texture_filter)
# - the choice is saved in the port settings and read back; a value other than 1 is ORIGINAL
# - classic mode always reports ORIGINAL, whatever the settings say
# Not covered here: the frames. ORIGINAL (and classic mode) are bit-identical to before, checked
# with headless runs (RT_RENDER_THREADS=0, --frames) against the previous build.
set -eu
cd "$(dirname "$0")/.."
TEMP_TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_TEST_DIR"' EXIT HUP INT TERM
python3 - "$TEMP_TEST_DIR" <<'PYCONFIG'
import sys
sys.path.insert(0, 'tools')
import game
game.write_config_header(game.load('gticlub2'), sys.argv[1])
PYCONFIG
case $(uname -s) in
    Darwin) GC_SECTIONS=-Wl,-dead_strip ;;
    *) GC_SECTIONS=-Wl,--gc-sections ;;
esac
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Wno-missing-field-initializers \
    -ffunction-sections -fdata-sections -Iruntime -I"$TEMP_TEST_DIR" \
    tests/test_display_menu.c "$GC_SECTIONS" -lm -o "$TEMP_TEST_DIR/menu"
"$TEMP_TEST_DIR/menu" "$TEMP_TEST_DIR/settings.ini"
