#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
TEMP_TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_TEST_DIR"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Iruntime tests/test_controller_gyro.c -lm -o "$TEMP_TEST_DIR/test"
"$TEMP_TEST_DIR/test"

# Generate constants from the public game profile; no ROM files are needed.
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
    tests/test_gyro_menu.c "$GC_SECTIONS" -lm -o "$TEMP_TEST_DIR/menu"
"$TEMP_TEST_DIR/menu" "$TEMP_TEST_DIR/settings.ini"

${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra \
    -Iruntime -I"$TEMP_TEST_DIR" $(sdl2-config --cflags) tests/test_frontend_gyro.c \
    $(sdl2-config --libs) -lm -o "$TEMP_TEST_DIR/frontend"
"$TEMP_TEST_DIR/frontend"
