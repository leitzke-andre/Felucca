# WaveStorm

This branch (`wavestorm-synth`) turns Felucca into **WaveStorm**: a playable vector synthesis + wave sequencing synth for
the M-VAVE FM-1. One sound engine (STORM), one part, its step sequencer and arpeggiator, Felucca's effects, modulation,
USB audio and MIDI, and user presets that keep the sound with its pattern. Felucca's other engines, song chains and
projects are left out. It was written by Claude as a reference implementation of the WaveStorm design (the `research/`
folder of the WaveStorm workspace), to compare against a hand-written version later.

The branch it grew from, `wavestorm-claude`, is Felucca with STORM added as engine 15; this branch still builds that
configuration with `FELUCCA_WS_ONLY=0`.

## What it does

Each voice plays four corner sources A, B, C, D, blended by an X/Y position:

```
 Y=0   A ─────── B        gA = (1-x)(1-y)   gB = x(1-y)
       │    •    │        gC = (1-x)y       gD = xy
 Y=1   C ─────── D        (the gains always sum to 1)
      X=0       X=1
```

- A corner is one of 48 band-limited single-cycle waves, or one of 8 built-in **wave sequences** (steps of wave,
  duration, level, pitch offset and crossfade; looping, ping-pong or one-shot, restarted on each note).
- **PATH** is a vector envelope: a breakpoint path around the X/Y position, restarted on each note. Loops (X-SWP,
  Y-SWP, CIRC, FIG8, BOX, DIAM, WANDR) or one-shots (PLUCK: from the bright corner into the knob position, SWELL).
- **RATE** scales the speed of the path and of the wave sequences together: 1/8x at 0, 1x at 64, ~8x at 127.
- The modulation steers the vector: anything routed to **SHP** moves X and anything routed to **FLT / CUT** moves Y
  (ENV DEST, LFO DEST, the matrix, aftertouch, and a preset's filter envelope amount). There is no filter after the
  mixer in this version; the vector is the tone control.
- **MIDI:** CC 71 sets X, CC 74 sets Y (channel 1), as well as Felucca's usual CCs (level, pan, envelope, sends).

## Using it

| Button | Pages |
| --- | --- |
| EDIT | `VECT`: X, Y, PATH, RATE · `WAVE`: WAV A..D · VOICE pages (poly/mono, glide, spread, aftertouch) |
| HOME | the home screen (KNOB 1..4: X, Y, PATH, RATE); tapped again OUTPUT (LEVEL PAN SPREAD MUTE), then CLOCK |
| GLO | CLOCK (tempo, swing, clock source); held: the GLO layer (tap tempo, MIDI LEARN, fill) |
| SAVE | USER (save / load / erase / rename user presets), PRESETS, PHRASES (factory patterns), TOOLS |
| SEQ, ARP, SCL, ENV, LFO, FX | as Felucca: the 64-step sequencer, the arpeggiator, scales and chords, envelopes, LFOs and the modulation matrix, effects |

- **Presets (17):** STORM PAD, GLASS PAD, VOX PAD, DRIFT PAD, VECT PLUCK, GLASS KEYS, HIT KEYS, VECT BASS, FOLD BASS,
  VECT LEAD, VOX LEAD, SHIMMER, VOWELS, CYCLES, RHYTHM, ARP SEQ, DIGI SEQ. The power-on sound is STORM PAD.
- **User presets** (32 slots) store the whole part except its mix: the sound, the ARP, SCL, CHORD and SLICER settings,
  and the first 16 steps of the pattern with LEN, DIV, SWING and GATE. Loading one brings all of that back (one undo,
  SAVE held, takes it back); LEVEL, PAN and MUTE stay as they are. A preset saved with no steps leaves the steps alone.
- **Autosave:** the part as it is (sound, all 64 steps, automation) and the global settings are put back at power-on
  (MENU > SYSTEM > RESTORE LAST, on by default), as Felucca does.
- **Waves:** SINE TRI SAW SQR PLS25 PLS10 ORGAN HOLLW GLASS VOX-A VOX-O VOX-E SYNC FOLD BUZZ DIGI, then SAW2 SQR2 PLS40
  PLS05 EVEN ODD3, the single harmonics H2 H3 H4 H5 H6 H8, VOX-I VOX-U CHOIR ORG2 ORG3 REED BRASS STRNG FLUTE CLAV BELL
  METAL SYNC2 SYNC3 FOLD2 PDSAW PDRES DIGI2 DIGI3 GRIT.
- **Sequences:** S-CYC (slow timbre cycle), S-ARP (octave/fifth steps), S-VOX (vowels), S-PWM (pulse widths,
  ping-pong), S-SHM (shimmer), S-RYT (rhythmic gates), S-DIG (fast digital hops), S-HIT (one-shot attack into a soft
  body).

## How it is built

The switch is `FELUCCA_WS_ONLY` (`firmware/src/core.h`, 1 by default on this branch).

| File | What |
| --- | --- |
| `firmware/src/eng_wavestorm.c` | The STORM engine (MIT, Felucca-Engines' house rules): paths, sequences, the mixer, presets |
| `firmware/src/eng_wavestorm_waves.h`, `_xl.h` | The wave banks: 16 waves (beside Felucca's engines), 48 (WaveStorm) |
| `tools/gen_wavestorm.py` | Writes both banks from formulas (no third-party material); the first 16 waves are identical |
| `firmware/src/ws_store.c` | WaveStorm's storage, instead of `project.c`'s body: settings, user presets, the autosave |
| `firmware/src/ws_compat.c` | What the shared code asks of the removed engines, answered as "not this one" |
| `firmware/src/core.h` | One part (`NPART` 1, in a `trk[]` of four as a safety net), STORM as engine 0 |
| `firmware/src/engines.c`, `params.c`, `upreset.c` | The engine table, the pages (OUTPUT, CLOCK on GLO; no SONG, PROJECT, MIXER), user presets with their pattern |
| other `firmware/src/*.c` | `#if FELUCCA_WS_ONLY` around what belongs to the removed engines, songs, projects, samples |
| `tests/wavestorm_test.c`, `tests/ws_synth_test.c`, `tests/ws_tests.sh` | The engine's tests, the whole firmware's, their runner |

- **The removed engines.** Their numbers (`ENGI_DRUM`, `ENGI_FM6`, ...) are defined past the end of `ENGINES[]`, so
  every `t->eng_req == ENGI_DRUM` in the shared code is false, and their `engine_t` objects are null pointers. Those
  numbers must never be used as an index: `tests/ws_synth_test.c` checks they are past the table, and a grep for
  `[ENGI_` found the one place that did (the favourites migration in `settings_persist.c`, now left out).
- **One part.** `NPART` and `NTRK` are 1, so the audio, the sequencer and the UI loop over one track. `trk[]` still has
  four entries: Felucca views that name four tracks are not offered, and if one is ever reached it reads an idle,
  silent track rather than past the array. The ALGORITHM knob, which selects tracks in Felucca, does nothing.
- **Kept on purpose.** The editor protocol (`editor.c`) stays: firmware updates over USB only start when it has
  declined a frame. Its sample, backup and FM6 commands are left out. `project.c` is still the file every includer
  names; in this build it includes `ws_store.c` instead of its own body.
- **The engine.** Fixed point only, C99, no allocation, no library calls. Per block (32 samples): the path, the
  sequences, the X/Y smoothing, the four gains and each source's mip level. Per sample: one interpolated table read and
  a gain ramp per sounding source; sources silent for the whole block are skipped. Mip level k keeps harmonics up to
  128 >> k, picked while the phase increment is below 2^(24 + k): no harmonic passes Nyquist.

## Build, test, install

```sh
./build.sh                                   # build/felucca.fwsc (Docker on macOS)
sh tests/ws_tests.sh                         # WaveStorm's tests (FELUCCA=1 adds Felucca's suite in its configuration)
python3 tools/gen_wavestorm.py firmware/src/eng_wavestorm_waves.h            # after changing the waves
python3 tools/gen_wavestorm.py --xl firmware/src/eng_wavestorm_waves_xl.h
sh web/emu/build.sh                          # the browser demo (Emscripten), after ./build.sh made build/gen
python3 -m http.server --bind 127.0.0.1 -d build/emu 8790   # then open http://localhost:8790/
```

The demo needs a secure context for its audio (AudioWorklet): open it at `localhost` or `127.0.0.1`. At the
`http://[::]:8790/` address Python prints without `--bind`, or at the Mac's network address, SWITCH ON fails with
"Cannot read properties of undefined (reading 'addModule')".

Install as any Felucca build (see the main README): the package is still identified as Felucca, so Felucca's installer
and the update entry take it. Test in the emulator first and have the recovery route ready before flashing a unit.

## Measured

| | Felucca + STORM (`wavestorm-claude`) | WaveStorm (this branch) |
| --- | --- | --- |
| App image (slot 581,564 B) | 541,396 B | 336,928 B (about 245 KB free) |
| RAM `.data` + `.bss` (of 73,728 B) | 62,356 B | 39,748 B |
| Pool (of 368,640 B) | 326,236 B | 236,112 B |
| Wave bank | 16 waves, 19.7 KB | 48 waves, 59.1 KB |

- `tests/ws_tests.sh` passes: both banks match the generator; the engine under strict ASan/UBSan (presets, release,
  X/Y sweeps without clicks, all sources and paths, no aliasing on a rising saw); the whole firmware under ASan/UBSan
  (one part, the pages, a note sounds and releases to silence, user presets with their pattern, the autosave and a
  damaged one ignored, CC 71 / 74, all 56 sources, the heavy STORM song).
- Felucca's own suite still passes in its configuration (`FELUCCA_WS_ONLY=0`): every host test, and its 61 target
  cost checks on a firmware built that way (`FELUCCA_WS_ONLY=0 ./build.sh`; those checks read the built binary, so with
  a WaveStorm build in `build/` they report Felucca's render functions missing, as they should).
- The browser demo boots into STORM PAD; HOME, OUTPUT, CLOCK, GLO and SAVE's four pages behave as above.

## Not done yet

- **Not run on hardware.** CPU cost on the FM-1 is unmeasured (host figures: a STORM voice costs about as much as an
  ANALOG one); the worst case is 8 table reads a voice-sample.
- **Identity:** the boot screen, ABOUT and the package still say Felucca.
- **The bundled web editor** (`web/editor.html`) is Felucca's: it does not know this build (one part, no projects).
- **On-device editing** of paths and sequences (the design's VECTOR PATH, CORNER and WAVESEQ pages), recording a path
  from the knobs, a filter after the mixer, tempo-synced sequence steps.
- **User presets** keep 16 steps of the 64 (the record format is Felucca's); the free ALGORITHM knob could steer X.
- **Old Felucca data** on a unit is ignored, not converted: its user presets (other engines) read as empty, its autosave
  is not WaveStorm's format. Settings carry over.
