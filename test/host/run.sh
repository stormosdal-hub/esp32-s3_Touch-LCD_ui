#!/bin/sh
# Host unit tests for hardware-independent code. Needs gcc and cJSON sources:
#   CJSON_DIR=/path/to/cJSON test/host/run.sh
set -e
cd "$(dirname "$0")"
M=../../main
: "${CJSON_DIR:?set CJSON_DIR to a cJSON checkout (https://github.com/DaveGamble/cJSON)}"
OUT="${TMPDIR:-/tmp}/lcd_ui_host_test"
gcc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined \
    -Istubs -I$M/core -I$M/io -I"$CJSON_DIR" \
    test_main.c $M/core/util.c $M/core/template.c $M/core/signals.c $M/io/io_codec.c "$CJSON_DIR/cJSON.c" -lm -o "$OUT"
"$OUT"
