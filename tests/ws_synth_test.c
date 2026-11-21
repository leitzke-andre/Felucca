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

/* the editor protocol through the browser demo's bridge: a request (cmd, args) -> the first frame back with that cmd
 * (its args in rep, their count returned; -1 none). Pushes before it are dropped */
static int sx_req(uint32_t cmd, const uint8_t *a, uint32_t na, uint8_t *rep, uint32_t max)
{
    uint8_t *b = web_sysex_buf();
    uint32_t n = 0, i, k;
    b[n++] = 0xF0, b[n++] = 0x7D, b[n++] = 0x46, b[n++] = 0x4C, b[n++] = (uint8_t)cmd;
    for (i = 0; i < na; i++)
        b[n++] = a[i];
    b[n++] = 0xF7;
    web_sysex_out_clear();
    if (web_sysex(n) != 1u)
        return -1;
    render_ms(40);
    for (i = 0; i < web_sysex_out_n(); i = k + 1u) {
        const uint8_t *o = web_sysex_out() + i;
        for (k = i; k < web_sysex_out_n() && web_sysex_out()[k] != 0xF7u; k++)
            ;
        if (k - i >= 5u && o[4] == cmd) {
            uint32_t m = k - i - 5u < max ? k - i - 5u : max;
            memcpy(rep, o + 5, m);
            return (int)m;
        }
    }
    return -1;
}
static int sx_pushed(uint32_t cmd, uint32_t arg)   /* a push (cmd, its first byte) among what came out */
{
    uint32_t i;
    for (i = 0; i + 6u < web_sysex_out_n(); i++)
        if (web_sysex_out()[i] == 0xF0u && web_sysex_out()[i + 4] == cmd && web_sysex_out()[i + 5] == arg)
            return 1;
    return 0;
}

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
        CHECK(WS_NWAVES == 48u && WS_NSRC == 62u && str_eq(N_WS_SRC[47], "GRIT") && str_eq(N_WS_SRC[48], "S-CYC") &&
              str_eq(N_WS_SRC[57], "S-RND") && str_eq(N_WS_SRC[58], "SEQ1") && str_eq(N_WS_SRC[61], "SEQ4"),
              "the 48-wave bank, the 10 built-in sequences, the sound's SEQ1..SEQ4 (%u sources)", (unsigned)WS_NSRC);
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

    /* ---- the sound's own path and sequences (EDIT > PATH .. SEQ SET): a factory sound's copies, edited with the
     * pages' knobs, played, kept by a user preset (in flash: the store read back), the undo and the autosave */
    {
        static ws_sound_t mine, other;
        int32_t x0 = WS_PATHS[3].pt[0].x;
        uint32_t wv;
        apply_preset_to(t, 0);                           /* STORM PAD: A S-SHM, B S-CYC, C S-VOX, D S-PWM; PATH CIRC */
        CHECK(!memcmp(&ws_snd[0].seq[0], &WS_SEQS[4], sizeof(ws_lseq_t)) &&
              !memcmp(&ws_snd[0].seq[1], &WS_SEQS[0], sizeof(ws_lseq_t)) && ws_snd[0].path.n == WS_PATHS[3].n &&
              ws_snd[0].path.pt[0].x == x0, "a factory sound's own: copies of its corners' sequences and its path");
        wse.seq = 0, wse.lane = WL_WAVE, wse.step = 0, wse.point = 0;
        wv = WS_SEQS[4].lane[WL_WAVE].v[0] + 5u;
        wse_knob(GR_WSPATH, 1, -10);                     /* X of point 1: -20 % */
        wse_knob(GR_WSSEQ, 2, -2);                       /* WAVE's LEN 4 -> 2 */
        wse_knob(GR_WSSTEP, 1, 5);                       /* its step 1: five waves on */
        wse_knob(GR_WSSTEP, 2, -10);                     /* .. PROB 50 % */
        CHECK(t->p[P_E2] == WS_UPATH && ws_snd[0].path.pt[0].x == x0 - 3280 && ws_snd[0].seq[0].lane[WL_WAVE].len == 2 &&
              ws_snd[0].seq[0].lane[WL_WAVE].v[0] == wv && ws_snd[0].seq[0].lane[WL_WAVE].miss[0] == 50,
              "EDIT > PATH / SEQ / STEP: a point's X, a lane's LEN, a step's WAVE and PROB (PATH on USER)");
        t->p[P_E4] = (int16_t)(WS_NWAVES + WS_NBSEQ);   /* WAV A: SEQ1 */
        t->p[P_E0] = 0, t->p[P_E1] = 0;                  /* (A alone) */
        t->p[P_LEVEL] = TP[P_LEVEL].def;
        t->p[P_AMODE] = TP[P_AMODE].def;
        web_keys(1u << 12);
        render_ms(400);
        CHECK(peak > 0.01f, "SEQ1 on corner A sounds (peak %.3f)", (double)peak);
        web_keys(0);
        render_ms(3000);
        mine = ws_snd[0];
        CHECK(up_store(1, "MY SEQ") == 0 && up_used(1), "SAVE > USER: U02 with its path and sequences");
        apply_preset_to(t, 2);                           /* VOX PAD: its own copies replace them */
        other = ws_snd[0];
        undo.keep = 0;                                   /* (a new undo copy for the next load: not browsing on) */
        memset(ws_snd_bank, 0, sizeof ws_snd_bank);
        ws_snd_boot();                                   /* the store read back from flash */
        CHECK(memcmp(&other, &mine, sizeof mine) && up_load(1) == 0 && !memcmp(&ws_snd[0], &mine, sizeof mine) &&
              t->p[P_E4] == WS_NWAVES + WS_NBSEQ, "U02 (from flash) brings them back after another sound");
        undo_swap();
        CHECK(!memcmp(&ws_snd[0], &other, sizeof other), "SAVE held (undo): the sound before, with its own");
        undo_swap();
        ws_capture(&ws_state);
        memset(&ws_snd[0], 0, sizeof ws_snd[0]);
        ws_restore(&ws_state);
        CHECK(!memcmp(&ws_snd[0], &mine, sizeof mine), "the autosave keeps them");
        apply_preset_to(t, 0);
        render_ms(100);
    }

    /* ---- an edit on EDIT > PATH .. SEQ SET: SAVE held undoes it (one knob's turn one undo, the PATH it put on USER
     * back); a cursor knob, or a turn that changes nothing, takes no copy */
    {
        static ws_sound_t before, after, kept;
        int16_t path0;
        uint8_t what;
        apply_preset_to(t, 0);
        render_ms(100);
        before = ws_snd[0];
        path0 = t->p[P_E2];
        wse.seq = 0, wse.lane = WL_LEVEL, wse.step = 0, wse.point = 0;
        wse_knob(GR_WSSTEP, 1, -3);                      /* LEVEL of step 1, two detents of one turn */
        wse_knob(GR_WSSTEP, 1, -3);
        after = ws_snd[0];
        CHECK(after.seq[0].lane[WL_LEVEL].v[0] == before.seq[0].lane[WL_LEVEL].v[0] - 6, "EDIT > STEP: a LEVEL turned");
        undo_swap();
        CHECK(!memcmp(&ws_snd[0], &before, sizeof before), "SAVE held: the whole turn undone");
        undo_swap();
        CHECK(!memcmp(&ws_snd[0], &after, sizeof after), "held again: redone");
        wse_knob(GR_WSPATH, 2, 4);                       /* Y of point 1: PATH on USER */
        CHECK(t->p[P_E2] == WS_UPATH && path0 != WS_UPATH, "EDIT > PATH: an edit puts PATH on USER");
        undo_swap();
        CHECK(t->p[P_E2] == path0 && !memcmp(&ws_snd[0], &after, sizeof after),
              "SAVE held: the point and the PATH before (the LEVEL edit stays)");
        undo_swap();
        kept = undo.snd;
        what = undo.what;
        wse_knob(GR_WSSTEP, 0, 1);                       /* (the STEP cursor) */
        wse_knob(GR_WSSEQ, 1, 1);                        /* (the LANE cursor) */
        wse.lane = WL_LEVEL;
        ws_snd[0].seq[0].lane[WL_LEVEL].miss[0] = 0;
        wse.step = 0;
        wse_knob(GR_WSSTEP, 2, 3);                       /* PROB already 100 % */
        CHECK(!memcmp(&undo.snd, &kept, sizeof kept) && undo.what == what,
              "cursor knobs and a turn past the end take no undo copy");
    }

    /* ---- HELP: every hint fits its row with every word (gfx.c cv_key_row drops the last words of one too wide) */
    {
        const char *all[NELEM(HELP_PAGE) + NELEM(HELP_WS) + 1];
        uint32_t na = 0, h, worst = 0, ok_h = 1;
        for (h = 0; h < NELEM(HELP_PAGE); h++) all[na++] = HELP_PAGE[h].hint;
        for (h = 0; h < NELEM(HELP_WS); h++) all[na++] = HELP_WS[h];
        all[na++] = H_HOME;
        for (h = 0; h < na; h++) {
            khint_t kh[3];
            char b[48];
            uint32_t n = help_parse(all[h], kh, b, sizeof b), i;
            int32_t w = 6 * (int32_t)(n - 1u);
            for (i = 0; i < n; i++)
                w += kh_w(kh[i].key, kh[i].act);
            if (n == 0u || w > 224) {
                ok_h = 0;
                printf("      too wide (%d of 224): %s\n", w, all[h]);
            }
            worst = (uint32_t)w > worst ? (uint32_t)w : worst;
        }
        CHECK(ok_h, "HELP: every hint fits its row with every word (widest %u of 224 px)", worst);
    }

    /* ---- ALGORITHM: X of the part, on every page (recorded as KNOB 1 on HOME is) */
    {
        int16_t x0;
        t->p[P_E0] = 40;
        x0 = t->p[P_E0];
        web_enc(EN_ALGO, 3);
        render_ms(50);
        CHECK(t->p[P_E0] > x0 && song.sel == 0u, "ALGORITHM turns X (%d -> %d)", x0, t->p[P_E0]);
        x0 = t->p[P_E0];
        web_enc(EN_ALGO, -2);
        render_ms(50);
        CHECK(t->p[P_E0] < x0, "and back (%d)", t->p[P_E0]);
    }

    /* ---- user presets: WaveStorm's record version, all 64 steps (from flash), erase forgets them; Felucca's
     * records read as empty */
    {
        static step_t want[NSTEP];
        for (i = 0; i < NSTEP; i++)
            t->step[i].time = ST_REST, t->step[i].n = 0;
        t->step[40].time = ST_NOTE, t->step[40].n = 2, t->step[40].note[0] = 60, t->step[40].note[1] = 67;
        t->step[40].vel = 99;
        t->step[63].time = ST_NOTE, t->step[63].n = 1, t->step[63].note[0] = 48;
        t->p[P_SLEN] = 64;
        memcpy(want, t->step, sizeof want);
        CHECK(up_store(4, "LONG ONE") == 0 && up_rec(4)->ver == UP_VER_WS && up_has_pat(4),
              "U05 stored as WaveStorm's version, its pattern after step 16 counts");
        for (i = 0; i < NSTEP; i++)
            t->step[i].time = ST_REST, t->step[i].n = 0;
        t->p[P_SLEN] = 16;
        memset(ws_snd_bank, 0, sizeof ws_snd_bank);
        ws_snd_boot();                                   /* (read back from flash) */
        undo.keep = 0;
        CHECK(up_load(4) == 0 && !memcmp(t->step, want, sizeof want) && t->p[P_SLEN] == 64,
              "U05 brings back all 64 steps (a chord on step 41, LEN 64)");
        CHECK(up_put(4, 0) == 0 && !ws_snd_steps(4) && !up_used(4), "erasing U05 forgets its steps");
        {
            up_rec_t f = *up_rec(0);                     /* a Felucca record (version 4, engine 0: ANALOG) */
            f.ver = 4;
            *up_rec(5) = f;
            CHECK(up_used(0) && !up_used(5), "a Felucca record in a slot reads as empty");
            memset(up_rec(5), 0, sizeof f);
        }
    }

    /* ---- the editor protocol (web/wavestorm-editor.html): INFO's WaveStorm tag, the path and a sequence read and
     * written (clamped to the pages' ranges, the undo copy, PATH on USER), a device edit pushed while watched */
    {
        static uint8_t rep[700], a[200];
        int n, k, tag = 0;
        apply_preset_to(t, 0);
        render_ms(50);
        n = sx_req(1, 0, 0, rep, sizeof rep);
        for (k = 0; n > 0 && k + 8 < n; k++)
            if (rep[k] == 0x58 && rep[k + 1] == 1 && rep[k + 2] == WS_NUSEQ && rep[k + 3] == WS_UPTS &&
                rep[k + 4] == WL_N && rep[k + 5] == WS_LSTEPS && rep[k + 6] == WS_NWAVES && rep[k + 8] == WS_UPATH)
                tag = 1;
        CHECK(n > 10 && !memcmp(rep, "WAVESTORM ", 10) && tag, "editor INFO: WAVESTORM, the tag 58 (sequences, points)");
        n = sx_req(84, (const uint8_t[]){1}, 1, rep, sizeof rep);
        CHECK(n == 2 + (int)ED_WS_SEQ_N && rep[0] == 1 && rep[1] == 0 && rep[2] == ws_snd[0].seq[0].mode &&
              rep[4 + 34] == ws_snd[0].seq[0].lane[WL_WAVE].len, "WS_GET 1: SEQ1 as the part has it (%d bytes)", n);
        memcpy(a + 1, rep + 2, ED_WS_SEQ_N);
        a[0] = 1;
        a[1 + 2 + 34] = 3;                               /* WAVE: LEN 3 */
        a[1 + 2 + 34 + 2] = 100;                         /* its step 1: wave 100 (past the last: kept as the last) */
        n = sx_req(85, a, 1 + ED_WS_SEQ_N, rep, sizeof rep);
        CHECK(n > 0 && rep[1] == 0 && ws_snd[0].seq[0].lane[WL_WAVE].len == 3 &&
              ws_snd[0].seq[0].lane[WL_WAVE].v[0] == WS_NWAVES - 1 && rep[2 + 2 + 34 + 2] == WS_NWAVES - 1,
              "WS_SET 1: SEQ1's WAVE lane, a value past the waves kept as the last (reply too)");
        undo_swap();
        CHECK(ws_snd[0].seq[0].lane[WL_WAVE].len != 3, "SAVE held undoes the editor's set");
        undo_swap();
        n = sx_req(84, (const uint8_t[]){0}, 1, rep, sizeof rep);
        memcpy(a + 1, rep + 2, ED_WS_PATH_N);
        a[0] = 0;
        a[1] = 2;                                        /* two points */
        a[2] = 127;                                      /* no loop */
        a[3] = 0, a[4] = 0, a[5] = 0;                    /* point 1: X -100 % */
        n = sx_req(85, a, 1 + ED_WS_PATH_N, rep, sizeof rep);
        CHECK(n > 0 && rep[1] == 0 && ws_snd[0].path.n == 2 && ws_snd[0].path.loop == 0xFFu &&
              ws_snd[0].path.pt[0].x == -16384 && t->p[P_E2] == WS_UPATH, "WS_SET 0: the path, PATH on USER");
        n = sx_req(85, a, 10, rep, sizeof rep);
        CHECK(n >= 2 && rep[1] == 1, "WS_SET with the wrong length: rc 1, nothing changed");
        n = sx_req(84, (const uint8_t[]){5}, 1, rep, sizeof rep);
        CHECK(n > 3 && rep[2] == WS_NSRC && !strcmp((const char *)rep + 3, N_WS_SRC[0]), "WS_GET 5: the sources' names");
        sx_req(22, (const uint8_t[]){1}, 1, rep, sizeof rep);   /* WATCH on */
        wse.seq = 2, wse.lane = WL_LEVEL, wse.step = 0;
        wse_knob(GR_WSSTEP, 1, -4);                      /* SEQ3 edited on the device */
        web_sysex_out_clear();
        render_ms(60);
        CHECK(sx_pushed(86, 3), "a SEQ3 edit on the device: WS_CHANGED 3 pushed");
        sx_req(22, (const uint8_t[]){0}, 1, rep, sizeof rep);
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
        CHECK(motion_param(P_CUT) && motion_param(P_RES) && motion_param(P_FKEY) && !motion_param(P_FTYPE) &&
              motion_param(P_E0) && motion_param(P_E1), "the filter's CUT RES KEY (not TYPE), X and Y can be automated and locked");
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
