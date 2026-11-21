# WaveStorm

This branch (`wavestorm-synth`) turns Felucca into **WaveStorm**: a playable vector synthesis + wave sequencing synth for
the M-VAVE FM-1. One sound engine (STORM), one part, its step sequencer and arpeggiator, Felucca's effects, modulation,
USB audio and MIDI, and user presets that keep the sound with its pattern. It was written by Claude as a reference
implementation of the WaveStorm design (the `research/` folder of the WaveStorm workspace), to compare against a
hand-written version later.

Everything else of Felucca's that a synth does not need is deleted from this branch, not switched off: the other 14
engines, samples and the SLICER, song chains, projects, drum lanes and the groovebox step extras (chance, conditions,
ratchets, nudge, the DETAIL page, SEQ TOOLS and EUCLID). The branch it grew from, `wavestorm-claude`, is Felucca with
STORM added as engine 15, all of that still in.

## What it does

Each voice plays four corner sources A, B, C, D, blended by an X/Y position:

```
 Y=0   A ─────── B        gA = (1-x)(1-y)   gB = x(1-y)
       │    •    │        gC = (1-x)y       gD = xy
 Y=1   C ─────── D        (the gains always sum to 1)
      X=0       X=1
```

- A corner is one of 48 band-limited single-cycle waves, or one of 10 built-in **wave sequences**, restarted on each
  note. A sequence is four **lanes**, as on the wavestate: TIME (each step's length and the crossfade into it), WAVE,
  PITCH and LEVEL. Each lane has its own length (1..16 steps) and loop point, so lanes of different lengths drift
  against each other; TIME's steps clock them all. Every step of every lane has a **probability**: one that misses its
  roll is skipped. LOOP, PING-PONG or ONE-SHOT; TIME in ms (5 ms .. 7.7 s, scaled by RATE) or, with SYNC, note values
  of the tempo or the MIDI clock (1/64 .. 2 bars).
- **PATH** is a vector envelope: a breakpoint path around the X/Y position, restarted on each note. Loops (X-SWP,
  Y-SWP, CIRC, FIG8, BOX, DIAM, WANDR) or one-shots (PLUCK: from the bright corner into the knob position, SWELL).
- **RATE** scales the speed of the path and of the wave sequences together: 1/8x at 0, 1x at 64, ~8x at 127.
- **The filter:** after the vector mix, every voice has a resonant state-variable filter (Felucca-Engines' SVF), EDIT >
  FILTER: TYPE (OFF, LP, BP, HP), CUT, RES and KEY tracking. Anything routed to **FLT / CUT** moves its cutoff (ENV DEST,
  LFO DEST, the matrix, aftertouch, a preset's envelope amount). OFF by default and in the pads (the vector is their
  tone), on in the basses, plucks, keys and leads.
- The modulation also steers the vector: anything routed to **SHP** moves X; the matrix's **X** and **Y** destinations
  move either (per block), and the PATH moves both per voice.
- **MIDI:** the part listens on one channel, MENU > MIDI > MIDI IN: CH1 (default) .. CH16, or OMNI (every channel);
  the keys' notes go out on it too. CC 74 sets the CUTOFF and CC 71 the RESONANCE (the standard sound controllers),
  CC 16 X and CC 17 Y, as well as Felucca's usual CCs (level, pan, envelope, sends). Felucca's channel blocks (CH1-4 ..), SEL, TRACK, T1..T4 CHANNEL and AUTO SELECT are gone with its tracks.

## Using it

| Button | Pages |
| --- | --- |
| EDIT | `VECT`: X, Y, PATH, RATE · `WAVE`: WAV A..D · `FILTER`: TYPE, CUT, RES, KEY · `PATH`, `PATH SET`, `SEQ`, `STEP`, `SEQ SET`: the sound's own path and sequences (below) · VOICE pages (poly/mono, glide, spread, aftertouch) |
| HOME | the home screen: the vector square (KNOB 1..4: X, Y, PATH, RATE); tapped again OUTPUT (LEVEL PAN SPREAD MUTE), then CLOCK |
| ALGORITHM (knob) | X, on every page (the top bar shows it as it turns; recorded as automation like KNOB 1 on HOME) |
| GLO | CLOCK (tempo, swing, clock source); held: the GLO layer (tap tempo, MIDI LEARN) |
| SEQ | STEP (the piano roll: notes, ties, rests, accent, slide; a step held and a knob turned: a parameter lock), PATTERN (LEN DIV SWING GATE), AUTOMATION (recorded knob moves, locks, per-step VEL) |
| SAVE | USER (save / load / erase / rename user presets), PRESETS, PHRASES (factory patterns), TOOLS (CLEAR PAT, INIT SOUND) |
| ARP, SCL, ENV, LFO, FX | as Felucca: the arpeggiator, scales and chords, envelopes, LFOs and the modulation matrix, effects (sends, the insert slot, the perform FX layer) |
| REC | records notes and knob moves into the pattern; held: CLICK, COUNT-IN, LEVEL |

- **The screen:** HOME and EDIT's VECT and WAVE pages show the vector as a joystick's field: a square (X left to
  right, Y top to bottom; A top left, B top right, C bottom left, D bottom right) with each corner's waveform beside
  it, drawn brighter the more of the mix that corner has (a wave sequence shows the step it plays). In the square:
  the X/Y knobs' position (a cross), the PATH's shape around it, and a dot where the sound is: peach while a note
  sounds, moving along the path with the modulation, on the cross otherwise. MENU > DISPLAY > HOME switches HOME to
  the oscilloscope (SCOPE) instead; Felucca's four-track TRACKS and LEVELS views are gone.
- **The look:** the MINT palette, after the FM-1's black-and-mint edition, is the default (MENU > COLOR has
  Felucca's others).
- **Presets (17):** STORM PAD, GLASS PAD, VOX PAD, DRIFT PAD, VECT PLUCK, GLASS KEYS, HIT KEYS, VECT BASS, FOLD BASS,
  VECT LEAD, VOX LEAD, SHIMMER, VOWELS, CYCLES, RHYTHM, ARP SEQ, DIGI SEQ. The power-on sound is STORM PAD.
- **User presets** (32 slots) store the whole part except its mix: the sound, the ARP, SCL and CHORD settings, all 64
  steps of the pattern (chords and velocities too) with LEN, DIV, SWING and GATE, and the sound's own path and
  sequences. Loading one brings all of that back (one undo, SAVE held, takes it back); LEVEL, PAN and MUTE stay as they
  are. A preset saved with no steps leaves the steps alone.
- **Autosave:** the part as it is (sound with its own path and sequences, all 64 steps, automation) and the global
  settings are put back at power-on
  (MENU > SYSTEM > RESTORE LAST, on by default). MENU > SYSTEM > CLEAR ALL starts over (user presets stay).
- **Waves:** SINE TRI SAW SQR PLS25 PLS10 ORGAN HOLLW GLASS VOX-A VOX-O VOX-E SYNC FOLD BUZZ DIGI, then SAW2 SQR2 PLS40
  PLS05 EVEN ODD3, the single harmonics H2 H3 H4 H5 H6 H8, VOX-I VOX-U CHOIR ORG2 ORG3 REED BRASS STRNG FLUTE CLAV BELL
  METAL SYNC2 SYNC3 FOLD2 PDSAW PDRES DIGI2 DIGI3 GRIT.
- **Sequences:** S-CYC (slow timbre cycle), S-ARP (octave/fifth steps), S-VOX (vowels), S-PWM (pulse widths,
  ping-pong), S-SHM (shimmer), S-RYT (rhythmic gates), S-DIG (fast digital hops), S-HIT (one-shot attack into a soft
  body), S-DRF (drifting lanes: 5 waves, 3 pitches, 4 levels, back in line every 60 steps), S-RND (sixteenths of the
  tempo whose waves, pitches and levels play by chance).

### Editing paths and sequences on the device

Every sound carries its own vector path (PATH **USER**, up to 8 points) and four lane sequences (**SEQ1**..**SEQ4**,
picked on WAV A..D like the built-in ones). A factory preset fills them with copies of what it plays: its PATH, and
its corners' sequences (a corner on a plain wave gives S-CYC, S-ARP, S-VOX or S-PWM), so there is always something to
start from. They are saved in user presets, the autosave and the undo.

| Page | KNOB 1 | KNOB 2 | KNOB 3 | KNOB 4 |
| --- | --- | --- | --- | --- |
| `PATH` | POINT 1..n | X (+-100 % around the X knob) | Y | TIME to reach it |
| `PATH SET` | POINTS 1..8 | LOOP: OFF (one-shot) or a point | FROM: a built-in path, copied in | |
| `SEQ` | SEQ 1..4 (the corners playing it beside it) | LANE: TIME WAVE PITCH LEVEL | LEN 1..16 | LOOP point |
| `STEP` | STEP | its value (a length or note value, a wave, semitones, a level) | PROB 0..100 % | XFADE (TIME) |
| `SEQ SET` | MODE: LOOP PING ONE | SYNC: OFF / ON | FROM: a built-in sequence, copied in | |

- The path pages draw the path in the vector square, the point being edited framed; editing the path puts PATH on
  USER, so what is edited is what plays.
- The sequence pages draw the four lanes as rows of 16 steps: a bar as high as the step's value and as bright as its
  chance, a tick at each lane's loop point, the edited step framed, and the steps a sounding note plays underlined.
- **Undo:** SAVE held puts back what the last edit on these pages changed (one knob's turn is one edit; the PATH it
  put on USER comes back too); held again, the edit again. FROM copies on every detent (turn through the built-ins and
  hear each), and the whole turn is one undo.

### The web editor

`web/wavestorm-editor.html` edits the same things from a computer: the vector square with the USER path's points to
drag around the X/Y cross, X, Y, PATH and the corners' waves, the points' X / Y / TIME, and SEQ1..SEQ4 as four lanes
of 16 steps to draw (values or chances), with each lane's LEN and LOOP, TIME's crossfades, MODE and SYNC. Edits on the
FM-1 show up in it as they happen, and its edits take the device's undo like the knobs' do. It is one file with no
build:

- with an FM-1 running WaveStorm: open it in Chrome or Edge (Web MIDI with SysEx; from `localhost` or https), USB
  cable in, **Connect FM-1 (USB)**;
- with the browser demo: `web/emu/build.sh` copies it beside the demo as `editor.html`; switch the demo on, open its
  editor link (or `http://localhost:8790/editor.html`) and **Connect emulator**. The demo relays the editor's SysEx to
  the firmware's own editor code (`editor.c`) through a BroadcastChannel between the two tabs.

It speaks Felucca's editor protocol plus three WaveStorm commands (84 WS_GET, 85 WS_SET, 86 WS_CHANGED:
`firmware/src/editor_ws.c`, `web/EDITOR_PROTOCOL.md`).

## How it is built

| File | What |
| --- | --- |
| `firmware/src/eng_wavestorm.c` | The STORM engine (MIT, Felucca-Engines' house rules): paths, sequences, the mixer, presets |
| `firmware/src/eng_wavestorm_waves.h` | The wave bank: 48 waves with their mip levels, written by `tools/gen_wavestorm.py` from formulas |
| `firmware/src/engines.c` | The engine table (STORM alone), its memory, the factory patterns, the power-on part |
| `firmware/src/ws_store.c` | WaveStorm's storage: settings, user presets, the autosave, CLEAR ALL |
| `firmware/src/ws_sound.c` | What a user preset keeps beside its record: its path, sequences and 64 steps, two slots in each of 16 flash objects at 0xA0000..0xBFFFF (Felucca's sample slots' sectors), mirrored in RAM |
| `firmware/src/ui_wsedit.c` | EDIT > PATH .. SEQ SET: the knobs and their undo; their cards in `ui_draw.c`, their pictures in `ui_graph.c` |
| `firmware/src/editor_ws.c`, `web/wavestorm-editor.html` | The editor commands for the path and the sequences, and the web editor |
| `firmware/src/core.h` | One part (`NPART` 1, in a `trk[]` of four as a safety net), STORM as engine 0, the 8-byte step |
| `firmware/src/params.c`, `upreset.c` | The pages (OUTPUT, CLOCK on GLO, PATTERN on SEQ), user presets with their pattern |
| `firmware/src/dsp.c` + `firmware/engines/` | Felucca-Engines' shared DSP: the tables and the SVF the voice filter uses |
| `tests/` | The WaveStorm tests, Felucca's tests of the platform kept as it is, the target cost check (below) |

- **Kept for what comes next.** The second core (`FELUCCA_CPU2`), the multi-part structure at `NPART` 1 (layers), the
  parameter locks (X/Y per step: a step held, the X / Y knob turned), the insert effect slot and the perform FX layer.
- **One part.** `NPART` and `NTRK` are 1, so the audio, the sequencer and the UI loop over one track. `trk[]` still has
  four entries: anything still sized by four tracks reads an idle, silent track rather than past the array. With one
  part, the part's mix is inlined into the audio ISR (no `mix_part` function in the binary). The ALGORITHM knob, which
  selects tracks in Felucca, turns X.
- **The step** is 8 bytes: up to four notes, NOTE / TIE / REST, accent and slide, velocity. The autosave format is
  versioned (6 now: the step, no project globals, MIDI IN a channel, the filter, the sound's own path and sequences);
  an older one is ignored.
- **The editor protocol** (`editor.c`) stays: firmware updates over USB only start when it has declined a frame, and
  the parameters, steps, motion, user presets, MIDI LEARN and the palette are still served; INFO names it WAVESTORM,
  and commands 84..86 carry the path and the sequences (`editor_ws.c`). A step travels as its 8 bytes; a longer one
  from a Felucca editor is taken with its extra bytes ignored. PROJECT, SONG, the sample and backup commands get no
  reply; the command numbers stay Felucca's.
- **User preset records** keep Felucca's layout (`upreset.c`) under a version of WaveStorm's own (6, or 7 for one whose
  pattern came in as a drum grid), so Felucca's records in the same sectors read as empty here and WaveStorm's read as
  empty in Felucca. The record keeps the first 16 steps; all 64 are beside it (`ws_sound.c`).
- **The engine.** Fixed point only, C99, no allocation, no library calls. Per block (32 samples): the path, the
  sequences, the X/Y smoothing, the four gains and each source's mip level. Per sample: one interpolated table read and
  a gain ramp per sounding source; sources silent for the whole block are skipped. Mip level k keeps harmonics up to
  128 >> k, picked while the phase increment is below 2^(24 + k): no harmonic passes Nyquist.

## Build, test, install

```sh
./build.sh                                   # build/wavestorm.fwsc (Docker on macOS); PYTHON= a Python with Pillow
sh tests/ws_tests.sh                         # every test (after ./build.sh)
python3 tools/gen_wavestorm.py firmware/src/eng_wavestorm_waves.h            # after changing the waves
sh web/emu/build.sh                          # the browser demo (Emscripten), after ./build.sh made build/gen
python3 -m http.server --bind 127.0.0.1 -d build/emu 8790   # then open http://localhost:8790/
```

The demo needs a secure context for its audio (AudioWorklet): open it at `localhost` or `127.0.0.1`. At the
`http://[::]:8790/` address Python prints without `--bind`, or at the Mac's network address, SWITCH ON fails with
"Cannot read properties of undefined (reading 'addModule')". Its computer keyboard plays it too: Z..M and Q..O the
keys, 1..0 the buttons FX SCL ENV LFO EDIT GLO HOME SAVE ARP SEQ, `-` PLAY, `=` REC, `[` `]` OCT.

`tests/ws_tests.sh` runs:

- the wave bank against its generator; the STORM engine under strict ASan/UBSan (presets, release, X/Y sweeps without
  clicks, all sources and paths, no aliasing on a rising saw, the filter's types and resonance, the lanes' drift,
  probabilities, ping-pong, one-shot and tempo sync);
- the whole firmware as the browser demo builds it, under ASan/UBSan: one part, no removed page, every family's button
  opens its pages, a note sounds and releases to silence, user presets with all 64 steps (from flash) and Felucca's
  records read as empty, the autosave and a damaged one ignored, the PATH / SEQ pages' edits and their undo, ALGORITHM
  on X, every HELP hint fitting its row, the editor commands through the demo's bridge (INFO, WS_GET / WS_SET, a
  device edit pushed), CC 16 / 17 / 71 / 74, the FILTER page and every TYPE, all 58 sources, a heavy pattern;
- Felucca's own tests of what WaveStorm keeps unchanged: keys and buttons, TRS MIDI (parser, UART DMA, TIMER5
  timestamps), USB audio and its descriptors (20 layouts), flash storage, M-UPGRADE, the update loader (needs the JieLi
  AC79 SDK), the package and the installer, the text renderer, palettes and font spacing (needs Pillow);
- the target cost of the render loops in the pi32v2 disassembly (`tests/target_budget.py` against
  `tests/target_budget.txt`, +10 %): `ws_render`, the effects, the audio and key-scan ISRs.

Felucca's other 70-odd tests (its UI test and everything built on it, the engines', songs', projects', drums') were
deleted with what they tested.

Install `build/wavestorm.fwsc` as any Felucca build (see the main README). The package identity is WaveStorm's own,
`FM-1_800` (a release `FM-1_8XY`; the stock firmware is `FM-1_0XY`, Felucca `FM-1_9XY`): Felucca's installers take any
`FM-1_` package and, after the install, check that the device reports the package's identity. The FM-1 shows up over
USB as "WaveStorm FM-1" (the installers look for "FM-1" in a port's name); its update loader stays Felucca's ("Felucca
Update", `ota-FM-1_800`). That the stock firmware's update entry takes an `FM-1_8XY` package as it takes Felucca's
`FM-1_9XY` is not tried on a unit yet. Test in the emulator first and have the recovery route ready before flashing a
unit.

## Measured

| | Felucca + STORM (`wavestorm-claude`) | WaveStorm (this branch) |
| --- | --- | --- |
| App image (slot 581,564 B) | 541,396 B | 308,808 B (about 273 KB free) |
| RAM `.data` + `.bss` (of 73,728 B) | 62,356 B | 34,468 B |
| Pool (of 368,640 B) | 326,236 B | 298,680 B |
| Wave bank | 16 waves, 19.7 KB | 48 waves, 59.1 KB |
| Audio ISR, target cost estimate | 4,196 (+ `mix_part` per part) | 3,939 (one part, inlined) |

The pool holds what the user presets keep beside their records (paths, sequences and 64 steps: a RAM mirror of 37 KB,
ws_sound.c) and the perform FX layer's
harmonizer buffer (32 KB) of its own: in Felucca it borrowed the SLICER's, and
with one part that borrowing read past the end of it.

## Not done yet

- **Not run on hardware.** CPU cost on the FM-1 is unmeasured (host figures: a STORM voice costs about as much as an
  ANALOG one); the worst case is 8 table reads a voice-sample.
- **The web editor on hardware:** tried against the browser demo only. The web installer (`web/index_pkg.html`, built
  into a site by `web/make_site.py` with the editor beside it) has no backup step: returning to the official firmware
  does not carry WaveStorm's sounds over.
- **Recording a path from the knobs**, per-corner tuning, layers.
- **Old Felucca data** on a unit is ignored, not converted: its user presets read as empty, its autosave
  is not WaveStorm's format (an autosave of an earlier WaveStorm build is ignored too: version 6 now), and MIDI LEARN
  maps made with Felucca point at other parameters (the numbers moved). User presets saved by WaveStorm builds before
  its own record version (none were released) read as empty too. Settings carry over.
