/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 André Leitzke Junior */
/* WaveStorm: editing the sound's own path and sequences on the device (eng_wavestorm.c ws_sound_t), five EDIT pages:
 *   PATH      POINT X Y TIME          a point of PATH USER: its place around the X/Y knobs (+-100 %) and the time it
 *                                     takes to get there from the point before
 *   PATH SET  POINTS LOOP FROM        how many points (1..8), the point it loops back to (OFF: a one-shot), FROM: a
 *                                     built-in path copied in at each detent (a start to edit from)
 *   SEQ       SEQ LANE LEN LOOP       which of SEQ1..SEQ4, which lane (TIME WAVE PITCH LEVEL), its length and loop point
 *   STEP      STEP VALUE PROB XFADE   the lane's step: its value (TIME: a length, or a note value with SYNC; WAVE: a
 *                                     wave; PITCH: semitones; LEVEL), its chance, and (TIME) the crossfade into it
 *   SEQ SET   MODE SYNC FROM          LOOP / PING-PONG / ONE-SHOT, SYNC to the tempo, FROM: a built-in sequence copied in
 * Editing the path puts PATH on USER (what is edited is what plays); a corner plays a SEQ when its WAV is set to it.
 * The values are written as the knobs turn, in the main loop; the audio reads them as they are (every index is bounded
 * there); a copy (FROM) is made with the audio IRQ off. The ui_graph.c pictures: graph_wspath, graph_wslanes; the
 * cards: ui_draw.c wse_columns. Included by ui.c */
static struct {
    uint8_t seq, lane, step, point;  /* SEQ1..4 (0..3), the lane (WL_*), its step, the path's point */
    uint8_t from_p, from_s;          /* FROM's last pick: a built-in path 1.. (not OFF) / sequence + 1; 0: none yet */
} wse;
static const char *const WSE_LANE_N[WL_N] = {"TIME", "WAVE", "PITCH", "LEVEL"};
static const char *const WSE_MODE_N[3] = {"ONE", "LOOP", "PING"};   /* WS_ONESHOT WS_LOOP WS_PINGPONG */
static const char *const WSE_SYNC_N[16] = {"1/64", "1/32T", "1/32", "1/16T", "1/16", "1/8T", "1/16.", "1/8", "1/4T",
                                           "1/8.", "1/4", "1/4.", "1/2", "1/2.", "1BAR", "2BAR"};   /* WS_SYNC_Q */

static int wse_page(uint32_t g) { return g >= GR_WSPATH && g <= GR_WSSSET; }
static ws_lseq_t *wse_seq(void) { return &ws_sound(TSEL)->seq[wse.seq % WS_NUSEQ]; }
static ws_lane_t *wse_lane(void) { return &wse_seq()->lane[wse.lane % WL_N]; }
static ws_upath_t *wse_path(void) { return &ws_sound(TSEL)->path; }
static uint32_t wse_len(const ws_lane_t *l) { return (uint32_t)clamp(l->len, 1, WS_LSTEPS); }
static uint32_t wse_npts(void) { return (uint32_t)clamp(wse_path()->n, 1, WS_UPTS); }
static void wse_fix(void)                            /* the cursors inside what there is */
{
    if (wse.step >= wse_len(wse_lane()))
        wse.step = (uint8_t)(wse_len(wse_lane()) - 1u);
    if (wse.point >= wse_npts())
        wse.point = (uint8_t)(wse_npts() - 1u);
}
static uint32_t wse_time_code(uint32_t ms)           /* the nearest TIME code to ms */
{
    uint32_t c = 0;
    while (c < 127u && WS_TIME_MS[c + 1u] <= ms)
        c++;
    if (c < 127u && WS_TIME_MS[c + 1u] - ms < ms - WS_TIME_MS[c])
        c++;
    return c;
}
static void wse_ms(char *b, const char **u, uint32_t ms)   /* "254" ms, "1.3" s */
{
    if (ms < 1000u) {
        fmt_int(b, (int32_t)ms);
        *u = "ms";
    } else {
        fmt_int(b, (int32_t)(ms / 1000u));
        b[str_len(b) + 2] = 0;
        b[str_len(b) + 1] = (char)('0' + ms / 100u % 10u);
        b[str_len(b)] = '.';
        *u = "s";
    }
}
/* step k of lane l (of sequence q): its value as the cards show it (b holds 12) */
static void wse_step_value(const ws_lseq_t *q, uint32_t l, uint32_t k, char *b, const char **u)
{
    uint32_t v = q->lane[l].v[k % WS_LSTEPS];
    *u = "";
    switch (l) {
    case WL_TIME:
        if (q->sync)
            str_cpy(b, WSE_SYNC_N[(v & 127u) >> 3], 12);
        else
            wse_ms(b, u, WS_TIME_MS[v & 127u]);
        break;
    case WL_WAVE:
        str_cpy(b, N_WS_SRC[v % WS_NWAVES], 12);
        break;
    case WL_PITCH: {
        int32_t s = clamp((int32_t)v - 64, -24, 24);
        b[0] = '+';
        fmt_int(b + (s > 0), s);
        *u = "st";
        break;
    }
    default:
        fmt_int(b, (int32_t)((v & 127u) * 100u / 127u));
        *u = "%";
        break;
    }
}

static int32_t wse_turn(int32_t v, int32_t s, int32_t lo, int32_t hi) { return clamp(v + s, lo, hi); }
/* a knob on one of the pages: slot 0..3, s detents */
static void wse_knob(uint32_t g, uint32_t slot, int32_t s)
{
    track_t *t = TSEL;
    ws_upath_t *pa = wse_path();
    ws_lseq_t *q = wse_seq();
    ws_lane_t *l = wse_lane();
    wse_fix();
    if (g == GR_WSPATH || g == GR_WSPSET) {
        ws_pt_t *pt = &pa->pt[wse.point % WS_UPTS];
        if (!(g == GR_WSPATH && slot == 0u))         /* (an edit: the path that plays is the one edited) */
            t->p[P_E2] = WS_UPATH;
        if (g == GR_WSPATH) {
            if (slot == 0u)
                wse.point = (uint8_t)wse_turn(wse.point, s, 0, (int32_t)wse_npts() - 1);
            else if (slot == 1u)
                pt->x = (int16_t)wse_turn(pt->x, s * 328, -16384, 16384);   /* 2 % a detent */
            else if (slot == 2u)
                pt->y = (int16_t)wse_turn(pt->y, s * 328, -16384, 16384);
            else
                pt->ms = WS_TIME_MS[wse_turn((int32_t)wse_time_code(pt->ms), s, 0, 127)];
        } else if (slot == 0u) {                     /* POINTS: a new one where the last is, a quarter second on */
            uint32_t n = (uint32_t)wse_turn((int32_t)wse_npts(), s, 1, WS_UPTS), i;
            for (i = wse_npts(); i < n; i++) {
                pa->pt[i] = pa->pt[i - 1u];
                pa->pt[i].ms = 250;
            }
            if (pa->loop != 0xFFu && pa->loop >= n)
                pa->loop = (uint8_t)(n - 1u);
            pa->n = (uint8_t)n;
        } else if (slot == 1u) {                     /* LOOP: OFF, 1..n */
            int32_t lp = pa->loop == 0xFFu ? -1 : pa->loop;
            lp = wse_turn(lp, s, -1, (int32_t)wse_npts() - 1);
            pa->loop = lp < 0 ? 0xFFu : (uint8_t)lp;
        } else if (slot == 2u) {                     /* FROM: a built-in path (not OFF), copied in */
            wse.from_p = (uint8_t)(wse.from_p ? wse_turn(wse.from_p, s, 1, WS_NBPATH - 1) : s > 0 ? 1 : WS_NBPATH - 1);
            fm1_irq_off();
            ws_path_copy(pa, wse.from_p);
            fm1_irq_on();
        }
    } else if (g == GR_WSSEQ) {
        if (slot == 0u)
            wse.seq = (uint8_t)wse_turn(wse.seq, s, 0, WS_NUSEQ - 1);
        else if (slot == 1u)
            wse.lane = (uint8_t)wse_turn(wse.lane, s, 0, WL_N - 1);
        else if (slot == 2u) {
            uint32_t n = (uint32_t)wse_turn((int32_t)wse_len(l), s, 1, WS_LSTEPS), i;
            for (i = wse_len(l); i < n; i++) {       /* a new step: the last one's value, always playing */
                l->v[i] = l->v[i - 1u];
                l->miss[i] = 0;
                if (wse.lane == WL_TIME)
                    q->xf[i] = q->xf[i - 1u];
            }
            l->len = (uint8_t)n;
            if (l->loop >= n)
                l->loop = (uint8_t)(n - 1u);
        } else
            l->loop = (uint8_t)wse_turn(l->loop, s, 0, (int32_t)wse_len(l) - 1);
    } else if (g == GR_WSSTEP) {
        uint32_t k = wse.step % WS_LSTEPS;
        if (slot == 0u)
            wse.step = (uint8_t)wse_turn(wse.step, s, 0, (int32_t)wse_len(l) - 1);
        else if (slot == 1u) {
            static const uint8_t LO[WL_N] = {0, 0, 40, 0}, HI[WL_N] = {127, WS_NWAVES - 1, 88, 127};
            int32_t d = wse.lane == WL_TIME && q->sync ? s * 8 : s;   /* (SYNC: one note value a detent) */
            l->v[k] = (uint8_t)wse_turn(l->v[k], d, LO[wse.lane % WL_N], HI[wse.lane % WL_N]);
        } else if (slot == 2u)
            l->miss[k] = (uint8_t)(100 - wse_turn(100 - l->miss[k], s * 5, 0, 100));
        else if (wse.lane == WL_TIME)
            q->xf[k] = (uint8_t)wse_turn(q->xf[k], s * 5, 0, 100);
    } else if (g == GR_WSSSET) {
        if (slot == 0u) {
            static const uint8_t ORD[3] = {WS_LOOP, WS_PINGPONG, WS_ONESHOT}, POS[3] = {2, 0, 1};   /* (by WS_*) */
            q->mode = ORD[wse_turn(POS[q->mode % 3u], s, 0, 2)];
        } else if (slot == 1u)
            q->sync = (uint8_t)wse_turn(q->sync, s, 0, 1);
        else if (slot == 2u) {                       /* FROM: a built-in sequence, copied in */
            wse.from_s = (uint8_t)(wse.from_s ? wse_turn(wse.from_s, s, 1, WS_NBSEQ) : s > 0 ? 1 : WS_NBSEQ);
            fm1_irq_off();
            ws_seq_copy(q, wse.from_s - 1u);
            fm1_irq_on();
            wse_fix();
        }
    }
    wse_fix();
    ui.force = 1;
}
/* the corners that play SEQn (bit c), on the part's sound */
static uint32_t wse_corners(const track_t *t, uint32_t n)
{
    uint32_t c, m = 0;
    for (c = 0; c < 4u; c++)
        m |= (uint32_t)(ws_src(t, c) == WS_NWAVES + WS_NBSEQ + n) << c;
    return m;
}
