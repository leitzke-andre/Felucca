# WaveStorm (STORM engine)

This branch (`wavestorm-claude`) of Felucca adds **STORM**, a vector synthesis + wave sequencing engine, as engine 15.
It is a first, complete-but-minimal implementation of the WaveStorm design (see the `research/` folder of the
WaveStorm workspace), written by Claude as a reference to compare against a hand-written version later.

## What it does

Each voice plays four corner sources A, B, C, D, blended by an X/Y position:

```
 Y=0   A ─────── B        gA = (1-x)(1-y)   gB = x(1-y)
       │    •    │        gC = (1-x)y       gD = xy
 Y=1   C ─────── D        (the gains always sum to 1)
      X=0       X=1
```

- A corner is one of 16 band-limited single-cycle waves, or one of 8 built-in **wave sequences** (steps of wave,
  duration, level, pitch offset and crossfade; looping, ping-pong or one-shot, restarted on each note).
- **PATH** is a vector envelope: a breakpoint path around the X/Y position, restarted on each note. Loops (X-SWP,
  Y-SWP, CIRC, FIG8, BOX, DIAM, WANDR) or one-shots (PLUCK: from the bright corner into the knob position, SWELL).
- **RATE** scales the speed of the path and of the wave sequences together: 1/8x at 0, 1x at 64, ~8x at 127.
- Felucca's modulation steers the vector: anything routed to **SHP** moves X and anything routed to **FLT / CUT** moves
  Y (ENV DEST, LFO DEST, the matrix, aftertouch, and a preset's filter envelope amount). STORM has no filter of its
  own in this version; the vector is its tone control.

## Controls

| Page | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
| --- | --- | --- | --- | --- |
| EDIT 1 `VECT` | X | Y | PATH | RATE |
| EDIT 2 `WAVE` | WAV A | WAV B | WAV C | WAV D |
| HOME | X | Y | PATH | RATE |

Pick STORM with PRESETS' ENG knob (it comes after ADPCM in the list; like ADPCM it has no white key on the EDIT layer).

**Waves:** SINE TRI SAW SQR PLS25 PLS10 ORGAN HOLLW GLASS VOX-A VOX-O VOX-E SYNC FOLD BUZZ DIGI.
**Sequences:** S-CYC (slow timbre cycle), S-ARP (octave/fifth steps), S-VOX (vowels), S-PWM (pulse widths, ping-pong),
S-SHM (shimmer), S-RYT (rhythmic gates), S-DIG (fast digital hops), S-HIT (one-shot attack into a soft body).

**Presets (17):** STORM PAD, GLASS PAD, VOX PAD, DRIFT PAD, VECT PLUCK, GLASS KEYS, HIT KEYS, VECT BASS, FOLD BASS,
VECT LEAD, VOX LEAD, SHIMMER, VOWELS, CYCLES, RHYTHM, ARP SEQ, DIGI SEQ.

## Files

| File | What |
| --- | --- |
| `firmware/src/eng_wavestorm.c` | The engine (MIT): paths, sequences, the mixer, presets |
| `firmware/src/eng_wavestorm_waves.h` | The wave bank: 16 waves x 8 mip levels, generated, 19.7 KB |
| `tools/gen_wavestorm.py` | Writes the wave bank from formulas (no third-party material) |
| `tests/wavestorm_test.c` | Desktop test: renders, levels, clicks, coverage, cost; writes WAVs |
| `firmware/src/core.h`, `engines.c`, `ui_layer.c`, `category.c` | Registration as engine 15 (`FELUCCA_WAVESTORM`, on by default) |
| `firmware/src/favorites.c` | STORM's favourites: bytes 28..30 of SLICE's row (engines from 14 on have no row of their own) |
| `tests/ui_test.c`, `tests/run_tests.sh` | The engine list's expectations with STORM; the STORM checks in the suite |

## How it is built

- Fixed point only, C99, no allocation, no library calls; the house rules of Felucca-Engines, so the engine could
  move there (MIT).
- Per block (32 samples): the path, the sequences, the X/Y smoothing, the four gains and each source's mip level.
  Per sample: one linearly interpolated table read and a gain ramp per sounding source. Sources whose gain is zero for
  the whole block are skipped, so X or Y at an edge halves the cost.
- Mip levels: level k keeps harmonics up to 128 >> k and is chosen while the note's phase increment is below
  2^(24 + k), so no harmonic passes Nyquist. Higher levels use shorter tables (256, 128, 64, then 32 samples): the
  bank is 19.7 KB instead of 66 KB, which matters because stock Felucca leaves about 64 KB of the 568 KB app slot.
- State: X/Y smoothing and the path in the voice's `s[]`; corner phases and sequence positions in the part's engine
  memory (512 bytes a part, within `ENG_MEM_PART`).
- Engine 14 is ADPCM's number. In the default build (no ADPCM) it is held by a silent placeholder named `--` that is
  never offered, so engine numbers in presets and projects stay stable (`-` is DIGITAL's: the web editor reads a repeated
  name as an alias).

## Build, test, install

```sh
./build.sh                                   # build/felucca.fwsc (Docker on macOS)
tests/run_tests.sh                           # Felucca's suite, now with the STORM checks
cc -O2 -std=c99 -Ibuild/gen -DFE_TABLES_H='"felucca_tables.h"' -o build/wavestorm_test tests/wavestorm_test.c
build/wavestorm_test build/wavestorm_demo    # WAV renders of every preset, the X/Y sweep and an alias check
python3 tools/gen_wavestorm.py firmware/src/eng_wavestorm_waves.h   # after changing the waves
```

Install as any Felucca build (see the main README). Test in the emulator first and have the recovery route ready
before flashing a unit.

## Measured (desktop)

- Image: 541,396 B (stock Felucca 517,608 B): +23.8 KB, about 40 KB of the 581,564 B app slot left.
- RAM: unchanged (`.data + .bss` 62,356 B; the engine state is in the existing engine memory pool).
- No sanitizer findings (ASan + UBSan, strict); X/Y sweeps step no more than the waves themselves (no zipper noise);
  a rising saw from MIDI 60 to 126 shows no aliased (falling) partials.
- Cost on the host (tests/regress.c, one part, 8 notes): ~860 instructions a sample over the idle mix for the presets with
  static corners, 1,190 for STORM PAD (four crossfading sequences); ANALOG's presets are 730 to 1,180 on the same
  measure. 60.8 ns a sample for the heaviest preset (CYCLES) against ANALOG RAVE's 77.9 and FM6 PAD's 77.8.
- `tests/run_tests.sh` passes with STORM: its 17 golden renders, the same on one and two cores, every screen's text fits,
  the web editor's tables match the firmware's.
- Felucca's browser demo (`web/emu`) runs it: STORM is in PRESETS after DRUM, its HOME and EDIT pages show X Y PATH RATE
  and WAV A..D, and a held note sounds.

## Not done yet

- **Not run on hardware.** CPU cost on the FM-1 is unmeasured (the host figures above are only relative); the worst
  case is 8 table reads a voice-sample (four corners mid-crossfade).
- No custom editor pages: the paths and sequences are built in, not editable on the device (the design's VECTOR PATH,
  CORNER and WAVESEQ pages), and no recording of paths from the knobs.
- No filter after the mixer; no tempo-synced sequence steps; the bundled web editor (`web/editor.html`) does not know
  engine 15.
