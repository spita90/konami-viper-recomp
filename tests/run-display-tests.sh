#!/bin/sh
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
${CXX:-c++} -std=c++20 -Wall -Wextra -Werror -Iruntime tests/test_texture_filter.cpp -o "$TEMP_TEST_DIR/filter"
"$TEMP_TEST_DIR/filter"
