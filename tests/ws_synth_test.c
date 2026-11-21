/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 André Leitzke Junior */
/* WaveStorm (FELUCCA_WS_ONLY): the firmware as the browser demo builds it (web/emu/felucca_web.c), natively, driven by
 * its own entry points (keys, buttons, MIDI, render): what a WaveStorm unit does.
 *   cc -O1 -g -std=gnu99 -w -fsanitize=address,undefined -DFELUCCA_VERSION='"test"' -Ibuild/gen -Ifirmware/src \
 *      -Ifirmware/hal -Ifirmware/bsp/chip -Ifirmware/bsp/boards/fm1 -o build/ws_synth_test tests/ws_synth_test.c -lm
 * Checks: one part and STORM alone; the pages (no SONG, PROJECT, four-part MIXER; GLO the clock, HOME OUTPUT then
 * CLOCK); a held note sounds, without clipping, and is silent after its release; a user preset brings back the sound,
 * its ARP and its pattern but keeps the mix; the autosave writes once the part has stayed the same, and puts it back;
 * MIDI CC 71 / 74 move X / Y; the heavy song plays. */
#include "../web/emu/felucca_web.c"

#if !FELUCCA_WS_ONLY
#error "ws_synth_test: a WaveStorm build (FELUCCA_WS_ONLY)"
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
                           else { printf("ok    " __VA_ARGS__); printf("\n"); } } while (0)

static float peak;
static void render_ms(uint32_t ms)              /* ms of audio (the main loop runs meanwhile); peak = its largest |L| */
{
    uint32_t n = ms * 441u / 10u, k, i;
    peak = 0;
    for (k = 0; k < n; k += 128u) {
        uint32_t m = n - k < 128u ? n - k : 128u;
        web_render(m);
        for (i = 0; i < m; i++) {
            float a = out_l[i] < 0 ? -out_l[i] : out_l[i];
            if (a > peak)
                peak = a;
        }
    }
}

static const uint8_t ACID[16] = {45, 45, 57, 45, 0, 48, 45, 55, 45, 0, 57, 52, 45, 48, 0, 50};

int main(void)
{
    uint32_t i, ok;
    track_t *t = &trk[0];
    web_nor_erase();
    web_boot();
    render_ms(500);

    /* ---- the instrument */
    CHECK(NTRK == 1u && NPART == 1u && NENGINES == 1u && ENGINES[0] == &ENG_WAVESTORM, "one part, STORM the only engine");
    CHECK(ENGI_DRUM >= NENGINES && ENGI_FM6 >= NENGINES && ENGI_SAMPLE >= NENGINES && ENGI_DIGITAL >= NENGINES &&
          ENGI_PHYS >= NENGINES, "the other engines' numbers are past ENGINES[] (ws_compat.c): never one of a track");
    CHECK(t->eng_req == ENGI_WAVESTORM && str_eq(ENGINES[0]->presets[t->preset].name, "STORM PAD"),
          "power-on sound: STORM PAD");

    /* ---- the pages */
    ok = 1;
    for (i = 0; i < NPAGES; i++)
        ok &= PAGES[i].graph != GR_SONG && PAGES[i].graph != GR_SLOTS && PAGES[i].graph != GR_TRK;
    CHECK(ok, "no SONG, PROJECT or four-part MIXER page");
    CHECK(str_eq(PAGES[page_first(FAM_GLO)].title, "CLOCK") && str_eq(PAGES[page_first(FAM_TRK)].title, "OUTPUT"),
          "GLO opens CLOCK; HOME's taps OUTPUT, then CLOCK");

    /* ---- every corner source of the 48-wave bank and the sequences: sounds, no clipping */
    {
        uint32_t src, bad = 0;
        float lo = 1, hi = 0;
        CHECK(WS_NWAVES == 48u && WS_NSRC == 56u && str_eq(N_WS_SRC[47], "GRIT") && str_eq(N_WS_SRC[48], "S-CYC"),
              "the 48-wave bank, then the 8 sequences (%u sources)", (unsigned)WS_NSRC);
        for (src = 0; src < WS_NSRC; src++) {
            for (i = 0; i < 4u; i++)
                t->p[P_E4 + i] = (int16_t)src;
            t->p[P_E0] = 64;
            t->p[P_E1] = 64;
            web_keys(1u << 12);
            render_ms(250);
            web_keys(0);
            if (peak < 0.005f || peak > 0.99f)
                bad++, printf("  source %u %s: peak %.4f\n", src, N_WS_SRC[src], (double)peak);
            lo = peak < lo ? peak : lo;
            hi = peak > hi ? peak : hi;
            render_ms(3000);
        }
        CHECK(!bad, "every source sounds, none clips (peaks %.3f .. %.3f)", (double)lo, (double)hi);
        apply_preset_to(t, 0);
    }

    /* ---- a note */
    web_keys(1u << 12);                                  /* (key 12 from F3: E4) */
    render_ms(1000);
    CHECK(peak > 0.02f && peak < 0.99f, "a held note sounds, no clipping (peak %.3f)", (double)peak);
    web_keys(0);
    render_ms(4000);
    render_ms(200);
    CHECK(peak < 0.002f, "silent after the release and the effects' tails (peak %.5f)", (double)peak);

    /* ---- a user preset: the sound, its ARP and its pattern; the mix stays */
    t->p[P_E0] = 10;                                     /* X */
    t->p[P_E2] = 8;                                      /* PATH PLUCK */
    t->p[P_AMODE] = 2;
    load_pat16(t, ACID, (const uint8_t[16]){0});
    t->p[P_SLEN] = 16;
    CHECK(up_store(0, "TEST SOUND") == 0 && up_used(0), "SAVE > USER: the part stored in U01");
    t->p[P_E0] = 100;
    t->p[P_E2] = 0;
    t->p[P_AMODE] = 0;
    t->p[P_LEVEL] = 50;
    for (i = 0; i < NSTEP; i++)
        t->step[i].time = ST_REST, t->step[i].n = 0;
    CHECK(up_load(0) == 0, "U01 loads");
    ok = 1;
    for (i = 0; i < 16u; i++)
        ok &= ACID[i] ? t->step[i].time == ST_NOTE && t->step[i].note[0] == ACID[i] : t->step[i].time == ST_REST;
    CHECK(ok && t->p[P_SLEN] == 16, "its 16-step pattern is back");
    CHECK(t->p[P_E0] == 10 && t->p[P_E2] == 8, "its sound is back (X 10, PATH PLUCK)");
    CHECK(t->p[P_AMODE] == 2, "its ARP is back");
    CHECK(t->p[P_LEVEL] == 50, "the mix stays (LEVEL as it was)");

    /* ---- the autosave: written once nothing changed for AS_IDLE_MS with nothing playing; put back at power-on */
    {
        uint32_t w0 = as.writes;
        t->p[P_E1] = 77;                                 /* Y */
        render_ms(12000);
        CHECK(as.writes > w0, "the autosave wrote the part (after 10 s unchanged)");
        t->p[P_E1] = 3;
        for (i = 0; i < NSTEP; i++)
            t->step[i].time = ST_REST, t->step[i].n = 0;
        CHECK(autosave_boot(1) == 1 && t->p[P_E1] == 77 && t->step[0].note[0] == ACID[0],
              "power-on: the autosave puts the part back (Y 77, its steps)");
        ws_state.sum ^= 1u;                              /* a damaged record is ignored */
        CHECK(!ws_state_ok(&ws_state, sizeof ws_state), "a damaged autosave is ignored");
    }

    /* ---- MIDI: CC 71 X, CC 74 Y (channel 1: the part) */
    web_midi(0xB0, 71, 0);
    web_midi(0xB0, 74, 127);
    render_ms(100);
    CHECK(t->p[P_E0] == 0 && t->p[P_E1] == 127, "MIDI CC 71 -> X, CC 74 -> Y (X %d, Y %d)", t->p[P_E0], t->p[P_E1]);

    /* ---- the heavy song: STORM PAD, 4-note chords on 16 steps, playing (the part's own LEVEL and ARP back to their
     * defaults first: a sound load keeps them, and the preset test above set them) */
    t->p[P_LEVEL] = TP[P_LEVEL].def;
    t->p[P_AMODE] = TP[P_AMODE].def;
    web_test_heavy();
    web_buttons(1u << B_PLAY);
    render_ms(60);
    web_buttons(0);
    render_ms(3000);
    CHECK(song.playing && peak > 0.1f && peak < 0.99f, "the heavy song plays, no clipping (peak %.3f)", (double)peak);

    if (fails) {
        printf("FAIL: %d checks\n", fails);
        return 1;
    }
    printf("ok: WaveStorm\n");
    return 0;
}
