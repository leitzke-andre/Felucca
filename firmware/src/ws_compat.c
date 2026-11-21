/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 André Leitzke Junior */
/* WaveStorm (FELUCCA_WS_ONLY): what the shared Felucca code asks of the engines WaveStorm does not have, answered as
 * "not this engine" or "nothing to do", so that code stays as Felucca has it.
 *   - The other engines' numbers are past ENGINES[] (>= NENGINES): a track's engine (always STORM, 0) never equals one,
 *     so every `t->eng_req == ENGI_DRUM` path is dead. They must never index anything: ws_compat_check (tests) and a
 *     grep for `[ENGI_` keep it so.
 *   - Their engine_t objects are null pointers: `ENGINES[e] == &ENG_ANALOG` is never true (ENGINES[] holds STORM).
 *   - SAMPLE's preset aliases: none. FM6's patch functions and DIGITAL's conversion: no-ops.
 * Included by engines.c after eng_wavestorm.c. */
#define ENGI_DIGITAL 0xF1u
#define ENGI_SAMPLE 0xF4u
#define ENGI_PHYS 0xF9u
#define ENGI_DRUM 0xFAu
#define ENGI_FM6 0xFCu
#define ENGI_SLICE 0xFDu
#define ENGI_ADPCM 0xFEu
#define ENG_ANALOG (*(const engine_t *)0)
#define ENG_SAMPLE (*(const engine_t *)0)
#define ENG_GRAIN (*(const engine_t *)0)
#define ENG_PHYS (*(const engine_t *)0)
#define ENG_DRUM (*(const engine_t *)0)
#define ENG_WHEEL (*(const engine_t *)0)
#define ENG_FM6 (*(const engine_t *)0)
#define ENG_SLICE (*(const engine_t *)0)

/* SAMPLE: its retired presets (aliases) and sets */
#define SMP_PRESET_TABLE ((const preset_t *)0)
#define SMP_NALIAS 0u
#define SMP_NSETS 0u
#define SMP_SET_PERC 0xFFu
static const uint8_t SMP_SET_ORIG[1] = {0};

/* FM6: a track's patch (the undo copy keeps one) */
#define FM6_OWN 0u
#define FM6_PACKED 1u
static const uint8_t FM6_INIT[FM6_PACKED] = {0};
static uint8_t fm6_own_ok;
static uint32_t fm6_pgen[NPART];                 /* (a patch's generation: the screen redraws on a change) */
static void fm6_set_patch(uint32_t tr, const uint8_t *v) { (void)tr; (void)v; }
static void fm6_adopt(uint32_t tr) { (void)tr; }
static void fm6_unpack(const uint8_t *in, uint8_t *v) { (void)in; (void)v; }
static void fm6_pack(const uint8_t *v, uint8_t *out) { (void)v; (void)out; }
static int32_t fm6_bank_slot(int32_t s) { (void)s; return -1; }   /* (no bank voice) */
static int fm6_slot_ok(int32_t v) { (void)v; return 1; }

/* DIGITAL (retired): its sounds converted to FM6 */
static uint32_t fm4_convert(const int16_t *p, uint8_t *v) { (void)p; (void)v; return 0; }
static void fm4_preset_values(int16_t *p, uint32_t k) { (void)p; (void)k; }

/* PHYS's and SAMPLE's retired sounds that load as DRUM (core.h): never one here */
static int drum_from_phys(uint32_t engine, int16_t *e) { (void)engine; (void)e; return 0; }
static int drum_from_perc(uint32_t engine, int16_t *e) { (void)engine; (void)e; return 0; }
static void phys_legacy(int16_t *e) { (void)e; }

/* SLICE's page */
static int slice_page_ok(void) { return 0; }
