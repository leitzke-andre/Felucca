/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 André Leitzke Junior */
/* WaveStorm's editor commands (web/wavestorm-editor.html; INFO tag 58): the selected part's own path and sequences
 * (eng_wavestorm.c ws_sound_t), one part a frame:
 *   84 WS_GET      part              -> part, 0, its data
 *   85 WS_SET      part, its data    -> part, rc (0 ok, 1 bad part or length), its data as kept
 *   86 WS_CHANGED  (pushed while WATCHed: editor.c ed_sync) part: changed on the device; the editor reads it again
 * part 0 PATH USER  n, loop (127 = none), then WS_UPTS x (x, y: three bytes each, the value + 16384 = 0..32768, LSB
 *                   first; ms: v14)
 *      1..4 SEQ1..4 mode (0 ONE-SHOT, 1 LOOP, 2 PING-PONG), sync, each lane (TIME WAVE PITCH LEVEL): len, loop, 16
 *                   values, 16 misses (100 - the chance, %); then 16 crossfades (%)
 *      5 (GET)      the sources' names (WAV A..D; a WAVE lane's values are the first WS_NWAVES): count, each
 *      6 (GET)      the paths' names: count, each
 * What a SET keeps is inside what the EDIT pages can set (ui_wsedit.c wse_path_fix, wse_seq_fix). It takes the undo
 * copy as the pages do (sets of one part in a row: one copy, SAVE held puts it back); a path's puts PATH on USER, as
 * editing it on the device does. Included by editor.c */
#define ED_WS_PATH_N (2u + WS_UPTS * 8u)
#define ED_WS_SEQ_N (2u + WL_N * (2u + 2u * WS_LSTEPS) + WS_LSTEPS)

static void ed_u15(int32_t v)
{
    uint32_t u = (uint32_t)(clamp(v, -16384, 16384) + 16384);
    ed_b(u);
    ed_b(u >> 7);
    ed_b(u >> 14);
}
static int32_t ed_ru15(const uint8_t *p) { return (int32_t)(p[0] | p[1] << 7 | (p[2] & 3u) << 14) - 16384; }

static void ed_ws_reply(uint32_t part)
{
    const ws_sound_t *s = ws_sound(TSEL);
    uint32_t i, l;
    if (part == 0u) {
        ed_b(s->path.n);
        ed_b(s->path.loop == 0xFFu ? 127u : s->path.loop);
        for (i = 0; i < WS_UPTS; i++) {
            ed_u15(s->path.pt[i].x);
            ed_u15(s->path.pt[i].y);
            ed_v(s->path.pt[i].ms);
        }
    } else if (part <= WS_NUSEQ) {
        const ws_lseq_t *q = &s->seq[part - 1u];
        ed_b(q->mode);
        ed_b(q->sync);
        for (l = 0; l < WL_N; l++) {
            ed_b(q->lane[l].len);
            ed_b(q->lane[l].loop);
            for (i = 0; i < WS_LSTEPS; i++)
                ed_b(q->lane[l].v[i]);
            for (i = 0; i < WS_LSTEPS; i++)
                ed_b(q->lane[l].miss[i]);
        }
        for (i = 0; i < WS_LSTEPS; i++)
            ed_b(q->xf[i]);
    } else if (part == 5u) {
        ed_b(WS_NSRC);
        for (i = 0; i < WS_NSRC; i++)
            ed_str(N_WS_SRC[i], 8);
    } else {
        ed_b(WS_NPATH);
        for (i = 0; i < WS_NPATH; i++)
            ed_str(N_WS_PATH[i], 8);
    }
}

/* a SET's data into snd's part (then inside the pages' ranges): 0 ok, 1 the wrong length */
static int ed_ws_take(uint32_t part, const uint8_t *a, uint32_t na, ws_sound_t *snd)
{
    uint32_t i, l;
    if (part == 0u) {
        ws_upath_t *pa = &snd->path;
        if (na != ED_WS_PATH_N)
            return 1;
        pa->n = a[0];
        pa->loop = a[1] == 127u ? 0xFFu : a[1];
        for (i = 0, a += 2; i < WS_UPTS; i++, a += 8) {
            pa->pt[i].x = (int16_t)ed_ru15(a);
            pa->pt[i].y = (int16_t)ed_ru15(a + 3);
            pa->pt[i].ms = (uint16_t)clamp(ed_rv(a + 6), 0, 65535);
        }
        wse_path_fix(pa);
        return 0;
    }
    {
        ws_lseq_t *q = &snd->seq[(part - 1u) % WS_NUSEQ];
        if (na != ED_WS_SEQ_N)
            return 1;
        q->mode = a[0];
        q->sync = a[1];
        a += 2;
        for (l = 0; l < WL_N; l++) {
            q->lane[l].len = a[0];
            q->lane[l].loop = a[1];
            for (i = 0; i < WS_LSTEPS; i++) {
                q->lane[l].v[i] = a[2 + i];
                q->lane[l].miss[i] = a[2 + WS_LSTEPS + i];
            }
            a += 2u + 2u * WS_LSTEPS;
        }
        for (i = 0; i < WS_LSTEPS; i++)
            q->xf[i] = a[i];
        wse_seq_fix(q);
        return 0;
    }
}

static int ed_ws_handle(uint32_t cmd, const uint8_t *a, uint32_t na)
{
    track_t *t = TSEL;
    uint32_t part, rc = 0;
    if (cmd != ED_WS_GET && cmd != ED_WS_SET)
        return 0;
    part = na ? a[0] : 127u;
    if (!na || part > 6u || (cmd == ED_WS_GET && na != 1u) || (cmd == ED_WS_SET && part > WS_NUSEQ)) {
        ed_b(part);
        ed_b(1);
        return 1;
    }
    if (cmd == ED_WS_SET) {
        ws_sound_t snd = *ws_sound(t);
        int16_t path = part ? t->p[P_E2] : (int16_t)WS_UPATH;
        rc = (uint32_t)ed_ws_take(part, a + 1, na - 1u, &snd);
        if (!rc && (memcmp(&snd, ws_sound(t), sizeof snd) || path != t->p[P_E2])) {
            edit_undo_take(t, 0x200000u | part, UNDO_WS);
            fm1_irq_off();                               /* (the audio must not see half a sequence) */
            *ws_sound(t) = snd;
            t->p[P_E2] = path;
            fm1_irq_on();
            motion_undo_done(t);
            ui.force = 1;
        }
        ed_w.ws[part] = ed_ws_sig(part);                 /* the editor's own change: no WS_CHANGED back */
    }
    ed_b(part);
    ed_b(rc);
    ed_ws_reply(part);
    return 1;
}
