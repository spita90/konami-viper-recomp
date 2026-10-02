#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Wno-unused-function -Iruntime $(sdl2-config --cflags) tests/test_window_state.c $(sdl2-config --libs) -o "$TEST_DIR/test"
"$TEST_DIR/test" "$TEST_DIR/window"
