#!/bin/sh
# The bindings of runtime/wheel.c (wheels, pedals, every game device), without hardware. Tested:
# devices that share a product name (a Fanatec base is two), finding a binding's device again, the
# capture of keys, buttons, hats and axes, the values read, the file written and read back.
set -eu
cd "$(dirname "$0")/.."
TEMP_TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_TEST_DIR"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -Wno-unused-function \
    -Iruntime $(sdl2-config --cflags) tests/test_wheel.c $(sdl2-config --libs) -lm -o "$TEMP_TEST_DIR/wheel"
"$TEMP_TEST_DIR/wheel" "$TEMP_TEST_DIR"
