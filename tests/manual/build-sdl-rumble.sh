#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
output=${1:-/tmp/viper-sdl-rumble}
${CC:-cc} -std=c11 -Wall -Wextra -Werror tests/manual/sdl_rumble.c \
    $(sdl2-config --cflags --libs) -o "$output"
printf 'Built %s\n' "$output"
