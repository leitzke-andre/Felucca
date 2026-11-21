/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 André Leitzke Junior */
/* WaveStorm's storage (felucca.c includes it where Felucca had its project.c). WaveStorm is a playable synth, not a
 * groovebox: no project slots or songs. What it keeps in flash:
 *   - the settings (MENU, the panel, favourites, MIDI LEARN: settings_persist.c, OBJ_SETTINGS), as Felucca;
 *   - the user presets (upreset.c): a sound with its arp settings and its pattern, its own path, sequences and all
 *     64 steps beside the record (ws_sound.c);
 *   - the autosave (OBJ_AUTOSAVE): the part as it is (its sound, all 64 steps, its automation) and the global
 *     values, put back at power-on with MENU > SYSTEM > RESTORE LAST ON (the default). Written as Felucca's was:
 *     only when it changed and then stayed the same for AS_IDLE_MS with nothing playing,
 *     at most one write every AS_GAP_MS, never while a note or the transport runs.
 * There are no projects: user presets hold a sound with its pattern (upreset.c). */
#define WS_STATE_MAGIC 0x31535357u               /* "WSS1" */
#define WS_STATE_VER 6u                 /* 2: the step without drum lanes or chance (8 bytes); 3: no project globals;
                                         * 4: MIDI IN a channel (was Felucca's blocks of four); 5: the filter;
                                         * 6: the sound's own path and sequences */

typedef struct {
    uint32_t magic;
    uint16_t size, np;                           /* sizeof, P_COUNT when written */
    uint8_t ver, preset, sel, rsv;
    int16_t g[G_COUNT];
    int16_t p[P_COUNT];
    step_t step[NSTEP];
    motion_store_t motion;
    ws_sound_t snd;                              /* the part's own path and sequences (eng_wavestorm.c) */
    uint32_t sum;
} ws_state_t;
static ws_state_t ws_state __attribute__((section(".pool")));   /* (the main loop's staging copy) */

static uint32_t ws_hash(uint32_t h, const void *p, uint32_t n)   /* FNV-1a, continued */
{
    const uint8_t *b = p;
    while (n--) {
        h ^= *b++;
        h *= 16777619u;
    }
    return h;
}
static uint32_t ws_state_sum(const ws_state_t *s) { return ws_hash(2166136261u, s, sizeof *s - sizeof s->sum); }   /* (sum: last) */

static void ws_capture(ws_state_t *s)
{
    uint32_t i, f = motion_guard();
    memset(s, 0, sizeof *s);
    s->magic = WS_STATE_MAGIC;
    s->size = sizeof *s;
    s->np = P_COUNT;
    s->ver = WS_STATE_VER;
    s->preset = trk[0].preset;
    for (i = 0; i < G_COUNT; i++)
        s->g[i] = song.g[i];
    for (i = 0; i < P_COUNT; i++)
        s->p[i] = motion_base_value(&trk[0], i);
    memcpy(s->step, trk[0].step, sizeof s->step);
    s->motion = motion;
    motion_unguard(f);
    s->snd = ws_snd[0];
    s->sum = ws_state_sum(s);
}

static int ws_state_ok(const ws_state_t *s, int n)
{
    return n == (int)sizeof *s && s->magic == WS_STATE_MAGIC && s->size == sizeof *s && s->ver == WS_STATE_VER &&
           s->np == P_COUNT && s->sum == ws_state_sum(s) && s->motion.count <= MOTION_MAX;
}

/* s -> the running part (as Felucca's project_restore_runtime did for one track) */
static void ws_restore(const ws_state_t *s)
{
    track_t *t = &trk[0];
    uint32_t i;
    mom_back(0);                                  /* (ui_input.c MOMENTARY: the values as they were, then the load) */
    transport_req = 2;
    panic_req = 1u;
    fm1_irq_off();                                /* the audio ISR must not see half a sound */
    seq_stop();
    transport_req = 0;
    motion = s->motion;
    memset(motion_active, 0, sizeof motion_active);
    motion_base_valid = 0;
    for (i = 0; i < G_COUNT; i++)
        song.g[i] = (int16_t)clamp(s->g[i], GP[i].min, GP[i].max);
    t->eng_req = ENGI_WAVESTORM;
    t->user = 0;
    for (i = 0; i < P_COUNT; i++)
        t->p[i] = (int16_t)param_fit(param_desc_of(ENGI_WAVESTORM, i), s->p[i]);
    t->preset = (uint8_t)(s->preset % ENGINES[ENGI_WAVESTORM]->npresets);
    memcpy(t->step, s->step, sizeof t->step);
    ws_steps(t->step);
    ws_snd[0] = s->snd;
    song.sel = 0;
    fm1_irq_on();
    pat_from_set(0);
    undo.trk = 0;                                 /* (ui.c) the undo copy belongs to what was there before */
    pat_sig[0] = ~steps_sig(t);                   /* (the steps are the user's) */
    sync_reload = 1;
    ui.force = 1;
}

/* MENU > SYSTEM > CLEAR ALL: the power-on sound with no steps or automation, the global values their defaults. The
 * user presets, settings and MIDI LEARN stay. 0 done */
static int clear_all(void)
{
    ws_state_t *s = &ws_state;
    uint32_t i;
    memset(s, 0, sizeof *s);
    for (i = 0; i < G_COUNT; i++)
        s->g[i] = GP[i].def;
    for (i = 0; i < P_COUNT; i++)
        s->p[i] = param_desc_of(ENGI_WAVESTORM, i)->def;
    for (i = 0; i < NSTEP; i++)
        s->step[i].time = ST_REST;
    ws_restore(s);
    undo_depth++;                                 /* (no undo copy of the power-on sound) */
    apply_preset_to(&trk[0], TRK_DEF[0][1]);
    undo_depth--;
    ui_message("ALL CLEARED");
    return 0;
}

/* settings + learned panel table: one flash object (as Felucca) */
#include "settings_persist.c"

#if FELUCCA_FLASH
static persist_t persist_saved __attribute__((section(".pool")));
static uint8_t persist_pending;                 /* 1 requested, 2 waiting after a flash error */
static uint32_t persist_retry_ms;
#endif

static void persist_boot(void)                    /* before settings_init / panel_init */
{
#if FELUCCA_FLASH
    persist_t p;
    uint32_t f = irq_save();
    uint32_t flash_id = FL_FAR(fl_jedec_ram)();
    flash_ok = flash_id == 0x856014u || flash_id == 0xB36014u; /* known 1 MiB parts, else RAM-only */
    irq_restore(f);
    if (!flash_ok)
        return;
    {
        int n = st_load(OBJ_SETTINGS, &p, sizeof p);
        if (settings_import(&p, n))
            persist_saved = p;
    }
    up_boot();                                     /* user presets */
    ws_snd_boot();                                 /* .. their paths and sequences (ws_sound.c) */
#endif
}

static void settings_poll(void)
{
#if FELUCCA_FLASH
    persist_t p;
    if (!persist_pending || !flash_ok || transport_busy() ||
        (persist_pending == 2u && (uint32_t)(fm1_ms - persist_retry_ms) < 1000u))
        return;
    p = persist_saved;
    settings_export(&p);
    if (!memcmp(&p, &persist_saved, sizeof p)) {
        persist_pending = 0;
        return;                                    /* unchanged: no erase cycle */
    }
    if (st_save(OBJ_SETTINGS, &p, sizeof p) == 0) {
        persist_saved = p;
        persist_pending = 0;
    } else {
        persist_pending = 2;
        persist_retry_ms = fm1_ms;
    }
#endif
}

static void settings_save(void)
{
#if FELUCCA_FLASH
    persist_pending = 1;
#endif
    settings_poll();
}

/* ------------------------------------------------------------------------------------------- the autosave --- */
#define AS_POLL_MS 250u
#define AS_IDLE_MS 10000u
#define AS_GAP_MS 60000u
#define AS_RETRY_MS 30000u
static struct {
    uint32_t saved, seen;                        /* the signature written last (or loaded at power-on), seen last */
    uint32_t t, last, poll;                      /* fm1_ms: seen changed / busy, the last write, the last check */
    uint32_t writes;                             /* (written since power-on: the host tests count them) */
    uint8_t wrote, err;                          /* a write since power-on (last is valid); the last one failed */
} as __attribute__((section(".pool")));

static uint32_t autosave_sig(void)               /* what an autosave would hold, hashed (no copy) */
{
    uint32_t h = 2166136261u, i;
    const track_t *t = &trk[0];
    for (i = 0; i < P_COUNT; i++) {
        int16_t v = motion_base_value(t, i);
        h = ws_hash(h, &v, sizeof v);
    }
    h = ws_hash(h, &t->preset, 1);
    h = ws_hash(h, t->step, sizeof t->step);
    for (i = 0; i < G_COUNT; i++)
        h = ws_hash(h, &song.g[i], sizeof song.g[i]);
    h = ws_hash(h, &ws_snd[0], sizeof ws_snd[0]);
    return ws_hash(h, &motion, sizeof motion);
}

static int autosave_quiet(void)                  /* nothing an erase (~40 ms of silence) would be heard in */
{
    uint32_t i;
    if (transport_busy() || fm1_in.notes || fm1_in.buttons)
        return 0;
    for (i = 0; i < NVOICE; i++)
        if (trk[0].v[i].active)
            return 0;
    return 1;
}

static void autosave_poll(void)                  /* main loop, every pass (after settings_poll) */
{
#if FELUCCA_FLASH
    uint32_t sig;
    if (!flash_ok || fm1_ms - as.poll < AS_POLL_MS)
        return;
    as.poll = fm1_ms;
    sig = autosave_sig();
    if (sig != as.seen || !autosave_quiet()) {
        as.seen = sig;
        as.t = fm1_ms;
        return;
    }
    if (sig == as.saved || (ui_prefs & PREF_RESTORE_OFF) || fm1_ms - as.t < AS_IDLE_MS ||
        (as.wrote && fm1_ms - as.last < (as.err ? AS_RETRY_MS : AS_GAP_MS)))
        return;
    as.last = fm1_ms;
    as.wrote = 1;
    ws_capture(&ws_state);
    as.err = st_save(OBJ_AUTOSAVE, &ws_state, sizeof ws_state) != 0;
    if (!as.err) {
        as.saved = sig;
        as.writes++;
    }
#endif
}

/* power-on, after the defaults (main.c): the last session's part when `allowed` (no crash, hang or failed boot
 * before) and RESTORE LAST is ON; anything else (none, torn, another format) is ignored. 1 = restored */
static int autosave_boot(int allowed)
{
    int rc = 0;
#if FELUCCA_FLASH
    if (flash_ok && allowed && !(ui_prefs & PREF_RESTORE_OFF)) {
        int n = st_load(OBJ_AUTOSAVE, &ws_state, sizeof ws_state);
        if (ws_state_ok(&ws_state, n)) {
            undo_depth++;
            ws_restore(&ws_state);
            undo_depth--;
            ui_message("RESTORED");
            rc = 1;
        }
    }
#else
    (void)allowed;
#endif
    as.saved = as.seen = autosave_sig();
    as.t = fm1_ms;
    return rc;
}

#if FELUCCA_FLASH
_Static_assert(sizeof(ws_state_t) <= ST_PAYLOAD_MAX, "the autosave does not fit one flash sector");
_Static_assert(sizeof(persist_t) <= ST_PAYLOAD_MAX, "settings do not fit one flash sector");
#endif
