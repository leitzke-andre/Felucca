/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 André Leitzke Junior */
/* WAVESTORM: vector synthesis with wave sequencing.
 *
 * Four corner sources A B C D sit on a square; an X/Y position sets their gains (bilinear: they always sum to 1)
 *   gA = (1-x)(1-y)   gB = x(1-y)   gC = (1-x)y   gD = xy
 * so X fades A->B (and C->D) and Y fades the top pair into the bottom pair.
 *
 * A corner (WAV A..D) is a single-cycle wave from the band-limited bank (tools/gen_wavestorm.py: 48 waves,
 * eng_wavestorm_waves.h) or one of the wave sequences (S-CYC .. S-RND, after the waves), restarted with each note. A
 * sequence is four lanes, as the wavestate's: TIME (each step's length and the crossfade into it), WAVE, PITCH and
 * LEVEL, each with its own length (1..16 steps) and loop point, so lanes of different lengths drift against each other.
 * TIME's steps clock the sequence: at each one every lane moves on a step. Every step of every lane has a chance; one
 * that misses its roll is skipped (the lane moves on to the next). LOOP, PING-PONG or ONE-SHOT, for every lane; TIME
 * in ms (x RATE) or, with SYNC, note values of the tempo (beat_samples, the host's).
 *
 * The position: X and Y (the knobs) are the centre; PATH adds a vector envelope, a breakpoint path restarted with
 * each note (loops, or one-shots such as PLUCK and SWELL); RATE scales the speed of the path and of the wave
 * sequences together (1/8x .. 1x at 64 .. ~8x; a synced sequence follows the tempo only). The host's SHP modulation
 * moves X; its FLT modulation (ENV, LFO, the matrix's CUT, a preset's fenv) moves the voice filter's cutoff (ws_filter,
 * the host's P_FTYPE P_CUT P_RES P_FKEY); the matrix moves X and Y as engine parameters.
 *
 * Cost: everything but the table reads runs once per block (CTL samples): the path, the sequences, the gains, the
 * mip level. A sample reads one interpolated table per sounding source ("tap"): 4 for four static corners, up to 8
 * while all four corners crossfade, and fewer when X or Y sit at an edge (a tap whose gain is 0 over the whole block
 * is skipped). The gains ramp linearly across the block (no zipper noise); X and Y also pass a one-pole smoother.
 *
 * State: s[0] s[1] the smoothed X and Y (Q15), s[2] the path's segment (from | to << 8), s[3] the time in it (ms
 * Q10), s[4] 1 once X and Y are set (0: the first block jumps to the target); per corner the phases and the
 * sequence's step in the part's engine memory (ws_part_t; the host's ENG_MEM, or an array of the engine's own). */
#include "eng_wavestorm_waves.h"      /* 48 waves (tools/gen_wavestorm.py) */

#define WS_NSEQ 10
#define WS_NSRC (WS_NWAVES + WS_NSEQ)
typedef char ws_bank_has_16[WS_NWAVES >= 16 ? 1 : -1];   /* WW_* below: the first 16, in every bank */
static const char *const N_WS_SRC[WS_NSRC + 1] = {
    WS_WAVE_NAME_LIST, "S-CYC", "S-ARP", "S-VOX", "S-PWM", "S-SHM", "S-RYT", "S-DIG", "S-HIT", "S-DRF", "S-RND", 0};
enum { WW_SINE, WW_TRI, WW_SAW, WW_SQR, WW_PLS25, WW_PLS10, WW_ORGAN, WW_HOLLW, WW_GLASS, WW_VOXA, WW_VOXO, WW_VOXE,
       WW_SYNC, WW_FOLD, WW_BUZZ, WW_DIGI };

/* ------------------------------------------------------------------ the rate --- */
/* ms a block, Q16 (0.7256 ms at 44.1 kHz): time advances by it x RATE's speed */
#define WS_MS_BLK_Q16 ((uint32_t)(((uint64_t)CTL * 1000u * 65536u + FS / 2u) / FS))
#define WS_XF_MIN 3              /* ms: the shortest crossfade between two steps (a hard switch clicks) */

/* ------------------------------------------------------------- vector paths --- */
/* a breakpoint: an offset from the X/Y centre (Q15: 32767 is the whole square) and the ms it takes to get there
 * from the point before (point 0's: from the last point, when the path loops back to it) */
typedef struct { int16_t x, y; uint16_t ms; } ws_pt_t;
typedef struct { const ws_pt_t *pt; uint8_t n, loop; } ws_path_t;   /* loop: the point after the last, 0xFF = stop */
#define WS_R 15000               /* the loops' radius: just under half the square */
#define WS_D 10607               /* WS_R / sqrt(2) */
static const ws_pt_t WP_XSWP[] = {{-WS_R, 0, 1000}, {WS_R, 0, 1000}};
static const ws_pt_t WP_YSWP[] = {{0, -WS_R, 1000}, {0, WS_R, 1000}};
static const ws_pt_t WP_CIRC[] = {{WS_R, 0, 250}, {WS_D, WS_D, 250}, {0, WS_R, 250}, {-WS_D, WS_D, 250},
                                  {-WS_R, 0, 250}, {-WS_D, -WS_D, 250}, {0, -WS_R, 250}, {WS_D, -WS_D, 250}};
static const ws_pt_t WP_FIG8[] = {{0, 0, 300}, {WS_D, WS_R, 300}, {WS_R, 0, 300}, {WS_D, -WS_R, 300},
                                  {0, 0, 300}, {-WS_D, WS_R, 300}, {-WS_R, 0, 300}, {-WS_D, -WS_R, 300}};
static const ws_pt_t WP_BOX[] = {{-WS_R, -WS_R, 500}, {WS_R, -WS_R, 500}, {WS_R, WS_R, 500}, {-WS_R, WS_R, 500}};
static const ws_pt_t WP_DIAM[] = {{0, -WS_R, 500}, {WS_R, 0, 500}, {0, WS_R, 500}, {-WS_R, 0, 500}};
static const ws_pt_t WP_WANDR[] = {{-4000, 9000, 700}, {11000, 3000, 500}, {6000, -12000, 900}, {-9000, -6000, 600},
                                   {-14000, 7000, 800}, {2000, 14000, 400}, {13000, -2000, 1000}, {-1000, -9000, 700}};
static const ws_pt_t WP_PLUCK[] = {{16384, 16384, 0}, {0, 0, 350}};
static const ws_pt_t WP_SWELL[] = {{-16384, -16384, 0}, {0, 0, 2500}};
static const char *const N_WS_PATH[] = {"OFF", "X-SWP", "Y-SWP", "CIRC", "FIG8", "BOX", "DIAM", "WANDR", "PLUCK",
                                        "SWELL", 0};
static const ws_path_t WS_PATHS[] = {
    {0, 0, 0},
    {WP_XSWP, NELEM(WP_XSWP), 0}, {WP_YSWP, NELEM(WP_YSWP), 0}, {WP_CIRC, NELEM(WP_CIRC), 0},
    {WP_FIG8, NELEM(WP_FIG8), 0}, {WP_BOX, NELEM(WP_BOX), 0}, {WP_DIAM, NELEM(WP_DIAM), 0},
    {WP_WANDR, NELEM(WP_WANDR), 0}, {WP_PLUCK, NELEM(WP_PLUCK), 0xFF}, {WP_SWELL, NELEM(WP_SWELL), 0xFF},
};
#define WS_NPATH ((int32_t)NELEM(WS_PATHS))

/* ----------------------------------------------------------- wave sequences --- */
#define WS_LSTEPS 16
enum { WL_TIME, WL_WAVE, WL_PITCH, WL_LEVEL, WL_N };
typedef struct {
    uint8_t len, loop;           /* 1..WS_LSTEPS steps; the step LOOP (and PING-PONG) goes back to, 0..len - 1 */
    uint8_t v[WS_LSTEPS];        /* TIME: a time code (WS_TIME_MS; SYNC: WS_SYNC_Q[v >> 3]), WAVE: a wave of the bank,
                                  * PITCH: 64 + semitones (40..88: two octaves each way), LEVEL: 0..127 */
    uint8_t miss[WS_LSTEPS];     /* the chance the step is skipped, 0..100 % (0: it always plays; the PROB the UI shows
                                  * is 100 - miss) */
} ws_lane_t;
enum { WS_ONESHOT, WS_LOOP, WS_PINGPONG };
typedef struct {
    ws_lane_t lane[WL_N];
    uint8_t xf[WS_LSTEPS];       /* TIME's steps: the crossfade into the step, % of its length */
    uint8_t mode;                /* WS_LOOP, WS_PINGPONG or WS_ONESHOT, every lane */
    uint8_t sync;                /* 0: TIME in ms, times RATE; 1: note values of the tempo */
} ws_lseq_t;
/* SYNC's note values, in 1/96 of a beat (a quarter): 1/64 1/32T 1/32 1/16T 1/16 1/8T 1/16D 1/8 1/4T 1/8D 1/4 1/4D
 * 1/2 1/2D 1 bar 2 bars; TIME's 0..127 spread over them (v >> 3) */
static const uint16_t WS_SYNC_Q[16] = {6, 8, 12, 16, 24, 32, 36, 48, 64, 72, 96, 144, 192, 288, 384, 768};
static uint32_t beat_samples(void);   /* the host's: a quarter at the tempo (or the MIDI clock's), samples */
#define WS_Q10_SMP_Q8 ((1024000u * 256u + FS / 2u) / FS)   /* ms Q10 a sample, Q8 (5944 at 44.1 kHz) */

#define WL(len, loop, ...) {(len), (loop), {__VA_ARGS__}, {0}}
#define L4(a) {a, a, a, a}
#define P_ 64                    /* PITCH: no offset */
static const ws_lseq_t WS_SEQS[WS_NSEQ] = {
    {   /* S-CYC: a slow timbre cycle */
        {WL(4, 0, 83, 83, 83, 83), WL(4, 0, WW_SAW, WW_SQR, WW_HOLLW, WW_TRI), WL(4, 0, P_, P_, P_, P_),
         WL(4, 0, 127, 127, 127, 127)}, L4(67), WS_LOOP, 0},
    {   /* S-ARP: octave and fifth steps, short crossfades */
        {WL(4, 0, 59, 59, 59, 59), WL(4, 0, WW_SAW, WW_SAW, WW_SQR, WW_SAW), WL(4, 0, P_, P_ + 12, P_ + 7, P_ + 19),
         WL(4, 0, 127, 100, 105, 90)}, {0}, WS_LOOP, 0},
    {   /* S-VOX: vowels, a-o-e-o */
        {WL(4, 0, 80, 80, 80, 80), WL(4, 0, WW_VOXA, WW_VOXO, WW_VOXE, WW_VOXO), WL(4, 0, P_, P_, P_, P_),
         WL(4, 0, 127, 127, 127, 127)}, L4(70), WS_LOOP, 0},
    {   /* S-PWM: the pulse widths, back and forth */
        {WL(3, 0, 68, 68, 68), WL(3, 0, WW_SQR, WW_PLS25, WW_PLS10), WL(3, 0, P_, P_, P_), WL(3, 0, 127, 127, 127)},
        {96, 96, 96}, WS_PINGPONG, 0},
    {   /* S-SHM: shimmer, bright partials an octave up, slowly */
        {WL(4, 0, 90, 90, 90, 90), WL(4, 0, WW_GLASS, WW_ORGAN, WW_GLASS, WW_SINE), WL(4, 0, P_, P_ + 12, P_ + 12, P_),
         WL(4, 0, 127, 95, 85, 127)}, L4(78), WS_LOOP, 0},
    {   /* S-RYT: rhythm, accents and gaps on sixteenth-ish steps */
        {WL(8, 0, 56, 56, 56, 56, 56, 56, 56, 56), WL(8, 0, WW_SYNC, WW_SAW, WW_SQR, WW_SAW, WW_SYNC, WW_SAW, WW_DIGI, WW_SAW),
         WL(8, 0, P_, P_, P_ + 12, P_, P_, P_ + 7, P_, P_), WL(8, 0, 127, 45, 100, 0, 115, 60, 105, 0)}, {0}, WS_LOOP, 0},
    {   /* S-DIG: digital chatter, fast hops between waves */
        {WL(6, 0, 43, 43, 43, 43, 43, 43), WL(6, 0, WW_DIGI, WW_FOLD, WW_BUZZ, WW_SYNC, WW_PLS10, WW_GLASS),
         WL(6, 0, P_, P_, P_, P_ + 12, P_, P_ + 24), WL(6, 0, 127, 127, 100, 115, 115, 90)}, {0}, WS_LOOP, 0},
    {   /* S-HIT: a one-shot attack, a bright click into a soft body that stays */
        {WL(4, 0, 28, 43, 59, 92), WL(4, 0, WW_BUZZ, WW_SYNC, WW_SAW, WW_TRI), WL(4, 0, P_ + 12, P_, P_, P_),
         WL(4, 0, 127, 127, 127, 127)}, {0, 33, 80, 30}, WS_ONESHOT, 0},
    {   /* S-DRF: drifting lanes, 5 waves over 3 pitches over 4 levels on a steady pulse: they line up again every 60 */
        {WL(1, 0, 68), WL(5, 0, WW_SAW, WW_GLASS, WW_VOXA, WW_ORGAN, WW_PLS25), WL(3, 0, P_, P_ + 12, P_ + 7),
         WL(4, 0, 127, 100, 115, 90)}, {60}, WS_LOOP, 0},
    {   /* S-RND: sixteenths of the tempo whose waves, pitches and levels each play by chance */
        {WL(8, 0, 32, 32, 32, 32, 32, 32, 32, 32),
         {8, 0, {WW_SAW, WW_SQR, WW_SYNC, WW_DIGI, WW_SAW, WW_FOLD, WW_PLS25, WW_BUZZ}, {0, 40, 50, 30, 0, 50, 40, 60}},
         {4, 0, {P_, P_ + 12, P_, P_ + 7}, {0, 50, 0, 60}},
         {8, 0, {127, 90, 110, 70, 127, 80, 100, 60}, {0, 30, 0, 30, 0, 30, 0, 30}}},
        L4(10), WS_LOOP, 1},
};
#undef P_

/* ----------------------------------------------------------------- the state --- */
typedef struct {
    uint32_t ph[2];              /* the current step's phase, the step before's (while they crossfade) */
    int32_t el;                  /* ms (Q10) into the current TIME step */
    uint32_t rng;                /* the steps' chances (xorshift32), seeded at the note */
    uint8_t idx[WL_N];           /* each lane's step */
    uint8_t back;                /* bit k: lane k runs backwards (PING-PONG) */
    uint8_t src;                 /* the source it was started for (WAV A..D) */
    uint8_t xfading, xf;         /* crossfading from the values before; the crossfade, % of the TIME step */
    uint8_t cw, pw, cl, pl;      /* the wave and LEVEL now, and before (fading out) */
    int8_t cs, ps;               /* the PITCH now and before, semitones */
} ws_corner_t;
typedef struct { ws_corner_t c[4]; } ws_voice_t;
typedef struct { ws_voice_t v[NVOICE]; } ws_part_t;
#ifdef ENG_MEM
ENG_MEM_FITS("WAVESTORM", sizeof(ws_part_t));
#define WS_PART(t) ((ws_part_t *)ENG_MEM((uint32_t)((t) - trk)))
#else
static ws_part_t ws_parts[NPART];   /* a host without engine memory (tests): the engine's own */
#define WS_PART(t) (&ws_parts[(uint32_t)((t) - trk)])
#endif
static ws_voice_t *ws_voice(track_t *t, const voice_t *v) { return &WS_PART(t)->v[(uint32_t)(v - t->v) % NVOICE]; }

static uint32_t ws_src(const track_t *t, uint32_t c) { return (uint32_t)clamp(t->p[P_E4 + c], 0, WS_NSRC - 1); }

/* the sequence corner source k (k = src - WS_NWAVES) plays */
static const ws_lseq_t *ws_seq(const track_t *t, uint32_t k) { (void)t; return &WS_SEQS[k % WS_NSEQ]; }

static uint32_t ws_rand(uint32_t *x)
{
    *x ^= *x << 13;
    *x ^= *x >> 17;
    *x ^= *x << 5;
    return *x;
}
/* the step after s in lane l (mode: every lane's; bit: the lane's ping-pong direction in *back) */
static uint32_t ws_lane_next(const ws_lane_t *l, uint32_t s, uint32_t mode, uint8_t *back, uint32_t bit)
{
    uint32_t n = (uint32_t)clamp(l->len, 1, WS_LSTEPS), lp = l->loop < n ? l->loop : n - 1u;
    if (n < 2u)
        return 0;
    if (mode == WS_PINGPONG) {
        if (!(*back & bit) ? s + 1u >= n : s <= lp)
            *back ^= (uint8_t)bit;
        if (lp + 1u >= n)                       /* (a loop of one step) */
            return lp;
        return *back & bit ? s - 1u : s + 1u;
    }
    if (mode == WS_LOOP)
        return s + 1u < n ? s + 1u : lp;
    return s + 1u < n ? s + 1u : s;
}
/* the lane's next step that plays: the next, skipping those that miss their roll (all of them: the last tried) */
static uint32_t ws_lane_step(const ws_lane_t *l, uint32_t s, uint32_t mode, uint8_t *back, uint32_t bit, uint32_t *rng)
{
    uint32_t k, n = (uint32_t)clamp(l->len, 1, WS_LSTEPS);
    for (k = 0; k < n; k++) {
        s = ws_lane_next(l, s, mode, back, bit);
        if (!l->miss[s] || (ws_rand(rng) >> 8) % 100u >= l->miss[s])
            break;
    }
    return s;
}
static uint32_t ws_lane_first(const ws_lane_t *l, uint32_t mode, uint8_t *back, uint32_t bit, uint32_t *rng)
{
    if (!l->miss[0] || (ws_rand(rng) >> 8) % 100u >= l->miss[0])
        return 0;
    return ws_lane_step(l, 0, mode, back, bit, rng);
}
/* the values of the lanes' steps now */
static void ws_lane_values(const ws_lseq_t *q, ws_corner_t *c)
{
    c->cw = (uint8_t)(q->lane[WL_WAVE].v[c->idx[WL_WAVE] % WS_LSTEPS] % WS_NWAVES);
    c->cs = (int8_t)clamp((int32_t)q->lane[WL_PITCH].v[c->idx[WL_PITCH] % WS_LSTEPS] - 64, -24, 24);
    c->cl = (uint8_t)(q->lane[WL_LEVEL].v[c->idx[WL_LEVEL] % WS_LSTEPS] & 127u);
    c->xf = (uint8_t)clamp(q->xf[c->idx[WL_TIME] % WS_LSTEPS], 0, 100);
}
/* TIME's step now, ms Q10 */
static int32_t ws_dur(const ws_lseq_t *q, const ws_corner_t *c)
{
    uint32_t v = q->lane[WL_TIME].v[c->idx[WL_TIME] % WS_LSTEPS] & 127u;
    if (q->sync) {               /* samples (a beat at most 2 s: 30 BPM) x ms Q10 a sample, Q8: fits 32 bits */
        uint32_t b = (uint32_t)clamp((int32_t)beat_samples(), 1, 2 * FS);
        return (int32_t)clamp((int32_t)((b * WS_SYNC_Q[v >> 3] / 96u * WS_Q10_SMP_Q8) >> 8), 1024, 1 << 24);
    }
    return (int32_t)WS_TIME_MS[v] << 10;
}
/* every lane on to its next step (TIME's step is over): the values so far fade out */
static void ws_tick(const ws_lseq_t *q, ws_corner_t *c)
{
    uint32_t k;
    c->pw = c->cw;
    c->ps = c->cs;
    c->pl = c->cl;
    c->ph[1] = c->ph[0];                         /* (the old step fades out from where it is) */
    for (k = 0; k < WL_N; k++)
        c->idx[k] = (uint8_t)ws_lane_step(&q->lane[k], c->idx[k], q->mode, &c->back, 1u << k, &c->rng);
    ws_lane_values(q, c);
    c->xfading = c->cw != c->pw || c->cs != c->ps || c->cl != c->pl;
}
/* ONE-SHOT: TIME on its last step, held there */
static int ws_held(const ws_lseq_t *q, const ws_corner_t *c)
{
    return q->mode == WS_ONESHOT && c->idx[WL_TIME] + 1u >= (uint32_t)clamp(q->lane[WL_TIME].len, 1, WS_LSTEPS);
}
/* the crossfade into the values now at el (ms Q10) of a TIME step dur long: Q15, 32767 = done */
static int32_t ws_fade(const ws_corner_t *c, int32_t dur, int32_t el)
{
    int32_t xf;                  /* (32-bit only: the target has no 64-bit divide) */
    if (!c->xfading)
        return 32767;
    xf = dur / 100 * c->xf;      /* (dur <= 2^24: no overflow) */
    if (xf < WS_XF_MIN << 10)
        xf = WS_XF_MIN << 10;
    if (el >= xf)
        return 32767;
    return xf < 32768 ? (el << 15) / xf : el / (xf >> 15);   /* (el < xf: el << 15 fits; else xf >> 15 >= 1) */
}

static uint32_t ws_seed = 2463534242u;   /* (each note's chances differ) */
static void ws_corner_start(const track_t *t, ws_corner_t *c, uint32_t src, uint32_t ph)
{
    uint32_t k;
    c->ph[0] = c->ph[1] = ph;
    c->el = 0;
    c->back = 0;
    c->xfading = 0;
    c->src = (uint8_t)src;
    ws_seed += 0x9E3779B9u;
    c->rng = (ws_seed ^ (ph * 0x85EBCA6Bu)) | 1u;
    if (src < WS_NWAVES) {
        c->cw = c->pw = (uint8_t)src;
        c->cs = c->ps = 0;
        c->cl = c->pl = 127;
        return;
    }
    {
        const ws_lseq_t *q = ws_seq(t, src - WS_NWAVES);
        for (k = 0; k < WL_N; k++)
            c->idx[k] = (uint8_t)ws_lane_first(&q->lane[k], q->mode, &c->back, 1u << k, &c->rng);
        ws_lane_values(q, c);
        c->pw = c->cw;
        c->ps = c->cs;
        c->pl = c->cl;
    }
}

static void ws_note_on(track_t *t, voice_t *v)
{
    ws_voice_t *wv = ws_voice(t, v);
    uint32_t c;
    for (c = 0; c < 4u; c++)     /* every corner on one phase: they blend instead of beating */
        ws_corner_start(t, &wv->c[c], ws_src(t, c), v->ph[0]);
    v->s[2] = 0 | 1 << 8;        /* the path: from point 0 to point 1 */
    v->s[3] = 0;
    v->s[4] = 0;                 /* X and Y: jump to the first target (a retrigger keeps s[0], s[1], s[4]: glides on) */
    v->s[5] = v->s[6] = 0;       /* the filter (a retrigger keeps it: no click) */
}

/* the path's offset now (Q15, *x and *y), then advance it by adv (ms Q10) */
static void ws_path(voice_t *v, uint32_t path, int32_t adv, int32_t *x, int32_t *y)
{
    const ws_path_t *pa = &WS_PATHS[path];
    uint32_t from = (uint32_t)v->s[2] & 0xFFu, to = ((uint32_t)v->s[2] >> 8) & 0xFFu, guard = 0;
    int32_t el = v->s[3], ms, f;
    if (!pa->n) {
        *x = *y = 0;
        return;
    }
    if (from >= pa->n || to >= pa->n)                  /* PATH changed under a sounding note: start it over */
        from = 0, to = pa->n > 1 ? 1u : 0u, el = 0;
    if (from == to) {                                   /* a one-shot at its end: hold */
        *x = pa->pt[to].x;
        *y = pa->pt[to].y;
        return;
    }
    ms = pa->pt[to].ms;
    f = ms ? clamp((el << 5) / ms, 0, 32767) : 32767;   /* el is ms Q10: << 5 is Q15 of ms */
    *x = pa->pt[from].x + (((pa->pt[to].x - pa->pt[from].x) * f) >> 15);
    *y = pa->pt[from].y + (((pa->pt[to].y - pa->pt[from].y) * f) >> 15);
    el += adv;
    while (from != to && el >= (int32_t)pa->pt[to].ms << 10 && guard++ < 16u) {
        el -= (int32_t)pa->pt[to].ms << 10;
        from = to;
        if (to + 1u < pa->n)
            to++;
        else if (pa->loop != 0xFFu)
            to = pa->loop;
        else
            el = 0;                                     /* (to stays: from == to, held) */
    }
    v->s[2] = (int32_t)(from | to << 8);
    v->s[3] = el;
}


/* the mip level for a phase increment: the first whose harmonics all stay below Nyquist (inc < 2^(24 + k)) */
static uint32_t ws_level(uint32_t inc)
{
    uint32_t k = 0;
    while (k < WS_LEVELS - 1u && inc >= (1u << (24u + k)))
        k++;
    return k;
}

typedef struct {
    const int16_t *tab;
    uint32_t *ph, inc, sh;
    int32_t g0, g1;              /* Q15 gain at the block's start and end */
} ws_tap_t;

static void ws_tap(ws_tap_t *tp, uint32_t wave, uint32_t *ph, uint32_t inc, int32_t g0, int32_t g1)
{
    uint32_t k = ws_level(inc);
    tp->tab = &WS_WAVES[wave % WS_NWAVES][WS_LVL_OFF[k]];
    tp->sh = WS_LVL_SH[k];
    tp->ph = ph;
    tp->inc = inc;
    tp->g0 = g0;
    tp->g1 = g1;
}

/* one source over the block, added to acc[]: an interpolated table read times a gain ramp. Its own function: the loop
 * keeps the phase, the gain and the table in registers */
static __attribute__((noinline)) void ws_run(const ws_tap_t *tp, int32_t *acc, uint32_t n)
{
    const int16_t *tab = tp->tab;
    uint32_t ph = *tp->ph, inc = tp->inc, sh = tp->sh, fsh = sh - 15u, i;
    int32_t g = tp->g0 << 5, dg = tp->g1 - tp->g0;     /* Q20: g0 -> g1 over CTL (32) samples */
    for (i = 0; i < n; i++) {
        uint32_t k = ph >> sh;
        int32_t a = tab[k], b = tab[k + 1u];
        int32_t s = a + (((b - a) * (int32_t)((ph >> fsh) & 0x7FFFu)) >> 15);
        acc[i] += (s * (g >> 5)) >> 15;
        g += dg;
        ph += inc;
    }
    *tp->ph = ph;
}

/* the phase increment of the voice's pitch plus semi semitones */
static uint32_t ws_inc(const vmod_t *m, int32_t semi)
{
    return semi ? cents_inc(m->pitch16 + semi * 16, 0, m->fine) : m->inc;
}

/* the voice filter (P_FTYPE LP / BP / HP): Felucca-Engines' trapezoidal SVF (dsp.c tsvf_*, A. Simper) with its three
 * taps, as ANALOG writes it out: LP v2, BP v1, HP in - k v1 - v2. At the cutoff the three peak alike (1/k: 2 at RES 0,
 * ~0.15 at 127); linear to half scale, then a soft knee, so only resonance peaks saturate. CUTOFF (0..127) plus the
 * voice's FLT modulation (ENV, LFO, the matrix's CUT: m->cutoff) plus KEY tracking from C4; the state in s[5], s[6] */
static void ws_filter(const int16_t *p, voice_t *v, int32_t *acc, uint32_t n, const vmod_t *m)
{
    uint32_t ft = (uint32_t)clamp(p[P_FTYPE], 1, 3) - 1u, i;
    int32_t cut = (p[P_CUT] << 8) + m->cutoff + ((p[P_FKEY] * (m->pitch16 - 60 * 16)) >> 3);
    int32_t k = 8192 - clamp(p[P_RES], 0, 127) * 7600 / 127, ic1 = v->s[5], ic2 = v->s[6];
    tsvf_t c;
    tsvf_coef_k(&c, cut, k);
    for (i = 0; i < n; i++) {
        int32_t in = acc[i] >> 1, v3 = in - ic2, y, a;
        int32_t v1 = (c.a1 * ic1 + c.a2 * v3) >> 13;
        int32_t v2 = ic2 + ((c.a2 * ic1 + c.a3 * v3) >> 13);
        ic1 = clamp(2 * v1 - ic1, -150000, 150000);
        ic2 = clamp(2 * v2 - ic2, -150000, 150000);
        y = ft == 0u ? v2 : ft == 1u ? v1 : in - ((k * v1) >> 12) - v2;
        a = y < 0 ? -y : y;
        if (a > 16000) {
            a = 16000 + (softclip((a - 16000) * 2) >> 1);
            y = y < 0 ? -a : a;
        }
        acc[i] = y * 2;
    }
    v->s[5] = ic1;
    v->s[6] = ic2;
}

static void ws_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    ws_voice_t *wv = ws_voice(t, v);
    ws_tap_t tap[8];
    int32_t acc[CTL];
    int32_t speed = WS_SPEED_Q8[clamp(p[P_E3], 0, 127)];
    int32_t adv = (int32_t)((WS_MS_BLK_Q16 * (uint32_t)speed) >> 14);   /* ms Q10 a block, at RATE's speed */
    int32_t adv1 = (int32_t)(WS_MS_BLK_Q16 >> 6);                       /* ms Q10 a block, in real time */
    int32_t px, py, tx, ty, x0, y0, x1, y1, g0[4], g1[4];
    uint32_t c, nt = 0, i;
    ws_path(v, (uint32_t)clamp(p[P_E2], 0, WS_NPATH - 1), adv, &px, &py);
    tx = clamp(p[P_E0] * 258 + px + (m->shape - (64 << 8)) * 2, 0, 32767);   /* SHP modulation moves X */
    ty = clamp(p[P_E1] * 258 + py, 0, 32767);           /* (Y: the matrix's Y and the PATH; FLT moves the filter) */
    if (!v->s[4]) {
        x0 = tx;
        y0 = ty;
    } else {
        x0 = v->s[0];
        y0 = v->s[1];
    }
    x1 = x0 + ((tx - x0) >> 2);                         /* one-pole: ~3 ms */
    y1 = y0 + ((ty - y0) >> 2);
    g0[0] = ((32767 - x0) * (32767 - y0)) >> 15;
    g0[1] = (x0 * (32767 - y0)) >> 15;
    g0[2] = ((32767 - x0) * y0) >> 15;
    g0[3] = (x0 * y0) >> 15;
    g1[0] = ((32767 - x1) * (32767 - y1)) >> 15;
    g1[1] = (x1 * (32767 - y1)) >> 15;
    g1[2] = ((32767 - x1) * y1) >> 15;
    g1[3] = (x1 * y1) >> 15;
    for (c = 0; c < 4u; c++) {
        ws_corner_t *cn = &wv->c[c];
        uint32_t src = ws_src(t, c);
        if (src != cn->src)                             /* WAV changed under the note: start the corner over */
            ws_corner_start(t, cn, src, wv->c[0].ph[0]);
        if (src < WS_NWAVES) {
            if (g0[c] || g1[c])
                ws_tap(&tap[nt++], src, &cn->ph[0], m->inc, g0[c], g1[c]);
        } else {
            const ws_lseq_t *q = ws_seq(t, src - WS_NWAVES);
            int32_t dur = ws_dur(q, cn), f0, f1, l;
            while (!ws_held(q, cn) && cn->el >= dur) {
                cn->el -= dur;
                ws_tick(q, cn);
                dur = ws_dur(q, cn);
            }
            f0 = ws_fade(cn, dur, cn->el);
            cn->el += q->sync ? adv1 : adv;          /* (SYNC: the tempo's time, not RATE's) */
            if (ws_held(q, cn) && cn->el > dur)
                cn->el = dur;                           /* held on the last step: no overflow */
            f1 = ws_fade(cn, dur, cn->el);
            if (f0 == 32767)
                f1 = 32767;
            l = cn->cl * 258;                           /* LEVEL 0..127 -> Q15 */
            if (g0[c] || g1[c]) {
                int32_t a0 = mulq15(g0[c], l), a1 = mulq15(g1[c], l);
                if (a0 || a1)
                    ws_tap(&tap[nt++], cn->cw, &cn->ph[0], ws_inc(m, cn->cs), mulq15(a0, f0), mulq15(a1, f1));
                if (f0 < 32767) {                       /* crossfading: the values before, fading out */
                    int32_t lp = cn->pl * 258;
                    int32_t b0 = mulq15(mulq15(g0[c], lp), 32767 - f0), b1 = mulq15(mulq15(g1[c], lp), 32767 - f1);
                    if (b0 || b1)
                        ws_tap(&tap[nt++], cn->pw, &cn->ph[1], ws_inc(m, cn->ps), b0, b1);
                }
            }
            if (f1 == 32767) {
                cn->ph[1] = cn->ph[0];
                cn->xfading = 0;
            }
        }
    }
    for (i = 0; i < n; i++)
        acc[i] = 0;
    for (c = 0; c < nt; c++)
        ws_run(&tap[c], acc, n);
    for (c = 0; c < 4u; c++) {                          /* corners that were silent this block keep up */
        ws_corner_t *cn = &wv->c[c];
        uint32_t k, used = 0;
        for (k = 0; k < nt; k++)
            if (tap[k].ph == &cn->ph[0])
                used = 1;
        if (!used)
            cn->ph[0] += m->inc * n;
    }
    if (p[P_FTYPE])
        ws_filter(p, v, acc, n, m);
    for (i = 0; i < n; i++)
        out[i] += voice_amp(acc[i], m, i);
    v->ph[0] = wv->c[0].ph[0];
    v->s[0] = x1;
    v->s[1] = y1;
    v->s[4] = 1;
}

/* name, {X, Y, PATH, RATE, WAV A, WAV B, WAV C, WAV D}, {A D S R}, fenv (the filter's envelope amount: WS_PFILT),
 * mono, FX(dist, chorus, delay, reverb),
 * pattern. The bank's map: X dark -> bright, Y smooth -> harsh; A or C keeps the body (a sine- or triangle-like corner) */
#define WSQ_CYC (WS_NWAVES + 0)    /* the sequences come after the bank's waves */
#define WSQ_ARP (WS_NWAVES + 1)
#define WSQ_VOX (WS_NWAVES + 2)
#define WSQ_PWM (WS_NWAVES + 3)
#define WSQ_SHM (WS_NWAVES + 4)
#define WSQ_RYT (WS_NWAVES + 5)
#define WSQ_DIG (WS_NWAVES + 6)
#define WSQ_HIT (WS_NWAVES + 7)
static const preset_t WS_PRESETS[] = {
    /* showcase: a pad whose four corners each run a sequence, circling slowly */
    {"STORM PAD", {64, 40, 3, 40, WSQ_SHM, WSQ_CYC, WSQ_VOX, WSQ_PWM}, {70, 90, 115, 90}, 0, 0, FX(0, 50, 25, 70), PAT(5)},
    /* pads: slow looping vector envelopes */
    {"GLASS PAD", {50, 30, 7, 30, WW_SINE, WW_GLASS, WW_TRI, WW_ORGAN}, {80, 90, 110, 95}, 0, 0, FX(0, 60, 20, 70), PAT(5)},
    {"VOX PAD", {64, 64, 5, 25, WW_VOXA, WW_VOXO, WW_VOXE, WW_HOLLW}, {75, 90, 115, 90}, 0, 0, FX(0, 40, 20, 75), PAT(5)},
    {"DRIFT PAD", {64, 50, 4, 20, WW_TRI, WW_SAW, WW_SINE, WW_DIGI}, {85, 95, 110, 100}, 0, 0, FX(0, 70, 30, 70), PAT(5)},
    /* keys and plucks: a fast one-shot from a bright corner to the body */
    {"VECT PLUCK", {10, 10, 8, 70, WW_TRI, WW_SAW, WW_SINE, WW_SYNC}, {0, 70, 30, 50}, 35, 0, FX(0, 20, 45, 30), PAT(3)},
    {"GLASS KEYS", {20, 20, 8, 50, WW_SINE, WW_GLASS, WW_TRI, WW_BUZZ}, {2, 85, 40, 65}, 20, 0, FX(0, 30, 30, 45), PAT(6)},
    {"HIT KEYS", {0, 0, 0, 64, WSQ_HIT, WW_SQR, WW_SINE, WW_SAW}, {0, 80, 60, 50}, 0, 0, FX(0, 20, 30, 35), PAT(6)},
    /* bass: the low-pass's envelope for the attack, Y for the colour */
    {"VECT BASS", {30, 0, 0, 64, WW_TRI, WW_SAW, WW_SQR, WW_SYNC}, {0, 60, 70, 25}, 45, 1, FX(5, 0, 10, 10), PAT(2)},
    {"FOLD BASS", {20, 10, 8, 80, WW_SINE, WW_FOLD, WW_TRI, WW_PLS25}, {0, 55, 80, 20}, 30, 1, FX(10, 0, 5, 5), PAT(8)},
    /* leads: X and Y on the HOME knobs for playing */
    {"VECT LEAD", {80, 30, 0, 64, WW_SQR, WW_SAW, WW_PLS25, WW_SYNC}, {4, 70, 100, 40}, 15, 1, FX(0, 10, 45, 30), PAT(4)},
    {"VOX LEAD", {64, 20, 2, 55, WW_VOXA, WW_VOXE, WW_SAW, WW_SYNC}, {6, 70, 100, 45}, 0, 1, FX(0, 20, 40, 35), PAT(4)},
    /* evolving textures: wave sequences with long crossfades */
    {"SHIMMER", {0, 0, 0, 40, WSQ_SHM, WW_SINE, WW_SINE, WW_SINE}, {60, 90, 120, 100}, 0, 0, FX(0, 50, 40, 80), PAT(5)},
    {"VOWELS", {0, 30, 2, 35, WSQ_VOX, WSQ_VOX, WW_SINE, WW_TRI}, {50, 90, 115, 85}, 0, 0, FX(0, 40, 30, 70), PAT(5)},
    {"CYCLES", {64, 64, 6, 30, WSQ_CYC, WSQ_PWM, WSQ_CYC, WSQ_SHM}, {60, 90, 115, 90}, 0, 0, FX(0, 60, 25, 65), PAT(5)},
    /* rhythmic sequences */
    {"RHYTHM", {0, 0, 0, 64, WSQ_RYT, WW_SAW, WW_SINE, WW_SQR}, {0, 70, 110, 40}, 0, 0, FX(0, 10, 40, 25), PAT(13)},
    {"ARP SEQ", {0, 20, 1, 64, WSQ_ARP, WSQ_ARP, WW_TRI, WW_SINE}, {0, 70, 100, 40}, 0, 0, FX(0, 20, 45, 30), PAT(13)},
    {"DIGI SEQ", {30, 60, 0, 64, WSQ_DIG, WSQ_RYT, WW_SQR, WW_FOLD}, {0, 70, 100, 35}, 0, 0, FX(15, 0, 40, 20), PAT(13)},
};

typedef char ws_presets_fit[NELEM(WS_PRESETS) <= 24u ? 1 : -1];   /* favourites: 24 bits (favorites.c fav_bit) */
/* each preset's filter, as WS_PRESETS (preset_t is Felucca-Engines' and has no room): TYPE (0 OFF, 1 LP, 2 BP, 3 HP),
 * CUTOFF, RESONANCE, KEY; ui.c apply_preset_to sets them (ws_preset_filter) */
static const uint8_t WS_PFILT[][4] = {
    {0, 127, 0, 64},             /* STORM PAD: open (the vector is the tone) */
    {0, 127, 0, 64}, {0, 127, 0, 64}, {1, 96, 20, 64},             /* GLASS PAD, VOX PAD, DRIFT PAD (a soft top) */
    {1, 52, 30, 80}, {1, 70, 15, 64}, {0, 127, 0, 64},             /* VECT PLUCK, GLASS KEYS, HIT KEYS */
    {1, 38, 50, 96}, {1, 48, 35, 80},                              /* VECT BASS, FOLD BASS */
    {1, 84, 35, 80}, {2, 76, 40, 64},                              /* VECT LEAD, VOX LEAD (a vocal band) */
    {0, 127, 0, 64}, {0, 127, 0, 64}, {0, 127, 0, 64},             /* SHIMMER, VOWELS, CYCLES */
    {3, 30, 20, 64}, {1, 80, 45, 64}, {0, 127, 0, 64},             /* RHYTHM (thinned), ARP SEQ, DIGI SEQ */
};
typedef char ws_pfilt_fit[NELEM(WS_PFILT) == NELEM(WS_PRESETS) ? 1 : -1];
static void ws_preset_filter(int16_t *p, uint32_t pi)
{
    const uint8_t *f = WS_PFILT[pi % NELEM(WS_PFILT)];
    p[P_FTYPE] = f[0];
    p[P_CUT] = f[1];
    p[P_RES] = f[2];
    p[P_FKEY] = f[3];
}

static const engine_t ENG_WAVESTORM = {
    .name = "STORM",
    .page_title = {"VECT", "WAVE"},
    .edit = {
        {"X", F_PCT, 0, 127, 0, 0, 0},
        {"Y", F_PCT, 0, 127, 0, 0, 0},
        {"PATH", F_ENUM, 0, WS_NPATH - 1, 0, N_WS_PATH, 0},
        {"RATE", F_INT, 0, 127, 64, 0, 0},
        {"WAV A", F_ENUM, 0, WS_NSRC - 1, WW_SINE, N_WS_SRC, 0},
        {"WAV B", F_ENUM, 0, WS_NSRC - 1, WW_SAW, N_WS_SRC, 0},
        {"WAV C", F_ENUM, 0, WS_NSRC - 1, WW_TRI, N_WS_SRC, 0},
        {"WAV D", F_ENUM, 0, WS_NSRC - 1, WW_SQR, N_WS_SRC, 0},
    },
    .presets = WS_PRESETS,
    .npresets = NELEM(WS_PRESETS),
    .note_on = ws_note_on,
    .render = ws_render,
    .knob = {P_E0, P_E1, P_E2, P_E3},
    .keep = 0x73,                /* X, Y and their set flag: a retrigger glides on; the filter's state */
    .mem = sizeof(ws_part_t),
};
