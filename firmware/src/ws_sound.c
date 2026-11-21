/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 André Leitzke Junior */
/* WaveStorm: the user presets' own paths and sequences (eng_wavestorm.c ws_sound_t: PATH USER, SEQ1..SEQ4), kept
 * beside the records (upreset.c), as Felucca kept FM6 patches. A slot's lives in one of WS_SND_OBJS storage objects
 * (OBJ_WSSND0.., storage.c: the sectors of Felucca's sample slots), WS_SND_PER of them each, mirrored in RAM as the
 * records are. Saved after its record (up_store_cat); loaded after it (up_load). A slot with no data of its own (a
 * record of before, or one the editor put) loads the sound's factory copies (ws_sound_default). Included by upreset.c */
#define WS_SND_PER 4u
#define WS_SND_OBJS (UP_SLOTS / WS_SND_PER)
#define WS_SND_MAGIC 0x31445357u                 /* "WSD1" */
typedef struct {
    uint32_t magic;
    uint16_t size, nslot;                        /* sizeof(ws_sound_t), WS_SND_PER: else the bank reads as empty */
    uint8_t valid[WS_SND_PER];                   /* 1: the slot's data is here */
    ws_sound_t s[WS_SND_PER];
} ws_snd_bank_t;
_Static_assert(UP_SLOTS % WS_SND_PER == 0u && OBJ_WSSND0 + WS_SND_OBJS == OBJ_COUNT, "the sounds' banks: one object each");
_Static_assert(sizeof(ws_snd_bank_t) <= ST_PAYLOAD_MAX, "a sounds' bank fits its sector");
static ws_snd_bank_t ws_snd_bank[WS_SND_OBJS] __attribute__((section(".pool")));   /* (main loop only) */

static void ws_snd_boot(void)                    /* persist_boot, after up_boot */
{
    uint32_t b;
    for (b = 0; b < WS_SND_OBJS; b++) {
        ws_snd_bank_t *bk = &ws_snd_bank[b];
#if FELUCCA_FLASH
        if (flash_ok && st_load(OBJ_WSSND0 + b, bk, sizeof *bk) == (int)sizeof *bk && bk->magic == WS_SND_MAGIC &&
            bk->size == sizeof(ws_sound_t) && bk->nslot == WS_SND_PER)
            continue;
#endif
        memset(bk, 0, sizeof *bk);
    }
}

/* the sound part t has now into slot k, then its bank to flash: 0 ok, 2 a flash error (the RAM copy stays) */
static int ws_snd_save(const track_t *t, uint32_t k)
{
    ws_snd_bank_t *bk = &ws_snd_bank[(k / WS_SND_PER) % WS_SND_OBJS];
    bk->magic = WS_SND_MAGIC;
    bk->size = sizeof(ws_sound_t);
    bk->nslot = WS_SND_PER;
    bk->s[k % WS_SND_PER] = *ws_sound(t);
    bk->valid[k % WS_SND_PER] = 1;
#if FELUCCA_FLASH
    if (flash_ok && st_save(OBJ_WSSND0 + k / WS_SND_PER, bk, sizeof *bk))
        return 2;
#endif
    return 0;
}

/* slot k's into part t (none stored: the sound's factory copies). The audio ISR off: no half a sequence */
static void ws_snd_load(const track_t *t, uint32_t k)
{
    const ws_snd_bank_t *bk = &ws_snd_bank[(k / WS_SND_PER) % WS_SND_OBJS];
    fm1_irq_off();
    if (bk->magic == WS_SND_MAGIC && bk->valid[k % WS_SND_PER])
        *ws_sound(t) = bk->s[k % WS_SND_PER];
    else
        ws_sound_default(t);
    fm1_irq_on();
}
