/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Core types (Felucca's): tracks, voices, engines, parameters.
 * Tracks are synth parts, each with its own engine, preset, parameters, voices and 64-step pattern, sharing one
 * budget of NVOICE sounding voices (voice.c). WaveStorm has one part (NPART, NTRK 1) playing the STORM engine.
 * Sections: sizes, parameters, voices and engines, tracks and the song, system. */
#include <stdint.h>

/* ------------------------------------------------------------ sizes --- */
#define NVOICE 8                 /* voices per part, and the budget shared by all parts */
#define NPART 1                  /* one synth part */
#define NTRK NPART               /* tracks (the formats and the protocol count these): every track is a part */
#define NSTEP 64
#define HALF_FRAMES 128          /* I2S half buffer: 2.9 ms at 44.1 kHz (a key waits 0..1 half, then plays 1 half later) */
#ifndef FELUCCA_CPU2
#define FELUCCA_CPU2 1           /* the second core (src/felucca.c): 1 = built in, MENU > 2ND CORE ON lets it render
                                  * some parts; 0 = none of it */
#endif
#define C1_NP (FELUCCA_CPU2 ? NPART : 1)   /* the copies of the state a part's render writes that parts shared before the
                                            * second core (mod.c, voice.c SPREAD, eng_formant.c): one each with two cores,
                                            * one for all with one, as up to 1.6 */
#define C1_PI(t) ((uint32_t)((t) - trk) % C1_NP)   /* a part's copy */
#define NENGINES 1               /* STORM alone, engine 0 (the other engines' numbers are not defined: any use left of
                                  * them is a compile error, not a wrong index) */
#define ENGI_WAVESTORM 0u
#define NENG_SHOWN 1
#define UP_SLOTS 32u             /* user presets (upreset.c) */

/* ------------------------------------------------------- parameters --- */
/* the engine interface (Felucca-Engines, MIT: engines/, a git submodule): param_desc_t and F_*, voice_t,
 * vmod_t, preset_t with FX() / PAT(), engine_t, NELEM */
#include "../engines/src/felucca_engine.h"

enum {                          /* per-track parameters */
    P_LEVEL,
    P_ATK, P_DEC, P_SUS, P_REL,
    P_ED_FLT, P_ED_PIT, P_ED_SHP,
    P_LRATE, P_LWAVE, P_LPHASE, P_LFADE,
    P_LD_PIT, P_LD_FLT, P_LD_SHP, P_LD_AMP,
    P_AMODE, P_ARATE, P_AOCT, P_AGATE,
    P_ASWING, P_APROB, P_AHOLD, P_AORDER,
    P_ROOT, P_SCALE, P_QUANT, P_TRANS,
    P_SLEN, P_SDIV, P_SSWING, P_SGATE,
    P_DIST, P_CHOR, P_DLY, P_REV,
    P_VOICE, P_GLIDE, P_PAN, P_MUTE,
    P_GLMODE, P_PRIO, P_ALLOC, P_DETUNE,
    P_M1SRC, P_M1DST, P_M1AMT,                 /* modulation matrix (mod.c): 4 slots of SRC, DST, AMT; */
    P_M2SRC, P_M2DST, P_M2AMT,                 /* new common parameters go just before P_E0 (user presets */
    P_M3SRC, P_M3DST, P_M3AMT,                 /* and user presets map by count) */
    P_M4SRC, P_M4DST, P_M4AMT,
    P_CHRD, P_VOIC,                            /* chord keys (chord.c): one key plays a chord; its voicing */
    P_LSYNC, P_LTRIG, P_LPOL,                  /* 1.2 (#154): LFO SYNC (OFF, a note value), TRIG (NOTE, FREE), POL (BI, UNI) */
    P_SPRD,                                    /* (#148): SPREAD, the voices of a POLY / UNISON track around PAN (fx.c) */
    P_ITYPE, P_IA, P_IB, P_IC, P_IMIX,         /* 1.5 (#78, #177): the INSERT, its TYPE, three values and MIX (fx.c) */
    P_ESYNC,                                   /* 1.5 (#175): ENV SYNC: ATK DEC REL as note values of the tempo (voice.c) */
    P_ATSNS, P_ATDST, P_ATAMT,                 /* 1.8: AFTERTOUCH: the keys' pressure (keytouch.c) and its routing (mod.c) */
    P_FTYPE, P_CUT, P_RES, P_FKEY,             /* WaveStorm: the voice filter (eng_wavestorm.c ws_filter), EDIT > FILTER:
                                                * TYPE OFF LP BP HP, CUTOFF, RESONANCE, KEY tracking; ENV / LFO > FLT and
                                                * the matrix's CUT move CUTOFF */
    P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7,
    P_COUNT
};

enum {                          /* global parameters */
    G_BPM, G_SWING, G_CLOCK, G_TUNE,
    G_DTIME, G_DFDBK, G_DCOLOR, G_DMIX,
    G_RSIZE, G_RDAMP, G_CRATE, G_CDEPTH,
    G_MIDI, G_SYNC, G_ROUTE, G_INFO,   /* G_ROUTE: MIDI IN, the channel 0..15 the part hears, 16 OMNI (seq.c midi_slot) */
    G_ENGSEL, G_ENGGO,          /* no page: the editor switches the engine with a SET of G_ENGSEL; G_ENGGO is
                                 * unused (ids are fixed by the formats and the protocol) */
    G_CLRSEQ, G_INITSND,
    G_RTYPE,                    /* REVERB TYPE: 0 ROOM, 1 SPRING, 2 HALL (1.2, the power-on default) (fx.c). Was
                                 * G_DRCH, Felucca's GM drum part's MIDI channel (inert since 1.0) */
    G_DRLVL, G_DRREV,           /* inert (label "-", on no page): Felucca's GM drum part they set is long gone;
                                 * kept so the ids, which the editor protocol uses, do not move */
    G_COUNT
};

/* stored parameters of an older layout -> today's P_* order. A store keeps np = the P_COUNT it was
 * written with; common parameters are only ever added just before P_E0, so the first np - 8 are
 * P_LEVEL.. in order and the last 8 are P_E0..P_E7; the parameters a store does not have take def[]
 * (user presets of an older P_COUNT) */
static void params_by_count(int16_t *out, const int16_t *in, uint32_t np, const int16_t *def)
{
    uint32_t i, nc = np - 8u;
    for (i = 0; i < P_E0; i++)
        out[i] = i < nc ? in[i] : def[i];
    for (i = 0; i < 8u; i++)
        out[P_E0 + i] = in[nc + i];
}


/* ------------------------------------------------- voices, engines --- */
enum { V_POLY, V_MONO, V_LEGATO, V_UNISON };   /* P_VOICE */
/* voice_t, vmod_t, preset_t, engine_t: felucca_engine.h (above) */

/* ------------------------------------------------- tracks, the song --- */
enum { ST_NOTE, ST_TIE, ST_REST };
#define SF_ACCENT 1u
#define SF_SLIDE 2u
typedef struct {                 /* acid-style step: up to 4 notes (POLY), time, accent, slide */
    uint8_t note[4];
    uint8_t n;                   /* notes in use, 0 = empty */
    uint8_t time;                /* ST_NOTE / ST_TIE / ST_REST */
    uint8_t flags;               /* SF_ACCENT | SF_SLIDE */
    uint8_t vel;
} step_t;
#define MOTION_MAX 128u          /* 1.2 (FUN10): 64 before */
/* place: track << 6 | step (Felucca's four tracks x 64 steps fit one byte). Values retain their signed parameter range
 * (-64..127: an int8_t, 1.2). param: the P_* id (< 128: P_COUNT is at most 127), bit 7 (MOTION_LOCK, 1.1) a parameter lock: the value
 * sounds on that step only and goes back after it (motion.c motion_step); without it an automation event, the
 * value holds until another one changes it. One record per (track, step, id), of either kind */
#define MOTION_LOCK 0x80u
#define MOTION_ID(e) ((uint32_t)(e)->param & 0x7Fu)
typedef struct { uint8_t place, param; int8_t value; } motion_event_t;
typedef struct { uint8_t count, on, rsv[2]; motion_event_t event[MOTION_MAX]; } motion_store_t;
_Static_assert(sizeof(motion_store_t) == 388u, "motion disk layout (FUN10)");

typedef struct track {
    int16_t p[P_COUNT];
    uint8_t engine, preset;      /* engine: what the audio ISR renders */
    uint8_t eng_req;             /* engine the UI asked for (the ISR switches at a block start) */
    uint8_t user;                /* user preset slot + 1 the sound came from (UI), 0 = none */
    voice_t v[NVOICE];
    /* LFO */
    uint32_t lfo_ph;
    int32_t lfo_val;             /* Q15 */
    int32_t lfo_fade;            /* Q15 ramp after note-on */
    uint32_t lfo_rnd;
    int16_t lfo_prev;            /* the S&H value of the cycle before (mod.c SLEW glides from it to lfo_rnd's) */
    /* keyboard / arp input: held notes in press order */
    uint8_t held[16];
    uint8_t nheld;
    uint8_t arp_phys;            /* keys physically held for the arp */
    uint8_t latched;             /* HOLD: keep notes after release */
    /* arp runtime */
    uint32_t arp_pos;            /* q8 samples into the current arp step */
    uint32_t arp_idx;
    uint8_t arp_note;            /* sounding arp note, 0 = none */
    uint8_t arp_walk;            /* WALK: the place in the note list it stands on */
    uint8_t arp_ch[3];           /* CHORD: the other notes sounding with arp_note, 0 = none */
    uint32_t arp_off;            /* q8 sample time of its note-off */
    /* sequencer */
    step_t step[NSTEP];
    uint32_t seq_pos;            /* q8 samples into the current step */
    uint16_t seq_idx;
    uint8_t seq_notes[4];        /* sounding seq notes */
    uint8_t seq_n;
    uint8_t seq_hold;            /* last step slides: keep the notes until the next step */
    uint8_t slide_glide;         /* next legato note glides (slide) */
    uint32_t seq_off;
    uint8_t rskip_idx;           /* live recording put notes into the step about to play: */
    uint8_t rskip_n, rskip[4];   /* do not trigger them again there (they sound already) */
    /* the matrix's TIME (seq.c seq_play_on): a step plays late. Latched as a step starts: */
    uint32_t nd_late;            /* the step playing now plays at this seq_pos + 1 (0: it played; the matrix's TIME) */
    /* live recording of held notes (seq.c rec_hold): the steps they are held into become TIEs */
    uint8_t rh_n, rh_note[4];    /* recorded notes still held, 0 = none */
    uint8_t rh_start;            /* the step they were recorded into */
    uint8_t rh_ties;             /* TIE steps written after it */
    uint8_t rh_last;             /* the last of them; rh_bak: what it held (an early release puts it back) */
    step_t rh_bak;
    /* mono */
    uint8_t mono_stack[8];
    uint8_t nmono;
    uint8_t mono_note;           /* note the MONO / LEGATO / UNISON voice(s) play, 0 = none */
    uint8_t rr;                  /* POLY ROTATE: next voice to try */
    /* mix runtime */
    int32_t peak;
    int32_t dist_hp, dist_lp1, dist_lp2;   /* DIST insert state (fx.c) */
    uint8_t tail;                /* blocks to mix after the last voice (the DIST / INSERT tail) */
    uint8_t ins_run;             /* the INSERT still sounds (fx.c track_insert: its wet share above 0) */
#if FELUCCA_CPU2
    /* the second core: a part's render state, its own, so the two cores' parts share nothing (fx.c mix_block); in
     * the pad that keeps sizeof(track_t) a multiple of 16 (14 bytes before) */
    uint8_t lfo_wrap;            /* the LFO's phase wrapped this block: its S&H is drawn after the parts (voice.c) */
    uint32_t esync_k[3];         /* ATK's step, DEC's and REL's coefficient (Q16) this block (voice.c esync_coef) */
    uint8_t spread_on;           /* SPREAD this block (fx.c mix_part, voice.c track_render) */
    uint8_t rsv_sz[15];          /* (sizeof(track_t) a multiple of 16: below; the filter's 4 parameters took 8) */
#else
    uint8_t rsv_sz[13];          /* (sizeof(track_t) a multiple of 16: below; the filter's 4 parameters took 8) */
#endif
    int16_t armp, aholdp;        /* P_AMODE / P_AHOLD as last seen by the ISR */
    /* engine switch (voice.c engine_block): the old engine's voices fade out, then it switches */
    uint8_t xf_on, xf;           /* fading; blocks of the fade still to render */
    int16_t pe_old[8];           /* P_E0..P_E7 of the sounding engine: the fade renders with these */
    uint8_t xp_n, xp_note[4], xp_vel[4];   /* note-ons during the fade, played on the new engine */
    /* modulation matrix (mod.c): MIDI performance controllers of the track's channel, 0..127 */
    uint8_t mw, at;              /* CC1 mod wheel, channel aftertouch */
    uint8_t ex_off;              /* 127 - CC11 expression (0: full, the MIDI default) */
    uint8_t m_vel, m_key, m_vi;  /* the latest note-on: velocity, note, voice index (per-block destinations) */
    uint8_t sp_alt;              /* SPREAD: the side the next voice starts on (alternating, voice.c voice_start) */
    int16_t m_rnd;               /* .. its RAND */
    int32_t m_env;               /* the amp envelope of voice m_vi, last block (Q15) */
} track_t;
/* the audio ISR indexes trk[] with a multiply by a small immediate and a shift while the size is a multiple of 16; 1.5's
 * new track parameters (the INSERT, the filter TYPE, ENV SYNC) and state would make it no multiple (the constant
 * loaded first: +3 % on the ISR's static cost, target_budget.py): one pad, rsv_sz, keeps it */
_Static_assert(sizeof(track_t) % 16u == 0, "track_t: a multiple of 16 bytes (a cheap trk[] index on the target)");

typedef struct {
    int16_t g[G_COUNT];
    uint8_t playing, seq_mode;
    uint8_t grid;                /* the keys' mode (ui_name.c keys_mode): 0 notes, 2 NAME (no key plays); Felucca's 1
                                  * (its drum grid) and 3 (its SONG keys) are not used */
    uint8_t rec;                 /* live recording armed: bit per track */
    uint8_t sel;                 /* selected track 0..NTRK-1: keys, pages, editor */
    int8_t octave;
    uint32_t tick;               /* sub-blocks since play */
    uint32_t cpu_q8;             /* audio ISR load, 1/256 */
    uint32_t master_q12;
    int32_t batt_raw;            /* smoothed ADC ch3 (battery divider), 0 = not read yet */
} song_t;
#define ROUTE_OMNI 16                /* G_ROUTE: MIDI IN, the channel 0..15 the part hears, or every channel */

/* WaveStorm: one part (NTRK 1) that everything plays and loops over, in an array of four: code still sized for
 * Felucca's four tracks (the editor's track arguments, the GLO layer's opening copy) reads an idle, silent track
 * rather than past the array */
#define NTRK_ALLOC 4u
static track_t trk[NTRK_ALLOC];
static song_t song;
#define TSEL (&trk[song.sel])    /* the selected track */

/* SWING of a track's step clock: the track's own plus the global one, at most 100 (#31). The sequencer
 * (seq.c step_samples) times steps with it. At 100 the even
 * steps are 1.4 x the straight length and the odd ones 0.6 x. */
#define SWING_MAX 100
static inline int32_t track_swing(const track_t *t)
{
    int32_t sw = t->p[P_SSWING] + song.g[G_SWING];
    return sw < 0 ? 0 : sw > SWING_MAX ? SWING_MAX : sw;
}
/* the length of step idx of a straight length `base`, swung: even steps longer, odd ones shorter */
static inline uint32_t swing_step_len(const track_t *t, uint32_t base, uint32_t idx)
{
    int32_t sw = track_swing(t) * (int32_t)base / 250;
    return base + (uint32_t)((idx & 1u) ? -sw : sw);
}

/* ----------------------------------------------------------- system --- */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* slot store before the index update */
static volatile uint32_t fm1_ms;  /* milliseconds since boot (TIMER4-based, TIMER5 ISR in main.c) */
/* boot-loop guard (main.c): two boots in a row that die in the first 30 s -> UBOOT */
#define BOOTGUARD_MAGIC 0x42475244u
struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));
