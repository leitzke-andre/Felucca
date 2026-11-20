#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 André Leitzke Junior
"""WaveStorm's wave bank: band-limited single-cycle waves as int16_t mip tables (no float on the target).

  gen_wavestorm.py OUT.h           write the header (firmware/src/eng_wavestorm_waves.h)
  gen_wavestorm.py --check OUT.h   exit 1 if OUT.h is not what this script writes

Every wave is generated here from a formula (additive spectra, or a time-domain shape analysed with a DFT), so
the bank carries no third-party material. Each wave has WS_LEVELS (8) mip levels: level k keeps the harmonics up to
128 >> k (127 at level 0), so a note whose phase increment is below 2^(24 + k) has no harmonic above Nyquist.
Level k is 256 >> k samples long, at least 32 (WS_LVL_LEN): fewer harmonics need fewer samples for the same
samples per cycle of the top harmonic, and the bank is about a third of the size of 256-sample levels. Each level
has one guard sample (its first again, so an interpolated read never wraps). WS_LVL_SH[k] is the right shift that
turns a 32-bit phase into an index into level k (32 - log2 of its length).
All levels of a wave share one gain: equal loudness across the bank (RMS), capped so no sample clips.

It also writes WS_SPEED_Q8[128], RATE's speed factor (256 = 1x): 1/8x at 0, 1x at 64, ~8x at 127.
"""
import argparse
import cmath
import math
import random
import sys
from pathlib import Path

LEN = 256
LEVELS = 8
NHARM = 127                      # the most a 256-sample table holds below its own Nyquist
TARGET_RMS = 0.40 * 32767        # every wave aims for this RMS at level 0 ...
PEAK = 31000                     # ... unless its peak would pass this


def harm_sine(amps):
    """amplitudes of sine partials, harmonic 1.. (complex: magnitude, phase -90 deg)"""
    return {n: a * -1j for n, a in amps.items() if a}


def from_time(f, n=4096):
    """the harmonics 1..NHARM of a time-domain shape f(t), t in [0, 1): a plain DFT (n points)"""
    xs = [f(i / n) for i in range(n)]
    out = {}
    for h in range(1, NHARM + 1):
        acc = 0j
        w = -2j * math.pi * h / n
        for i, x in enumerate(xs):
            acc += x * cmath.exp(w * i)
        out[h] = acc * 2 / n
    return out


def formant(f0, forms):
    """a vowel: harmonics of f0 through Gaussian formant peaks (centre Hz, width Hz, gain), with a -6 dB/oct tilt"""
    amps = {}
    for n in range(1, NHARM + 1):
        f = n * f0
        g = sum(gain * math.exp(-((f - c) / w) ** 2) for c, w, gain in forms)
        amps[n] = (0.08 + g) / n ** 0.5
    return harm_sine(amps)


def digi():
    rnd = random.Random(1986)                       # fixed seed: the same wave every build
    amps = {n: rnd.random() / n ** 0.8 for n in range(1, 48)}
    return {n: a * cmath.exp(1j * rnd.uniform(0, 2 * math.pi)) for n, a in amps.items()}


def pulse(w):
    return from_time(lambda t: 1.0 if t < w else -1.0)


WAVES = [
    ("SINE", harm_sine({1: 1})),
    ("TRI", harm_sine({n: (-1) ** ((n - 1) // 2) / n ** 2 for n in range(1, NHARM + 1, 2)})),
    ("SAW", harm_sine({n: 1 / n for n in range(1, NHARM + 1)})),
    ("SQR", harm_sine({n: 1 / n for n in range(1, NHARM + 1, 2)})),
    ("PLS25", pulse(0.25)),
    ("PLS10", pulse(0.10)),
    ("ORGAN", harm_sine({1: 1, 2: 0.8, 3: 0.6, 4: 0.5, 6: 0.3, 8: 0.25, 10: 0.1, 12: 0.1})),
    ("HOLLW", harm_sine({1: 1, 3: 0.75, 5: 0.5, 7: 0.14, 9: 0.5, 11: 0.12, 13: 0.17, 15: 0.08})),
    ("GLASS", harm_sine({1: 1, 3: 0.2, 7: 0.55, 11: 0.4, 16: 0.3, 23: 0.25, 31: 0.15, 42: 0.1})),
    ("VOX-A", formant(130, [(730, 90, 1.0), (1090, 110, 0.6), (2440, 160, 0.35)])),
    ("VOX-O", formant(130, [(570, 80, 1.0), (840, 90, 0.7), (2410, 160, 0.2)])),
    ("VOX-E", formant(130, [(530, 80, 0.9), (1840, 120, 0.7), (2480, 160, 0.4)])),
    ("SYNC", from_time(lambda t: 2 * ((2.5 * t) % 1.0) - 1)),
    ("FOLD", from_time(lambda t: math.sin(3.2 * math.sin(2 * math.pi * t)))),
    ("BUZZ", harm_sine({n: 1.0 for n in range(1, NHARM + 1)})),
    ("DIGI", digi()),
]


def lvl_len(k):
    return max(32, LEN >> k)


def render(harm, hmax, n):
    """one cycle of n samples from the harmonics up to hmax (complex amplitude c: c * e^(i 2 pi n t), real part)"""
    out = []
    for i in range(n):
        t = i / n
        out.append(sum((c * cmath.exp(2j * math.pi * n * t)).real for n, c in harm.items() if n <= hmax))
    return out


def wave_levels(harm):
    levels = [render(harm, min(NHARM, 128 >> k), lvl_len(k)) for k in range(LEVELS)]
    base = levels[0]
    rms = math.sqrt(sum(x * x for x in base) / len(base))
    peak = max(max(abs(x) for x in lv) for lv in levels)
    gain = min(TARGET_RMS / rms, PEAK / peak)
    return [[int(round(x * gain)) for x in lv] for lv in levels]


def speed_q8():
    """RATE 0..127 -> speed in Q8: 2^((r - 64) * 3 / 64), 1/8x .. 1x .. ~7.7x"""
    return [int(round(256 * 2 ** ((r - 64) * 3 / 64))) for r in range(128)]


def header():
    offs, o = [], 0
    for k in range(LEVELS):
        offs.append(o)
        o += lvl_len(k) + 1
    wave_len = o
    L = ["/* SPDX-License-Identifier: MIT",
         " * Copyright (C) 2026 André Leitzke Junior */",
         "/* WaveStorm's wave bank and RATE table: generated by tools/gen_wavestorm.py, do not edit. */",
         "#ifndef ENG_WAVESTORM_WAVES_H",
         "#define ENG_WAVESTORM_WAVES_H",
         f"#define WS_LEVELS {LEVELS}            /* mip levels: level k keeps harmonics up to 128 >> k */",
         f"#define WS_NWAVES {len(WAVES)}",
         f"#define WS_WAVE_LEN {wave_len}        /* int16_t a wave: every level and its guard sample */",
         "static const char *const WS_WAVE_NAMES[WS_NWAVES + 1] = {" +
         ", ".join(f'"{n}"' for n, _ in WAVES) + ", 0};",
         "/* level k of a wave: its samples from WS_LVL_OFF[k], index = phase >> WS_LVL_SH[k] */",
         "static const uint16_t WS_LVL_OFF[WS_LEVELS] = {" + ", ".join(str(o) for o in offs) + "};",
         "static const uint8_t WS_LVL_SH[WS_LEVELS] = {" +
         ", ".join(str(32 - int(math.log2(lvl_len(k)))) for k in range(LEVELS)) + "};",
         f"static const int16_t WS_WAVES[WS_NWAVES][WS_WAVE_LEN] = {{"]
    for name, harm in WAVES:
        L.append(f"    {{   /* {name} */")
        for k, lv in enumerate(wave_levels(harm)):
            row = lv + [lv[0]]
            L.append(f"        /* level {k}: {len(lv)} samples + guard */")
            for i in range(0, len(row), 16):
                L.append("        " + ", ".join(str(v) for v in row[i:i + 16]) + ",")
        L.append("    },")
    L.append("};")
    sp = speed_q8()
    L.append("/* RATE -> speed, Q8 (256 = 1x) */")
    L.append("static const uint16_t WS_SPEED_Q8[128] = {")
    for i in range(0, 128, 16):
        L.append("    " + ", ".join(str(v) for v in sp[i:i + 16]) + ",")
    L.append("};")
    L.append("#endif")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    text = header()
    if a.check:
        ok = Path(a.out).exists() and Path(a.out).read_text() == text
        print(f"{'ok' if ok else 'FAIL'}: {a.out} {'matches' if ok else 'differs from'} tools/gen_wavestorm.py")
        sys.exit(0 if ok else 1)
    Path(a.out).write_text(text)
    size = len(WAVES) * sum(lvl_len(k) + 1 for k in range(LEVELS)) * 2
    print(f"{a.out}: {len(WAVES)} waves x {LEVELS} levels, {size} bytes of tables")


if __name__ == "__main__":
    main()
