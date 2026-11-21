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
    CHECK(t->eng_req == ENGI_WAVESTORM && str_eq(ENGINES[0]->presets[t->preset].name, "STORM PAD"),
          "power-on sound: STORM PAD");

    /* ---- the pages */
    ok = 1;
    for (i = 0; i < NPAGES; i++) {                     /* (their page kinds are gone from params.c too) */
        static const char *const GONE[] = {"SONG", "PROJECT", "MIXER", "SLICER", "SLICES", "LANES", "DETAIL"};
        uint32_t g;
        for (g = 0; g < sizeof GONE / sizeof GONE[0]; g++)
            ok &= !str_eq(PAGES[i].title, GONE[g]);
    }
    CHECK(ok, "no SONG, PROJECT, MIXER, SLICER, LANES or DETAIL page");
    CHECK(str_eq(PAGES[page_first(FAM_GLO)].title, "CLOCK") && str_eq(PAGES[page_first(FAM_TRK)].title, "OUTPUT"),
          "GLO opens CLOCK; HOME's taps OUTPUT, then CLOCK");

    /* ---- the name: the splash says WaveStorm, each line inside its square */
    {
        uint32_t k, fit = 1;
        for (k = 0; k < sizeof SPLASH_LINES / sizeof SPLASH_LINES[0]; k++)
            fit &= text_w(&AF_M, SPLASH_LINES[k]) <= SPL_SQ - 2 * SPL_PAD;
        CHECK(str_eq(SPLASH_LINES[0], "WaveStorm") && fit, "the splash: WaveStorm, every line fits its square");
    }

    /* ---- every family's button opens its pages on a tap (SEQ lost its with the SEQ TOOLS layer once) */
    {
        uint32_t f, bad = 0;
        for (f = FAM_HOME + 1u; f < FAM_COUNT; f++) {
            if (FAM_BTN[f] == B_HOME)
                continue;
            web_buttons(1u << B_HOME); render_ms(120); web_buttons(0); render_ms(200);
            web_buttons(1u << FAM_BTN[f]); render_ms(120); web_buttons(0); render_ms(200);
            if (ui.home || cur_page()->fam != f) {
                printf("      family %u (button %u) did not open\n", f, FAM_BTN[f]);
                bad++;
            }
        }
        web_buttons(1u << B_HOME); render_ms(120); web_buttons(0); render_ms(200);
        CHECK(!bad, "ENV LFO FX SCL EDIT GLO SAVE ARP SEQ: a tap opens their pages");
    }

    /* ---- the look: MINT at power-on; HOME's vector square puts its dot where X and Y say */
    CHECK(settings.palette == UI_DEFAULT_INDEX && str_eq(UI_PALETTES[UI_DEFAULT_INDEX].name, "MINT") &&
          T_THEME == UI_PALETTES[UI_DEFAULT_INDEX].c2, "the palette at power-on: MINT");
    {
        int16_t keep[3] = {t->p[P_E0], t->p[P_E1], t->p[P_E2]};
        uint32_t k, hits = 0;
        static const int16_t XY[2][2] = {{0, 0}, {127, 127}};
        for (k = 0; k < 2u; k++) {
            int32_t x, y;
            uint16_t px;
            t->p[P_E0] = XY[k][0];
            t->p[P_E1] = XY[k][1];
            t->p[P_E2] = 0;                               /* (PATH OFF: the dot on the knobs' place) */
            render_ms(200);
            x = vx_px(XY[k][0] * 258);
            y = (int32_t)graph_y() + vx_py(XY[k][1] * 258);
            px = web_fb[y * 240 + x];
            px = (uint16_t)(px >> 8 | px << 8);           /* (the panel's byte order) */
            hits += ui.home && (px == T_THEME || web_fb[y * 240 + x] == T_THEME);
        }
        t->p[P_E0] = keep[0];
        t->p[P_E1] = keep[1];
        t->p[P_E2] = keep[2];
        render_ms(100);
        CHECK(hits == 2u, "HOME: the vector's dot at corner A for X 0 Y 0, at corner D for X 127 Y 127");
    }


    /* ---- every corner source of the 48-wave bank and the sequences: sounds, no clipping */
    {
        uint32_t src, bad = 0;
        float lo = 1, hi = 0;
        CHECK(WS_NWAVES == 48u && WS_NSRC == 58u && str_eq(N_WS_SRC[47], "GRIT") && str_eq(N_WS_SRC[48], "S-CYC") &&
              str_eq(N_WS_SRC[57], "S-RND"),
              "the 48-wave bank, then the 10 sequences (%u sources)", (unsigned)WS_NSRC);
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

    /* ---- MIDI: CC 16 X, CC 17 Y, CC 74 CUTOFF, CC 71 RESONANCE (channel 1: the part) */
    web_midi(0xB0, 16, 0);
    web_midi(0xB0, 17, 127);
    web_midi(0xB0, 74, 32);
    web_midi(0xB0, 71, 100);
    render_ms(100);
    CHECK(t->p[P_E0] == 0 && t->p[P_E1] == 127 && t->p[P_CUT] == 32 && t->p[P_RES] == 100,
          "MIDI CC 16 -> X, 17 -> Y, 74 -> CUTOFF, 71 -> RESONANCE (X %d, Y %d, CUT %d, RES %d)", t->p[P_E0], t->p[P_E1],
          t->p[P_CUT], t->p[P_RES]);

    /* ---- the filter: its page, a preset's setting, and a note through every TYPE */
    {
        uint32_t i, pg = NPAGES, vb = 0, ok_f = 1;
        float pk[4];
        for (i = 0; i < NPAGES; i++)
            if (str_eq(PAGES[i].title, "FILTER") && PAGES[i].fam == FAM_EDIT && PAGES[i].id[0] == P_FTYPE)
                pg = i;
        for (i = 0; i < ENG_WAVESTORM.npresets; i++)
            if (str_eq(ENG_WAVESTORM.presets[i].name, "VECT BASS"))
                vb = i;
        apply_preset_to(t, vb);
        ok_f = t->p[P_FTYPE] == 1 && t->p[P_CUT] < 64 && t->p[P_ED_FLT] > 0;
        t->p[P_LEVEL] = TP[P_LEVEL].def;                 /* (the user preset test above left the mix and the ARP set) */
        t->p[P_AMODE] = TP[P_AMODE].def;
        for (i = 0; i < 4u; i++) {
            t->p[P_FTYPE] = (int16_t)i;
            t->p[P_CUT] = 70;
            web_keys(1u << 12);
            render_ms(300);
            pk[i] = peak;
            web_keys(0);
            render_ms(1500);
        }
        CHECK(pg < NPAGES && ok_f && pk[0] > 0.01f && pk[1] > 0.002f && pk[2] > 0.002f && pk[3] > 0.002f,
              "EDIT > FILTER; VECT BASS sets its low-pass; OFF LP BP HP all sound (%.3f %.3f %.3f %.3f)", pk[0], pk[1],
              pk[2], pk[3]);
        apply_preset_to(t, 0);
        render_ms(100);
    }

    /* ---- MIDI IN: one channel (CH1..CH16) or OMNI; the part hears nothing else */
    {
        uint32_t k, n[3];
        static const struct { int16_t route; uint8_t ch; } R[3] = {{2, 0}, {2, 2}, {ROUTE_OMNI, 9}};
        for (k = 0; k < 3u; k++) {
            uint32_t i;
            song.g[G_ROUTE] = R[k].route;
            render_ms(50);
            web_midi(0x90u | R[k].ch, 60, 100);
            render_ms(60);
            for (i = 0, n[k] = 0; i < NVOICE; i++)
                n[k] += t->v[i].active && t->v[i].note == 60;
            web_midi(0x80u | R[k].ch, 60, 0);
            render_ms(1500);
        }
        song.g[G_ROUTE] = 0;
        render_ms(50);
        CHECK(!n[0] && n[1] && n[2] && NELEM(N_ROUTE) == 17u && str_eq(N_ROUTE[ROUTE_OMNI], "OMNI"),
              "MIDI IN CH3: channel 1 ignored, channel 3 plays; OMNI: channel 10 plays");
    }

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
