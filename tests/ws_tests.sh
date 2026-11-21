#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 André Leitzke Junior
# WaveStorm's tests (FELUCCA_WS_ONLY, the default on this branch). Run from anywhere, after ./build.sh (build/gen):
#   sh tests/ws_tests.sh
# - the wave banks are what tools/gen_wavestorm.py writes (16 and 48 waves);
# - the STORM engine on Felucca-Engines' interface, strict ASan / UBSan (tests/wavestorm_test.c): presets, release,
#   X/Y sweeps without clicks, every source and path, aliasing render;
# - the whole WaveStorm firmware as the browser demo builds it (tests/ws_synth_test.c), with ASan / UBSan and the
#   exclusions Felucca's own whole-firmware runs use (its DSP shifts negative values on purpose);
# - with FELUCCA=1: Felucca's own suite in its configuration (FELUCCA_WS_ONLY=0), the shared code's regression check.
#   Its target cost checks read build/felucca.dis: build that firmware first (FELUCCA_WS_ONLY=0 ./build.sh), else they
#   report Felucca's render functions missing from the WaveStorm binary.
set -e
cd "$(dirname "$0")/.."
[ -f build/gen/felucca_tables.h ] || { echo "ws_tests: run ./build.sh once first (build/gen)"; exit 1; }
mkdir -p build/ws build/ws/wav
fail=0
run() { echo "== $1"; shift; "$@" || fail=1; }
INC="-Ibuild/gen -Ifirmware/src -Ifirmware/hal -Ifirmware/bsp/chip -Ifirmware/bsp/boards/fm1"

run "wave bank, 16 waves (beside Felucca's engines)" python3 tools/gen_wavestorm.py --check firmware/src/eng_wavestorm_waves.h
run "wave bank, 48 waves (WaveStorm)" python3 tools/gen_wavestorm.py --xl --check firmware/src/eng_wavestorm_waves_xl.h

python3 firmware/engines/tools/gen_engine_tables.py build/ws/fe_tables.h > /dev/null
${CC:-cc} -O1 -g -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
    -fsanitize=address,undefined -fno-sanitize-recover=all -Ibuild/ws -o build/ws/wavestorm_test tests/wavestorm_test.c
run "STORM engine (strict ASan/UBSan): presets, release, X/Y sweep, sources, paths, cost; WAVs in build/ws/wav" \
    build/ws/wavestorm_test build/ws/wav

${CC:-cc} -O1 -g -std=gnu99 -w -fsanitize=address,undefined \
    -fno-sanitize=signed-integer-overflow,shift,bounds,object-size,pointer-overflow -fno-sanitize-recover=undefined \
    -DFELUCCA_VERSION='"test"' $INC -o build/ws/ws_synth_test tests/ws_synth_test.c -lm
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
    run "WaveStorm firmware (ASan/UBSan): one part, pages, notes, user presets, autosave, MIDI CC, every source" \
    build/ws/ws_synth_test

if [ "${FELUCCA:-0}" = 1 ]; then
    echo "== Felucca's suite in its own configuration (FELUCCA_WS_ONLY=0)"
    CCC_OVERRIDE_OPTIONS="+-DFELUCCA_WS_ONLY=0" sh tests/run_tests.sh || fail=1
fi

if [ "$fail" = 0 ]; then echo "WAVESTORM TESTS PASSED"; else echo "WAVESTORM TESTS FAILED"; exit 1; fi
