/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 André Leitzke Junior */
/* WAVESTORM: vector synthesis with wave sequencing.
 *
 * Four corner sources A B C D sit on a square; an X/Y position sets their gains (bilinear: they always sum to 1)
 *   gA = (1-x)(1-y)   gB = x(1-y)   gC = (1-x)y   gD = xy
 * so X fades A->B (and C->D) and Y fades the top pair into the bottom pair.
 *
 * A corner (WAV A..D) is a single-cycle wave from the band-limited bank (eng_wavestorm_waves.h, tools/gen_wavestorm.py)
 * or one of eight wave sequences (S-CYC .. S-HIT): steps of wave, duration, level, pitch offset and crossfade,
 * one-shot, looping or ping-pong, restarted with each note.
 *
 * The position: X and Y (the knobs) are the centre; PATH adds a vector envelope, a breakpoint path restarted with
 * each note (loops, or one-shots such as PLUCK and SWELL); RATE scales the speed of the path and of the wave
 * sequences together (1/8x .. 1x at 64 .. ~8x). The host's SHP modulation moves X and its FLT modulation moves Y:
 * ENV DEST SHP / FLT, LFO DEST SHP / FLT, the matrix's SHP / CUT, and a preset's filter envelope amount (fenv) all
 * steer the vector. WAVESTORM has no filter of its own (v1): the vector is its tone control.
 *
 * Cost: everything but the table reads runs once per block (CTL samples): the path, the sequences, the gains, the
 * mip level. A sample reads one interpolated table per sounding source ("tap"): 4 for four static corners, up to 8
 * while all four corners crossfade, and fewer when X or Y sit at an edge (a tap whose gain is 0 over the whole block
 * is skipped). The gains ramp linearly across the block (no zipper noise); X and Y also pass a one-pole smoother.
 *
 * State: s[0] s[1] the smoothed X and Y (Q15), s[2] the path's segment (from | to << 8), s[3] the time in it (ms
 * Q10), s[4] 1 once X and Y are set (0: the first block jumps to the target); per corner the phases and the
 * sequence's step in the part's engine memory (ws_part_t; the host's ENG_MEM, or an array of the engine's own). */
#include "eng_wavestorm_waves.h"

#define WS_NSEQ 8
#define WS_NSRC (WS_NWAVES + WS_NSEQ)
typedef char ws_bank_is_16[WS_NWAVES == 16 ? 1 : -1];   /* N_WS_SRC below names the bank's 16 waves in order */
static const char *const N_WS_SRC[WS_NSRC + 1] = {
    "SINE", "TRI", "SAW", "SQR", "PLS25", "PLS10", "ORGAN", "HOLLW", "GLASS", "VOX-A", "VOX-O", "VOX-E", "SYNC", "FOLD",
    "BUZZ", "DIGI", "S-CYC", "S-ARP", "S-VOX", "S-PWM", "S-SHM", "S-RYT", "S-DIG", "S-HIT", 0};
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
typedef struct {
    uint8_t wave;                /* WW_* */
    int8_t semi;                 /* pitch offset, semitones */
    uint8_t level;               /* 0..255 */
    uint8_t pad;
    uint16_t ms, xf;             /* the step's length; the crossfade into it from the step before (both at 1x) */
} ws_step_t;
enum { WS_ONESHOT, WS_LOOP, WS_PINGPONG };
typedef struct { const ws_step_t *st; uint8_t n, mode, loop; } ws_seq_t;   /* loop: where WS_LOOP goes back to */

static const ws_step_t WQ_CYC[] = {   /* a slow timbre cycle */
    {WW_SAW, 0, 255, 0, 600, 400}, {WW_SQR, 0, 255, 0, 600, 400}, {WW_HOLLW, 0, 255, 0, 600, 400},
    {WW_TRI, 0, 255, 0, 600, 400}};
static const ws_step_t WQ_ARP[] = {   /* octave and fifth steps, short crossfades */
    {WW_SAW, 0, 255, 0, 150, 0}, {WW_SAW, 12, 200, 0, 150, 0}, {WW_SQR, 7, 210, 0, 150, 0},
    {WW_SAW, 19, 180, 0, 150, 0}};
static const ws_step_t WQ_VOX[] = {   /* vowels: a-o-e-o */
    {WW_VOXA, 0, 255, 0, 500, 350}, {WW_VOXO, 0, 255, 0, 500, 350}, {WW_VOXE, 0, 255, 0, 500, 350},
    {WW_VOXO, 0, 255, 0, 500, 350}};
static const ws_step_t WQ_PWM[] = {   /* the pulse widths, back and forth */
    {WW_SQR, 0, 255, 0, 250, 240}, {WW_PLS25, 0, 255, 0, 250, 240}, {WW_PLS10, 0, 255, 0, 250, 240}};
static const ws_step_t WQ_SHM[] = {   /* shimmer: bright partials an octave up, slowly */
    {WW_GLASS, 0, 255, 0, 900, 700}, {WW_ORGAN, 12, 190, 0, 900, 700}, {WW_GLASS, 12, 170, 0, 900, 700},
    {WW_SINE, 0, 255, 0, 900, 700}};
static const ws_step_t WQ_RYT[] = {   /* rhythm: accents and gaps on sixteenth-ish steps */
    {WW_SYNC, 0, 255, 0, 125, 0}, {WW_SAW, 0, 90, 0, 125, 0}, {WW_SQR, 12, 200, 0, 125, 0},
    {WW_SAW, 0, 0, 0, 125, 0}, {WW_SYNC, 0, 230, 0, 125, 0}, {WW_SAW, 7, 120, 0, 125, 0},
    {WW_DIGI, 0, 210, 0, 125, 0}, {WW_SAW, 0, 0, 0, 125, 0}};
static const ws_step_t WQ_DIG[] = {   /* digital chatter: fast hops between waves */
    {WW_DIGI, 0, 255, 0, 60, 0}, {WW_FOLD, 0, 255, 0, 60, 0}, {WW_BUZZ, 0, 200, 0, 60, 0},
    {WW_SYNC, 12, 230, 0, 60, 0}, {WW_PLS10, 0, 230, 0, 60, 0}, {WW_GLASS, 24, 180, 0, 60, 0}};
static const ws_step_t WQ_HIT[] = {   /* a one-shot attack: a bright click into a soft body that stays */
    {WW_BUZZ, 12, 255, 0, 25, 0}, {WW_SYNC, 0, 255, 0, 60, 20}, {WW_SAW, 0, 255, 0, 150, 120},
    {WW_TRI, 0, 255, 0, 1000, 300}};
static const ws_seq_t WS_SEQS[WS_NSEQ] = {
    {WQ_CYC, NELEM(WQ_CYC), WS_LOOP, 0}, {WQ_ARP, NELEM(WQ_ARP), WS_LOOP, 0},
    {WQ_VOX, NELEM(WQ_VOX), WS_LOOP, 0}, {WQ_PWM, NELEM(WQ_PWM), WS_PINGPONG, 0},
    {WQ_SHM, NELEM(WQ_SHM), WS_LOOP, 0}, {WQ_RYT, NELEM(WQ_RYT), WS_LOOP, 0},
    {WQ_DIG, NELEM(WQ_DIG), WS_LOOP, 0}, {WQ_HIT, NELEM(WQ_HIT), WS_ONESHOT, 0},
};

/* ----------------------------------------------------------------- the state --- */
typedef struct {
    uint32_t ph[2];              /* the current step's phase, the step before's (while they crossfade) */
    int32_t el;                  /* ms (Q10) into the current step */
    uint8_t step, prev, src;     /* the step, the one before, the source it was started for (WAV A..D) */
    int8_t dir;                  /* WS_PINGPONG: +1 / -1 */
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

static void ws_corner_start(ws_corner_t *c, uint32_t src, uint32_t ph)
{
    c->ph[0] = c->ph[1] = ph;
    c->el = 0;
    c->step = c->prev = 0;
    c->dir = 1;
    c->src = (uint8_t)src;
}

static void ws_note_on(track_t *t, voice_t *v)
{
    ws_voice_t *wv = ws_voice(t, v);
    uint32_t c;
    for (c = 0; c < 4u; c++)     /* every corner on one phase: they blend instead of beating */
        ws_corner_start(&wv->c[c], ws_src(t, c), v->ph[0]);
    v->s[2] = 0 | 1 << 8;        /* the path: from point 0 to point 1 */
    v->s[3] = 0;
    v->s[4] = 0;                 /* X and Y: jump to the first target (a retrigger keeps s[0], s[1], s[4]: glides on) */
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

/* the step after s in q (and the direction for ping-pong) */
static uint32_t ws_next(const ws_seq_t *q, uint32_t s, int8_t *dir)
{
    if (q->n < 2u)
        return 0;
    switch (q->mode) {
    case WS_PINGPONG:
        if ((*dir > 0 && s + 1u >= q->n) || (*dir < 0 && s == 0))
            *dir = (int8_t)-*dir;
        return *dir > 0 ? s + 1u : s - 1u;
    case WS_LOOP:
        return s + 1u < q->n ? s + 1u : q->loop;
    default:
        return s + 1u < q->n ? s + 1u : s;
    }
}

/* the crossfade into the current step at el (ms Q10): Q15, 32767 = done */
static int32_t ws_fade(const ws_step_t *st, int32_t el)
{
    int32_t xf = st->xf > WS_XF_MIN ? st->xf : WS_XF_MIN;
    return clamp((el << 5) / xf, 0, 32767);
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

static void ws_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    ws_voice_t *wv = ws_voice(t, v);
    ws_tap_t tap[8];
    int32_t acc[CTL];
    int32_t speed = WS_SPEED_Q8[clamp(p[P_E3], 0, 127)];
    int32_t adv = (int32_t)((WS_MS_BLK_Q16 * (uint32_t)speed) >> 14);   /* ms Q10 a block */
    int32_t px, py, tx, ty, x0, y0, x1, y1, g0[4], g1[4];
    uint32_t c, nt = 0, i;
    ws_path(v, (uint32_t)clamp(p[P_E2], 0, WS_NPATH - 1), adv, &px, &py);
    tx = clamp(p[P_E0] * 258 + px + (m->shape - (64 << 8)) * 2, 0, 32767);   /* SHP modulation moves X */
    ty = clamp(p[P_E1] * 258 + py + m->cutoff * 2, 0, 32767);               /* FLT modulation moves Y */
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
            ws_corner_start(cn, src, wv->c[0].ph[0]);
        if (src < WS_NWAVES) {
            if (g0[c] || g1[c])
                ws_tap(&tap[nt++], src, &cn->ph[0], m->inc, g0[c], g1[c]);
        } else {
            const ws_seq_t *q = &WS_SEQS[src - WS_NWAVES];
            const ws_step_t *st, *pv;
            int32_t f0, f1, l;
            if (cn->step >= q->n || cn->prev >= q->n)
                cn->step = cn->prev = 0;
            while (!(q->mode == WS_ONESHOT && cn->step + 1u >= q->n)
                   && cn->el >= (int32_t)(q->st[cn->step].ms ? q->st[cn->step].ms : 1) << 10) {
                cn->el -= (int32_t)(q->st[cn->step].ms ? q->st[cn->step].ms : 1) << 10;
                cn->prev = cn->step;
                cn->step = (uint8_t)ws_next(q, cn->step, &cn->dir);
                cn->ph[1] = cn->ph[0];                  /* the old step fades out from where it is */
            }
            st = &q->st[cn->step];
            pv = &q->st[cn->prev];
            f0 = cn->prev == cn->step && cn->el == 0 ? 32767 : ws_fade(st, cn->el);
            if (cn->step == 0 && cn->prev == 0)
                f0 = 32767;                             /* the first step of a note: no fade in */
            cn->el += adv;
            if (q->mode == WS_ONESHOT && cn->step + 1u >= q->n && cn->el > (1 << 30))
                cn->el = 1 << 30;                       /* held on the last step: no overflow */
            f1 = f0 == 32767 ? 32767 : ws_fade(st, cn->el);
            l = st->level * 128 + (st->level >> 1);    /* 0..255 -> Q15 */
            if (g0[c] || g1[c]) {
                int32_t a0 = mulq15(g0[c], l), a1 = mulq15(g1[c], l);
                if (a0 || a1)
                    ws_tap(&tap[nt++], st->wave, &cn->ph[0], ws_inc(m, st->semi), mulq15(a0, f0), mulq15(a1, f1));
                if (f0 < 32767) {                       /* crossfading: the step before, fading out */
                    int32_t lp = pv->level * 128 + (pv->level >> 1);
                    int32_t b0 = mulq15(mulq15(g0[c], lp), 32767 - f0), b1 = mulq15(mulq15(g1[c], lp), 32767 - f1);
                    if (b0 || b1)
                        ws_tap(&tap[nt++], pv->wave, &cn->ph[1], ws_inc(m, pv->semi), b0, b1);
                }
            }
            if (f1 == 32767)
                cn->ph[1] = cn->ph[0];
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
    for (i = 0; i < n; i++)
        out[i] += voice_amp(acc[i], m, i);
    v->ph[0] = wv->c[0].ph[0];
    v->s[0] = x1;
    v->s[1] = y1;
    v->s[4] = 1;
}

/* name, {X, Y, PATH, RATE, WAV A, WAV B, WAV C, WAV D}, {A D S R}, fenv (moves Y), mono, FX(dist, chorus, delay, reverb),
 * pattern. The bank's map: X dark -> bright, Y smooth -> harsh; A or C keeps the body (a sine- or triangle-like corner) */
#define WSQ_CYC 16
#define WSQ_ARP 17
#define WSQ_VOX 18
#define WSQ_PWM 19
#define WSQ_SHM 20
#define WSQ_RYT 21
#define WSQ_DIG 22
#define WSQ_HIT 23
static const preset_t WS_PRESETS[] = {
    /* showcase: a pad whose four corners each run a sequence, circling slowly */
    {"STORM PAD", {64, 40, 3, 40, WSQ_SHM, WSQ_CYC, WSQ_VOX, WSQ_PWM}, {70, 90, 115, 90}, 0, 0, FX(0, 50, 25, 70), PAT(5)},
    /* pads: slow looping vector envelopes */
    {"GLASS PAD", {50, 30, 7, 30, WW_SINE, WW_GLASS, WW_TRI, WW_ORGAN}, {80, 90, 110, 95}, 0, 0, FX(0, 60, 20, 70), PAT(5)},
    {"VOX PAD", {64, 64, 5, 25, WW_VOXA, WW_VOXO, WW_VOXE, WW_HOLLW}, {75, 90, 115, 90}, 0, 0, FX(0, 40, 20, 75), PAT(5)},
    {"DRIFT PAD", {64, 50, 4, 20, WW_TRI, WW_SAW, WW_SINE, WW_DIGI}, {85, 95, 110, 100}, 0, 0, FX(0, 70, 30, 70), PAT(5)},
    /* keys and plucks: a fast one-shot from a bright corner to the body */
    {"VECT PLUCK", {10, 10, 8, 70, WW_TRI, WW_SAW, WW_SINE, WW_SYNC}, {0, 70, 30, 50}, 0, 0, FX(0, 20, 45, 30), PAT(3)},
    {"GLASS KEYS", {20, 20, 8, 50, WW_SINE, WW_GLASS, WW_TRI, WW_BUZZ}, {2, 85, 40, 65}, 0, 0, FX(0, 30, 30, 45), PAT(6)},
    {"HIT KEYS", {0, 0, 0, 64, WSQ_HIT, WW_SQR, WW_SINE, WW_SAW}, {0, 80, 60, 50}, 0, 0, FX(0, 20, 30, 35), PAT(6)},
    /* bass: the filter envelope pushes Y for the attack */
    {"VECT BASS", {30, 0, 0, 64, WW_TRI, WW_SAW, WW_SQR, WW_SYNC}, {0, 60, 70, 25}, 45, 1, FX(5, 0, 10, 10), PAT(2)},
    {"FOLD BASS", {20, 10, 8, 80, WW_SINE, WW_FOLD, WW_TRI, WW_PLS25}, {0, 55, 80, 20}, 30, 1, FX(10, 0, 5, 5), PAT(8)},
    /* leads: X and Y on the HOME knobs for playing */
    {"VECT LEAD", {80, 30, 0, 64, WW_SQR, WW_SAW, WW_PLS25, WW_SYNC}, {4, 70, 100, 40}, 0, 1, FX(0, 10, 45, 30), PAT(4)},
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
    .keep = 0x13,                /* X, Y and their set flag: a retrigger glides on */
    .mem = sizeof(ws_part_t),
};
