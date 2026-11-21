/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 André Leitzke Junior */
/* WaveStorm: what a user preset keeps beside its record (upreset.c): the sound's own path and sequences
 * (eng_wavestorm.c ws_sound_t: PATH USER, SEQ1..SEQ4) and the part's whole pattern, all 64 steps (the record holds the
 * first 16, in Felucca's format). A slot's lives in one of WS_SND_OBJS storage objects (OBJ_WSSND0.., storage.c: the
 * sectors of Felucca's sample slots), WS_SND_PER of them each, mirrored in RAM as the records are. Saved after its
 * record (up_store_cat); loaded after it (up_load, up_pat_load). A slot with none of its own (a record the editor put)
 * loads the sound's factory copies (ws_sound_default) and the record's 16 steps. Erasing a slot or the editor putting
 * a record there forgets them (ws_snd_forget). Version 1 ("WSD1": 4 slots an object, no steps) reads as empty.
 * Included by upreset.c */
#define WS_SND_PER 2u
#define WS_SND_OBJS (UP_SLOTS / WS_SND_PER)
#define WS_SND_MAGIC 0x32445357u                 /* "WSD2" */
enum { WS_HAS_SOUND = 1, WS_HAS_STEPS = 2 };
typedef struct {
    ws_sound_t s;                                /* PATH USER, SEQ1..SEQ4 */
    step_t step[NSTEP];                          /* the whole pattern */
} ws_slot_t;
typedef struct {
    uint32_t magic;
    uint16_t size, nslot;                        /* sizeof(ws_slot_t), WS_SND_PER: else the bank reads as empty */
    uint8_t has[WS_SND_PER];                     /* WS_HAS_*: what of the slot is here */
    ws_slot_t d[WS_SND_PER];
} ws_snd_bank_t;
_Static_assert(UP_SLOTS % WS_SND_PER == 0u && OBJ_WSSND0 + WS_SND_OBJS == OBJ_COUNT, "the sounds' banks: one object each");
_Static_assert(sizeof(ws_snd_bank_t) <= ST_PAYLOAD_MAX, "a sounds' bank fits its sector");
static ws_snd_bank_t ws_snd_bank[WS_SND_OBJS] __attribute__((section(".pool")));   /* (main loop only) */

static ws_snd_bank_t *ws_snd_bank_of(uint32_t k) { return &ws_snd_bank[(k / WS_SND_PER) % WS_SND_OBJS]; }
static uint32_t ws_snd_has(uint32_t k) { return ws_snd_bank_of(k)->has[k % WS_SND_PER]; }

static void ws_steps(step_t *s)                  /* loaded steps inside their fixed fields (as Felucca's proj_steps) */
{
    uint32_t i, j;
    for (i = 0; i < NSTEP; i++) {
        if (s[i].n > 4u) s[i].n = 4;
        if (s[i].time > ST_REST) s[i].time = ST_REST;
        for (j = 0; j < 4u; j++) s[i].note[j] &= 127u;
        s[i].flags &= SF_ACCENT | SF_SLIDE;
        s[i].vel &= 127u;
    }
}

static void ws_snd_boot(void)                    /* persist_boot, after up_boot */
{
    uint32_t b, i;
    for (b = 0; b < WS_SND_OBJS; b++) {
        ws_snd_bank_t *bk = &ws_snd_bank[b];
#if FELUCCA_FLASH
        if (flash_ok && st_load(OBJ_WSSND0 + b, bk, sizeof *bk) == (int)sizeof *bk && bk->magic == WS_SND_MAGIC &&
            bk->size == sizeof(ws_slot_t) && bk->nslot == WS_SND_PER) {
            for (i = 0; i < WS_SND_PER; i++)
                ws_steps(bk->d[i].step);
            continue;
        }
#endif
        memset(bk, 0, sizeof *bk);
        (void)i;
    }
}

static int ws_snd_write(uint32_t k)              /* slot k's bank to flash: 0 ok, 2 a flash error (the RAM copy stays) */
{
    ws_snd_bank_t *bk = ws_snd_bank_of(k);
    bk->magic = WS_SND_MAGIC;
    bk->size = sizeof(ws_slot_t);
    bk->nslot = WS_SND_PER;
#if FELUCCA_FLASH
    if (flash_ok && st_save(OBJ_WSSND0 + (k / WS_SND_PER) % WS_SND_OBJS, bk, sizeof *bk))
        return 2;
#endif
    return 0;
}

/* the sound and the steps part t has now into slot k, then its bank to flash (ws_snd_write) */
static int ws_snd_save(const track_t *t, uint32_t k)
{
    ws_slot_t *d = &ws_snd_bank_of(k)->d[k % WS_SND_PER];
    d->s = *ws_sound(t);
    memcpy(d->step, t->step, sizeof d->step);
    ws_snd_bank_of(k)->has[k % WS_SND_PER] = WS_HAS_SOUND | WS_HAS_STEPS;
    return ws_snd_write(k);
}

static void ws_snd_forget(uint32_t k)            /* slot k keeps nothing of its own (written only if it had) */
{
    if (!ws_snd_has(k))
        return;
    ws_snd_bank_of(k)->has[k % WS_SND_PER] = 0;
    memset(&ws_snd_bank_of(k)->d[k % WS_SND_PER], 0, sizeof(ws_slot_t));
    (void)ws_snd_write(k);
}

/* slot k's path and sequences into part t (none stored: the sound's factory copies). The audio ISR off: no half a
 * sequence */
static void ws_snd_load(const track_t *t, uint32_t k)
{
    fm1_irq_off();
    if (ws_snd_has(k) & WS_HAS_SOUND)
        *ws_sound(t) = ws_snd_bank_of(k)->d[k % WS_SND_PER].s;
    else
        ws_sound_default(t);
    fm1_irq_on();
}

/* slot k's 64 steps, 0 = it keeps none (the record's 16 are its pattern) */
static const step_t *ws_snd_steps(uint32_t k)
{
    return ws_snd_has(k) & WS_HAS_STEPS ? ws_snd_bank_of(k)->d[k % WS_SND_PER].step : 0;
}
static int ws_steps_empty(const step_t *s)
{
    for (uint32_t i = 0; i < NSTEP; i++)
        if (s[i].n)
            return 0;
    return 1;
}
