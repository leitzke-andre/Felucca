/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 André Leitzke Junior */
/* WAVESTORM on its own: the engine (firmware/src/eng_wavestorm.c) with Felucca-Engines' interface and DSP, on a
 * minimal host like firmware/engines/tests/fe_host.c. It
 *   - renders every preset through a phrase (a chord, a retrigger, releases) and prints a hash and peak per render,
 *   - checks the level stays inside the voice's range and that silence follows the release,
 *   - sweeps X and Y fast on a held note and checks for clicks (the largest sample-to-sample step),
 *   - plays every corner source and path once (no crash, no undefined behaviour under the sanitizers),
 *   - times the worst case (8 voices, four crossfading sequences) in ns per voice-sample,
 *   - with an output folder argument, writes each preset's render as a 16-bit WAV there.
 *   cc -O2 -std=c99 -I firmware/engines/build -o build/wavestorm_test tests/wavestorm_test.c && build/wavestorm_test [dir]
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define NVOICE 8
#define NPART 4
#ifdef FE_TABLES_H
#include FE_TABLES_H             /* Felucca's own tables (tests/run_tests.sh: felucca_tables.h) */
#else
#include "fe_tables.h"
#endif
#include "../firmware/engines/src/felucca_engine.h"
enum { P_ATK, P_REL, P_FTYPE, P_CUT, P_RES, P_FKEY, P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7, P_COUNT };
typedef struct track {
    int16_t p[P_COUNT];
    voice_t v[NVOICE];
} track_t;
static track_t trk[NPART];
#include "../firmware/engines/src/dsp.c"
#include "../firmware/src/eng_wavestorm.c"

static const engine_t *const E = &ENG_WAVESTORM;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void voice_on(track_t *t, voice_t *v, uint32_t note)
{
    int sounding = v->active && v->stage != 0;
    uint32_t ph0 = v->ph[0];
    int32_t s[8];
    uint32_t k;
    memcpy(s, v->s, sizeof s);
    v->note = (uint8_t)note;
    v->vel = 100;
    v->gate = v->active = 1;
    v->stage = 1;
    v->pitch16 = v->pitch_cur = (int32_t)note * 16;
    if (!sounding)
        v->env = v->env_out = 0;
    E->note_on(t, v);
    if (sounding) {
        v->ph[0] = ph0;
        for (k = 0; k < 8u; k++)
            if (k != 2u && k != 3u && (E->keep >> k & 1u))
                v->s[k] = s[k];
    }
}

static int32_t env_tick(voice_t *v)   /* attack in 4 blocks, sustain 3/4, release 1/16 a block (fe_host's) */
{
    if (!v->active)
        return 0;
    if (v->stage == 1) {
        v->env += 1 << 22;
        if (v->env >= 1 << 24) {
            v->env = 1 << 24;
            v->stage = 2;
        }
    } else if (v->stage == 2) {
        v->env -= (v->env - (3 << 22)) >> 3;
    } else if (v->stage == 3) {
        v->env -= v->env >> 4;
        if (v->env < (1 << 24) / 1000)
            v->env = 0, v->stage = 0, v->active = 0;
    }
    return v->env >> 9;
}

/* one block of part t: every active voice, with the modulation of shape / cutoff given */
static void block(track_t *t, int32_t *out, int32_t *last, int32_t shape, int32_t cut)
{
    uint32_t k;
    memset(out, 0, sizeof(int32_t) * CTL);
    for (k = 0; k < NVOICE; k++) {
        voice_t *v = &t->v[k];
        vmod_t m;
        int32_t a;
        if (!v->active)
            continue;
        a = env_tick(v);
        if (!v->active)
            continue;
        memset(&m, 0, sizeof m);
        m.pitch16 = v->pitch16;
        m.inc = pitch_inc((uint32_t)clamp(v->pitch16, 0, 2047));
        m.amp0 = last[k];
        m.amp1 = a;
        m.envq15 = a;
        m.cutoff = cut;
        m.shape = (64 << 8) + shape;
        last[k] = a;
        E->render(t, v, out, CTL, &m);
    }
}

static void wav_write(const char *path, const int16_t *x, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    uint8_t h[44];
    uint32_t i, sz = n * 2u;
    static const uint32_t W[] = {0x46464952u, 0, 0x45564157u, 0x20746d66u, 16, 0x00010001u, FS, FS * 2u, 0x00100002u,
                                 0x61746164u, 0};
    if (!f)
        return;
    for (i = 0; i < 11u; i++) {
        uint32_t w = i == 1 ? 36u + sz : i == 10 ? sz : W[i];
        h[i * 4] = (uint8_t)w, h[i * 4 + 1] = (uint8_t)(w >> 8), h[i * 4 + 2] = (uint8_t)(w >> 16), h[i * 4 + 3] = (uint8_t)(w >> 24);
    }
    fwrite(h, 1, 44, f);
    for (i = 0; i < n; i++) {
        uint8_t b[2] = {(uint8_t)x[i], (uint8_t)((uint16_t)x[i] >> 8)};
        fwrite(b, 1, 2, f);
    }
    fclose(f);
}

#define NBLK 4200                /* ~3 s */
static int16_t wav[NBLK * CTL];

/* the phrase: a three-note chord, a fourth note, the first retriggered, all released, the tail */
static void render_preset(const preset_t *pr, uint32_t idx, const char *dir)
{
    track_t *t = &trk[idx % NPART];
    int32_t out[CTL], last[NVOICE] = {0}, peak = 0;
    uint64_t h = 14695981039346656037ull;
    uint32_t b, i, k;
    int32_t tail = 0;
    memset(t, 0, sizeof *t);
    for (k = 0; k < 8u; k++)
        t->p[P_E0 + k] = pr->e[k];
    ws_preset_filter(t->p, idx);                    /* (its filter, as ui.c apply_preset_to sets it) */
    for (b = 0; b < NBLK; b++) {
        int32_t sh = 0, cut = 0;
        if (b == 0) voice_on(t, &t->v[0], 48);
        if (b == 0) voice_on(t, &t->v[1], 55);
        if (b == 0) voice_on(t, &t->v[2], 64);
        if (b == 900) voice_on(t, &t->v[3], 71);
        if (b == 1500) voice_on(t, &t->v[0], 48);
        if (b == 2600)
            for (k = 0; k < NVOICE; k++)
                if (t->v[k].active)
                    t->v[k].gate = 0, t->v[k].stage = 3;
        if (pr->fenv)            /* the preset's filter envelope: a decaying push of the cutoff after the note-on */
            cut = (pr->fenv * 2 * 256 * (int32_t)(b < 900 ? 900 - b : 0)) / 900;
        block(t, out, last, sh, cut);
        for (i = 0; i < CTL; i++) {
            int32_t a = out[i] < 0 ? -out[i] : out[i], s = clamp(out[i] / 2, -32768, 32767);   /* the WAV at -6 dB */
            uint32_t x = (uint32_t)out[i];
            for (k = 0; k < 4u; k++)
                h = (h ^ ((x >> (8u * k)) & 0xFFu)) * 1099511628211ull;
            if (a > peak)
                peak = a;
            if (b >= NBLK - 40u && a > tail)
                tail = a;
            wav[b * CTL + i] = (int16_t)s;
        }
    }
    printf("wavestorm/%u %016llx %d %s\n", idx, (unsigned long long)h, peak, pr->name);
    CHECK(peak > 2000, "%s: almost silent (peak %d)", pr->name, peak);
    CHECK(peak < 4 * VOICE_FS, "%s: over four voices' level (peak %d)", pr->name, peak);   /* (the phrase: 4 voices) */
    CHECK(tail == 0, "%s: still sounding after the release (%u)", pr->name, tail);
    if (dir) {
        char path[512], name[64];
        for (k = 0; pr->name[k] && k < 63u; k++)
            name[k] = pr->name[k] == ' ' ? '_' : pr->name[k];
        name[k] = 0;
        snprintf(path, sizeof path, "%s/%02u_%s.wav", dir, idx, name);
        wav_write(path, wav, NBLK * CTL);
    }
}

/* a held note on four corners: first each corner alone (the largest step between two samples each wave makes on its
 * own: the saw's and the square's resets), then X and Y swept corner to corner in 0.3 s: the sweep may not step more
 * than the waves do (no click, no zipper) */
static void sweep_test(const char *dir)
{
    track_t *t = &trk[0];
    int32_t out[CTL], last[NVOICE] = {0}, prev = 0, maxd = 0, maxd_static = 0;
    uint32_t b, i;
    memset(t, 0, sizeof *t);
    t->p[P_E4] = WW_SINE, t->p[P_E5] = WW_SAW, t->p[P_E6] = WW_SQR, t->p[P_E7] = WW_SINE;
    voice_on(t, &t->v[0], 45);
    for (b = 0; b < 2000; b++) {
        uint32_t ph = b % 400u;
        int32_t xy = ph < 200u ? (int32_t)ph * 127 / 200 : (int32_t)(400u - ph) * 127 / 200;
        t->p[P_E0] = (int16_t)(b < 1000 ? ((b / 250u) & 1u) * 127 : xy);   /* first half: each corner in turn */
        t->p[P_E1] = (int16_t)(b < 1000 ? ((b / 500u) & 1u) * 127 : 127 - xy);
        block(t, out, last, 0, 0);
        for (i = 0; i < CTL; i++) {
            int32_t d = out[i] - prev;
            d = d < 0 ? -d : d;
            prev = out[i];
            if (b % 250u > 20u && b < 1000 && d > maxd_static)   /* (each corner settled) */
                maxd_static = d;
            if (b >= 1000 && d > maxd)
                maxd = d;
            wav[b * CTL + i] = (int16_t)clamp(out[i], -32768, 32767);
        }
    }
    printf("sweep: largest step %d swept, %d with each corner alone\n", maxd, maxd_static);
    CHECK(maxd <= maxd_static + maxd_static / 4, "X/Y sweep: step %d against %d static", maxd, maxd_static);
    if (dir) {
        char path[512];
        snprintf(path, sizeof path, "%s/sweep_xy.wav", dir);
        wav_write(path, wav, 2000 * CTL);
    }
}

/* a saw (all four corners) a semitone higher every 50 ms from MIDI 60 to 126: with the mip levels no harmonic passes
 * Nyquist, so a spectrogram shows only rising lines (aliasing would fall) */
static void alias_render(const char *dir)
{
    track_t *t = &trk[3];
    int32_t out[CTL], last[NVOICE] = {0};
    uint32_t b, i, note = 60, nb = 67u * 69u;
    memset(t, 0, sizeof *t);
    t->p[P_E4] = t->p[P_E5] = t->p[P_E6] = t->p[P_E7] = WW_SAW;
    voice_on(t, &t->v[0], note);
    for (b = 0; b < nb && b < NBLK; b++) {
        if (b && b % 69u == 0)
            t->v[0].pitch16 = (int32_t)++note * 16;
        block(t, out, last, 0, 0);
        for (i = 0; i < CTL; i++)
            wav[b * CTL + i] = (int16_t)clamp(out[i], -32768, 32767);
    }
    if (dir) {
        char path[512];
        snprintf(path, sizeof path, "%s/alias_saw.wav", dir);
        wav_write(path, wav, b * CTL);
    }
}

/* every source on every corner and every path, for a while each: no crash, sound, no clipping */
static void coverage_test(void)
{
    track_t *t = &trk[1];
    int32_t out[CTL], last[NVOICE] = {0};
    uint32_t src, path, b, i;
    for (src = 0; src < WS_NSRC; src++)
        for (path = 0; path < (uint32_t)WS_NPATH; path++) {
            int32_t peak = 0;
            memset(t, 0, sizeof *t);
            memset(last, 0, sizeof last);
            t->p[P_E0] = 64, t->p[P_E1] = 64, t->p[P_E2] = (int16_t)path, t->p[P_E3] = 127;
            t->p[P_E4] = t->p[P_E5] = t->p[P_E6] = t->p[P_E7] = (int16_t)src;
            voice_on(t, &t->v[0], 36 + (src * 5u) % 84u);
            for (b = 0; b < 600; b++) {
                if (b == 300)
                    t->p[P_E5] = (int16_t)((src + 7u) % WS_NSRC);   /* a corner's source changed while it sounds */
                block(t, out, last, 0, 0);
                for (i = 0; i < CTL; i++)
                    peak = out[i] > peak ? out[i] : -out[i] > peak ? -out[i] : peak;
            }
            CHECK(peak > 1000 && peak < 32767, "source %s path %s: peak %d", N_WS_SRC[src], N_WS_PATH[path], peak);
        }
    printf("coverage: %u sources x %d paths\n", WS_NSRC, WS_NPATH);
}

/* the filter: LP takes the top off a saw, HP the body of a sine, BP sits between; full resonance swept over the whole
 * range (modulation, key tracking) stays bounded, finite and dies out after the release */
static int32_t filt_run(uint32_t type, int32_t cut, uint32_t wave, int32_t *dsum)
{
    track_t *t = &trk[3];
    int32_t out[CTL], last[NVOICE] = {0}, peak = 0, prev = 0;
    uint32_t b, i;
    memset(t, 0, sizeof *t);
    t->p[P_E0] = 0, t->p[P_E1] = 0, t->p[P_E3] = 64;
    t->p[P_E4] = t->p[P_E5] = t->p[P_E6] = t->p[P_E7] = (int16_t)wave;
    t->p[P_FTYPE] = (int16_t)type, t->p[P_CUT] = (int16_t)cut, t->p[P_RES] = 0, t->p[P_FKEY] = 0;
    voice_on(t, &t->v[0], 48);
    *dsum = 0;
    for (b = 0; b < 200; b++) {
        block(t, out, last, 0, 0);
        for (i = 0; i < CTL; i++) {
            int32_t a = out[i] < 0 ? -out[i] : out[i], d = out[i] - prev;
            prev = out[i];
            if (b >= 100) {                                  /* (settled) */
                peak = a > peak ? a : peak;
                *dsum += (d < 0 ? -d : d) >> 4;
            }
        }
    }
    return peak;
}
static void filter_test(void)
{
    int32_t d_off, d_lp, d_bp, p_off, p_lp, p_hp, p_sine, tail = 0, peak = 0;
    track_t *t = &trk[3];
    int32_t out[CTL], last[NVOICE] = {0};
    uint32_t b, i;
    p_off = filt_run(0, 127, WW_SAW, &d_off);
    p_lp = filt_run(1, 24, WW_SAW, &d_lp);
    (void)filt_run(2, 60, WW_SAW, &d_bp);
    p_sine = filt_run(0, 127, WW_SINE, &d_bp);
    p_hp = filt_run(3, 100, WW_SINE, &d_bp);
    CHECK(d_lp * 5 < d_off && p_lp > 1000, "filter LP 24: the saw's top (%d against %d), level %d", d_lp, d_off, p_lp);
    CHECK(p_hp * 5 < p_sine, "filter HP 100: a low sine's body (%d against %d)", p_hp, p_sine);
    CHECK(p_off > 1000, "filter OFF: as before (peak %d)", p_off);
    memset(t, 0, sizeof *t);                             /* full resonance, swept up and down the whole range */
    t->p[P_E0] = 64, t->p[P_E1] = 64, t->p[P_E3] = 64;
    t->p[P_E4] = WW_SAW, t->p[P_E5] = WW_SQR, t->p[P_E6] = WW_BUZZ, t->p[P_E7] = WW_SYNC;
    t->p[P_FTYPE] = 1, t->p[P_CUT] = 64, t->p[P_RES] = 127, t->p[P_FKEY] = 127;
    for (i = 0; i < 4u; i++)
        voice_on(t, &t->v[i], 24 + i * 19);
    for (b = 0; b < 1600; b++) {
        int32_t cut = (int32_t)((b % 400u) < 200u ? b % 400u : 400u - b % 400u) * 300 * 256 / 200 - 150 * 256;
        if (b == 800)
            t->p[P_FTYPE] = 2;
        if (b == 1000)
            t->p[P_FTYPE] = 3;
        if (b == 1300)
            for (i = 0; i < NVOICE; i++)
                if (t->v[i].active)
                    t->v[i].gate = 0, t->v[i].stage = 3;
        block(t, out, last, 0, cut);
        for (i = 0; i < CTL; i++) {
            int32_t a = out[i] < 0 ? -out[i] : out[i];
            peak = a > peak ? a : peak;
            if (b >= 1550)
                tail += a > 2;
        }
    }
    CHECK(peak < 4 * 2 * 32768, "filter: full resonance swept, LP BP HP, peak %d (bounded)", peak);
    CHECK(tail == 0, "filter: silent after the release (%d)", tail);
    printf("filter: LP top %d/%d, HP body %d/%d, resonance sweep peak %d\n", d_lp, d_off, p_hp, p_sine, peak);
}

/* the worst case: 8 voices, all four corners sequences mid-crossfade most of the time */
static void cpu_test(void)
{
    track_t *t = &trk[2];
    int32_t out[CTL], last[NVOICE] = {0};
    uint32_t b, k;
    clock_t c0;
    double ns;
    memset(t, 0, sizeof *t);
    t->p[P_E0] = 64, t->p[P_E1] = 64, t->p[P_E2] = 3, t->p[P_E3] = 127;
    t->p[P_E4] = WSQ_SHM, t->p[P_E5] = WSQ_CYC, t->p[P_E6] = WSQ_VOX, t->p[P_E7] = WSQ_PWM;
    for (k = 0; k < NVOICE; k++)
        voice_on(t, &t->v[k], 40 + k * 5);
    c0 = clock();
    for (b = 0; b < 20000; b++)
        block(t, out, last, 0, 0);
    ns = (double)(clock() - c0) / CLOCKS_PER_SEC * 1e9 / (20000.0 * CTL * NVOICE);
    printf("cpu: %.1f ns per voice-sample on this machine (worst case: 4 crossfading sequences)\n", ns);
    t->p[P_FTYPE] = 1, t->p[P_CUT] = 64, t->p[P_RES] = 64;
    c0 = clock();
    for (b = 0; b < 20000; b++)
        block(t, out, last, 0, 0);
    printf("cpu: %.1f ns with the filter (LP)\n",
           (double)(clock() - c0) / CLOCKS_PER_SEC * 1e9 / (20000.0 * CTL * NVOICE));
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : 0;
    uint32_t j;
    printf("wavestorm: %u presets, %u wave sources + %u sequences, %d paths, %zu B engine memory a part\n",
           (unsigned)E->npresets, (unsigned)WS_NWAVES, (unsigned)WS_NSEQ, WS_NPATH, sizeof(ws_part_t));
    for (j = 0; j < E->npresets; j++)
        render_preset(&E->presets[j], j, dir);
    sweep_test(dir);
    alias_render(dir);
    coverage_test();
    filter_test();
    cpu_test();
    if (fails) {
        printf("FAIL: %d checks\n", fails);
        return 1;
    }
    printf("ok: wavestorm\n");
    return 0;
}
