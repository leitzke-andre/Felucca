#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 André Leitzke Junior
# WaveStorm's tests. Run from anywhere, after ./build.sh (build/gen, build/felucca.dis):
#   sh tests/ws_tests.sh
# - the wave bank is what tools/gen_wavestorm.py writes (48 waves);
# - the STORM engine on Felucca-Engines' interface, strict ASan / UBSan (tests/wavestorm_test.c): presets, release,
#   X/Y sweeps without clicks, every source and path, aliasing render;
# - the whole WaveStorm firmware as the browser demo builds it (tests/ws_synth_test.c), with ASan / UBSan and the
#   exclusions Felucca's own whole-firmware runs use (its DSP shifts negative values on purpose);
# - the platform WaveStorm keeps from Felucca, with Felucca's own tests of it: keys and buttons, TRS MIDI (parser,
#   UART DMA, TIMER5 timestamps), USB audio and descriptors, flash storage, the update path (M-UPGRADE, the
#   installer, the package; the loader only with a JieLi AC79 SDK in AC79_SDK), the text renderer and palettes;
# - the target cost of the render loops in the pi32v2 disassembly (tests/target_budget.py; BUDGET_UPDATE=1 keeps a
#   change).
# PYTHON names a Python with Pillow for the text spacing check (else it is skipped), as for ./build.sh.
set -e
cd "$(dirname "$0")/.."
[ -f build/gen/felucca_tables.h ] || { echo "ws_tests: run ./build.sh once first (build/gen)"; exit 1; }
mkdir -p build/ws build/ws/wav build/ws/host
fail=0
run() { echo "== $1"; shift; "$@" || fail=1; }
INC="-Ibuild/gen -Ifirmware/src -Ifirmware/hal -Ifirmware/bsp/chip -Ifirmware/bsp/boards/fm1"
HALI="-Ifirmware/hal -Ifirmware/bsp/chip -Ifirmware/bsp/boards/fm1 -Ifirmware/src"
CC="${CC:-cc}"
H=build/ws/host
PY="${PYTHON:-python3}"
SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"

run "wave bank: what tools/gen_wavestorm.py writes" python3 tools/gen_wavestorm.py --check firmware/src/eng_wavestorm_waves.h

python3 firmware/engines/tools/gen_engine_tables.py build/ws/fe_tables.h > /dev/null
$CC -O1 -g -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
    -fsanitize=address,undefined -fno-sanitize-recover=all -Ibuild/ws -o build/ws/wavestorm_test tests/wavestorm_test.c
run "STORM engine (strict ASan/UBSan): presets, release, X/Y sweep, sources, paths, cost; WAVs in build/ws/wav" \
    build/ws/wavestorm_test build/ws/wav

$CC -O1 -g -std=gnu99 -w -fsanitize=address,undefined \
    -fno-sanitize=signed-integer-overflow,shift,bounds,object-size,pointer-overflow -fno-sanitize-recover=undefined \
    -DFELUCCA_VERSION='"test"' $INC -o build/ws/ws_synth_test tests/ws_synth_test.c -lm
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
    run "WaveStorm firmware (ASan/UBSan): one part, pages, notes, user presets, autosave, MIDI CC, every source" \
    build/ws/ws_synth_test

# ---- the platform (Felucca's tests of the code WaveStorm keeps as it is)
PC="$CC -O1 -w $HALI -Ibuild/gen"
$PC -o $H/input_test tests/input_test.c
run "keys and buttons: debounce, glitches, encoders" $H/input_test
$PC -o $H/midi_uart_test tests/midi_uart_test.c
run "TRS MIDI parser" $H/midi_uart_test
$PC -o $H/uart_dma_test tests/uart_dma_test.c -lm
run "TRS UART: pending events, stale counts, exact packets and continuous traffic" $H/uart_dma_test
$PC -O2 -o $H/timer5_midi_test tests/timer5_midi_test.c
run "TRS timestamping during audio with saturated CC input" $H/timer5_midi_test
$PC -O2 -DFELUCCA_UART=0 -o $H/timer5_midi_nouart_test tests/timer5_midi_test.c
run "TIMER5 with TRS input disabled" $H/timer5_midi_nouart_test
HALF=$(sed -n 's/^#define HALF_FRAMES \([0-9]*\).*/\1/p' firmware/src/core.h)
for c in 1 0; do
    $PC -O2 -DT_CDC=$c -DHALF_FRAMES="$HALF" -o $H/uac_test tests/uac_test.c -lm
    run "USB audio input (CDC $c): descriptors, ring and packets, 44.1 -> 48 kHz resampler, 48 kHz stream" $H/uac_test
done
# USB descriptor layouts: CDC UAC LAYOUT CDC-presented 48K AS-first (Felucca #67; AS first 0: the AC collection of
# Felucca 1.0 .. 1.5, MIDI first)
for v in 1.1.0.1.1.1 1.1.1.1.1.1 1.1.2.1.1.1 1.1.3.1.1.1 1.1.0.0.1.1 1.1.2.0.1.1 0.1.0.1.1.1 1.1.0.1.0.1 1.1.0.0.0.1 \
         1.1.2.1.0.1 0.1.0.1.0.1 1.0.0.1.1.1 1.0.1.1.1.1 1.0.0.0.1.1 0.0.0.1.1.1 \
         1.1.0.1.1.0 1.1.0.0.1.0 1.1.0.1.0.0 1.1.0.0.0.0 0.1.0.1.1.0; do
    IFS=. read -r t_cdc t_uac t_lay t_on t_48 t_asf <<EOF
$v
EOF
    $PC -DT_CDC="$t_cdc" -DT_UAC="$t_uac" -DT_LAYOUT="$t_lay" -DT_ON="$t_on" -DT_48K="$t_48" -DT_ASF="$t_asf" \
        -o $H/usb_desc_test tests/usb_desc_test.c
    run "USB descriptors: CDC $t_cdc (presented $t_on), UAC $t_uac (48 kHz $t_48), layout $t_lay, AS first $t_asf" \
        $H/usb_desc_test
done
$PC -o $H/storage_test tests/storage_test.c
run "flash storage (A/B, torn writes)" $H/storage_test
$PC -DOWN_PKG=1 -o $H/ota_test tests/ota_test.c
run "M-UPGRADE entry (own loader)" $H/ota_test build/wavestorm.fwsc
if [ -d "$SDK" ]; then
    head -c 200000 build/felucca.bin > $H/old_app.bin
    AC79_SDK="$SDK" python3 tools/fm1pkg_make.py $H/old_app.bin build/loader/ota.bin $H/old.fwsc > /dev/null
    $PC -o $H/ldr_test tests/ldr_test.c
    run "update loader: other app -> this build" $H/ldr_test $H/old.fwsc build/wavestorm.fwsc
else
    echo "== update loader: skipped (no JieLi AC79 SDK in AC79_SDK)"
fi
run "package flash boundaries (synthetic SDK)" python3 tests/fm1pkg_test.py
if command -v node > /dev/null; then
    run "web installer: fm1pkg.js and fm1ota.js against a simulated FM-1" node web/test_install.mjs
else
    echo "== web installer: skipped (no node)"
fi
run "installer CLI (fm1_install.py) against a simulated FM-1" python3 tests/install_test.py
$PC -o $H/text_ref_test tests/text_ref_test.c -lm
run "text: pens, kerning and pixels equal the reference renderer" $H/text_ref_test
$PC -o $H/theme_test tests/theme_test.c -lm
run "themes: contrast, text blending and font metrics" $H/theme_test
if "$PY" -c "import PIL" 2> /dev/null; then
    run "text spacing of the fonts" "$PY" tests/text_spacing_test.py build/gen/ui_fonts.h
else
    echo "== text spacing: skipped (no Pillow in $PY; PYTHON names another)"
fi

# ---- the target's render loops
run "target budget: parser, cost and CLI fixtures" python3 tests/target_budget_test.py
run "target cost of the render loops (pi32v2 disassembly)" python3 tests/target_budget.py \
    build/felucca.dis tests/target_budget.txt

if [ "$fail" = 0 ]; then echo "WAVESTORM TESTS PASSED"; else echo "WAVESTORM TESTS FAILED"; exit 1; fi
