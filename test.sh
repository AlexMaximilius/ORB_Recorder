#!/bin/sh
# test.sh -- run the suite. Exits non-zero if anything fails.
#
# Same two targets as test.bat: the annotation rasteriser and the PNG writer.
# Both are pure computation, so they can be checked by a machine rather than
# by looking -- and both are where a silent regression hides, because a broken
# compressor still produces files that open perfectly.
#
# Needs only cc. Python and Pillow are optional and add two more decoders.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-cc}
FAILED=0

echo "=== paint.h ==================================================="
if $CC -O2 -std=c11 -Wall -Wextra -I"$HERE" -o "$HERE/tests/test_paint" \
        "$HERE/tests/test_paint.c" -lm; then
    "$HERE/tests/test_paint" || FAILED=1
else
    echo "  COMPILE FAILED"; FAILED=1
fi

echo
echo "=== png_write.h ==============================================="
if $CC -O2 -std=c11 -Wall -Wextra -I"$HERE" -o "$HERE/tests/test_png" \
        "$HERE/tests/test_png.c" -lm; then
    "$HERE/tests/test_png" || FAILED=1
else
    echo "  COMPILE FAILED"; FAILED=1
fi

echo
echo "=== independent decoders (zlib, PIL) =========================="
if command -v python3 >/dev/null 2>&1; then
    if $CC -O2 -std=c11 -Wall -Wextra -I"$HERE" -o "$HERE/tests/make_sample" \
            "$HERE/tests/make_sample.c" -lm; then
        python3 "$HERE/tests/verify_png.py" || FAILED=1
    else
        echo "  COMPILE FAILED"; FAILED=1
    fi
else
    echo "  skipped: python3 is not on PATH"
fi

echo
if [ "$FAILED" -eq 0 ]; then
    rm -f "$HERE"/tests/test_paint "$HERE"/tests/test_png "$HERE"/tests/make_sample
    echo "ALL TESTS PASSED"
    exit 0
fi
echo "TESTS FAILED"
exit 1
